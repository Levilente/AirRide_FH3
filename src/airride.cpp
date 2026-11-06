// Airride for Forza Horizon 3 (FH3 Mod Platform mod).
//
// Garage > Upgrades > Platform and Handling > Airride (Stock / Airride). A car with the Airride can be lowered while it
// stands still: hold the left stick down for a second and the car sinks smoothly (ease in, ease out) by the amount set on
// the car's Tuning > Springs page ("Airride drop", 1 to 10 cm), with 0.3 degrees of extra negative camber per cm so the
// wheels fit. Hold the stick again, or drive off, and the car rises back to the height it was lowered from.
//
// How the car is moved (read from the game's code; the log shows every step):
//  - Car::Update (game+0x18B8D20, once a frame on the game's thread) is hooked. The HUD controller (game+0xB26690) knows
//    which Car is the player's.
//  - A Car holds its physics spec at Car+0x140 and the car dynamics' interface at Car+0x71E8. The game re-applies a changed
//    spec to the live physics with interface slot 80 (offset 0x280; the body loader does it at game+0x18BADFE after it has
//    rewritten the track width). The conversion (game+0x1D69BA0) takes the two suspension definitions in the spec
//    (front spec+0x378, rear spec+0x420: MinRideHeight +0x10, MaxRideHeight +0x14, metres), the tuning values (46 floats
//    0..1 from spec+0x6E80: front ride height +0x6EC8, rear +0x6EF4, camber +0x6EB4 front, +0x6EE0 rear) and the camber range
//    (TuneCamberMin / Max, degrees, game+0x4FA1BBC / +0x4FA1BC0).
//  - Lowering: every few frames this mod writes new values into the spec and calls slot 80. Height mode 0 shifts MinRideHeight
//    and MaxRideHeight of both axles down by the eased drop, mode 1 lowers the normalised ride-height tuning value instead.
#include "fh3mod.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

constexpr int kStages            = 2;      // Stock, Airride
constexpr size_t kOffSpec        = 0x140;  // Car -> physics spec
constexpr size_t kOffIface       = 0x71E8; // Car -> car dynamics interface (CCarDynamics + 0x4B0)
constexpr size_t kOffModelInfo   = 0x110;  // Car -> record whose +0x30 is the car's model id
constexpr size_t kOffModelId     = 0x30;
constexpr size_t kIfaceToDyn     = 0x4B0;
constexpr size_t kDynVelocity    = 0x10;   // CCarDynamics: velocity (3 floats)
constexpr size_t kSlotSetSpec    = 0x280;  // interface vtable: re-apply the spec
constexpr size_t kSuspFront      = 0x378;  // spec: front suspension definition
constexpr size_t kSuspRear       = 0x420;  // spec: rear suspension definition
constexpr size_t kSuspMinRide    = 0x10;
constexpr size_t kSuspMaxRide    = 0x14;
constexpr size_t kTuneRideFront  = 0x6EC8; // spec: normalised tuning values
constexpr size_t kTuneRideRear   = 0x6EF4;
constexpr size_t kTuneCamberFront = 0x6EB4;
constexpr size_t kTuneCamberRear  = 0x6EE0;
constexpr uintptr_t kRvaCarUpdate  = 0x18B8D20;
constexpr uintptr_t kRvaHudCtrl    = 0xB26690;
constexpr uintptr_t kRvaCamberMin  = 0x4FA1BBC;
constexpr uintptr_t kRvaCamberMax  = 0x4FA1BC0;

const FH3ModApi* g_api = nullptr;
FH3Mod* g_mod          = nullptr;
int g_handle           = -1;
char* g_game           = nullptr;

// fh3-mods\Airride.ini (written with these defaults on first start).
int g_hold_ms         = 1000;   // how long the left stick is held to lower / raise
int g_default_drop    = 5;      // cm, for a car whose slider was never touched
float g_camber_per_cm = 0.3f;   // degrees of negative camber per cm of drop
float g_duration      = 1.4f;   // seconds a lowering or raising takes
int g_step_ms         = 33;     // the spec is re-applied at most this often while moving
float g_still_mps     = 0.5f;   // the car counts as standing still below this speed
float g_raise_mps     = 2.0f;   // a lowered car that moves faster than this is raised again
int g_height_mode     = 0;      // 0 shifts the suspension's ride height range, 1 lowers the ride-height tuning value
float g_below_min_cm  = 1.5f;  // how far under the game's own lowest ride height (the tuning slider at 0) the car may be pushed
bool g_camber_on      = true;
int g_price           = 3000;
bool g_verbose        = true;
bool g_probe          = true;   // log the render objects found from the Car on every button hold (for the visual-only lowering)

std::wstring g_cfg_path;

void say(const char* fmt, ...) {
    char line[600];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (g_api && g_mod) g_api->log(g_mod, line);
}

// ---- config ---------------------------------------------------------------------------------------------------------
float cfg_float(const wchar_t* key, float def) {
    wchar_t buf[64] = L"";
    GetPrivateProfileStringW(L"Airride", key, L"", buf, 64, g_cfg_path.c_str());
    if (buf[0]) return static_cast<float>(std::wcstod(buf, nullptr));
    wchar_t v[48];
    _snwprintf(v, 48, L"%g", static_cast<double>(def));
    WritePrivateProfileStringW(L"Airride", key, v, g_cfg_path.c_str());
    return def;
}
int cfg_int(const wchar_t* key, int def) { return static_cast<int>(std::lround(cfg_float(key, static_cast<float>(def)))); }

void load_config() {
    g_hold_ms        = cfg_int(L"HoldMs", g_hold_ms);
    g_default_drop   = cfg_int(L"DefaultDropCm", g_default_drop);
    g_camber_per_cm  = cfg_float(L"CamberPerCm", g_camber_per_cm);
    g_duration       = cfg_float(L"DurationSeconds", g_duration);
    g_step_ms        = cfg_int(L"StepMs", g_step_ms);
    g_still_mps      = cfg_float(L"StandstillMps", g_still_mps);
    g_raise_mps      = cfg_float(L"AutoRaiseMps", g_raise_mps);
    g_height_mode    = cfg_int(L"HeightMode", g_height_mode);
    g_below_min_cm   = cfg_float(L"BelowMinCm", g_below_min_cm);
    g_camber_on      = cfg_int(L"Camber", 1) != 0;
    g_price          = cfg_int(L"Price", g_price);
    g_verbose        = cfg_int(L"Log", 1) != 0;
    g_probe          = cfg_int(L"Probe", 1) != 0;
    if (g_default_drop < 1) g_default_drop = 1;
    if (g_default_drop > 10) g_default_drop = 10;
    if (g_hold_ms < 200) g_hold_ms = 200;
    if (g_duration < 0.2f) g_duration = 0.2f;
    if (g_step_ms < 8) g_step_ms = 8;
}

// ---- the drop in cm, per garage car (Airride.ini [Drop]) ----------------------------------------------------------------
int drop_for(uint32_t garage) {
    wchar_t key[24];
    _snwprintf(key, 24, L"%u", garage);
    const int v = static_cast<int>(GetPrivateProfileIntW(L"Drop", key, g_default_drop, g_cfg_path.c_str()));
    return v < 1 ? 1 : v > 10 ? 10 : v;
}
void set_drop(uint32_t garage, int cm) {
    wchar_t key[24], val[16];
    _snwprintf(key, 24, L"%u", garage);
    _snwprintf(val, 16, L"%d", cm);
    WritePrivateProfileStringW(L"Drop", key, val, g_cfg_path.c_str());
}

// ---- the tuning menu row: Springs > Airride drop (cm) -----------------------------------------------------------------------
bool car_has_airride() { return g_handle >= 0 && g_api->upgrade_stage(g_mod, g_handle, 0) > 0; }

float row_get(void*) { return static_cast<float>(drop_for(g_api->upgrade_active_garage())); }
void row_set(void*, float v) {
    const uint32_t garage = g_api->upgrade_active_garage();
    const int cm          = static_cast<int>(std::lround(v));
    set_drop(garage, cm);
    say("tuning: Airride drop for garage car %u set to %d cm", garage, cm);
}
int row_visible(void*) { return car_has_airride() ? 1 : 0; }

// ---- state of the player's car --------------------------------------------------------------------------------------------
std::atomic<int> g_request{0};              // the stick was held: toggle (set by the input thread)
std::atomic<void*> g_hud_player{nullptr};   // the HUD's pointer to the player's Car
std::atomic<ULONGLONG> g_hud_tick{0};       // the HUD controller ran (driving with the HUD up)

void* g_car          = nullptr;             // the Car the state below belongs to
bool g_have_base     = false;
int g_target         = 0;                   // 1 lowered / lowering, 0 raised / raising
float g_p            = 0;                   // progress 0 (raised) .. 1 (lowered), linear in time
ULONGLONG g_last_step = 0;
float g_last_applied_p = -1;
// the spec values the lowering started from
float g_base_min[2], g_base_max[2], g_base_tune_ride[2], g_base_camber[2];
float g_drop_m = 0, g_cam_range = 0, g_room_cm = 0;        // metres, camber tuning range in degrees
float g_written[8];                          // what the last apply wrote (to notice that the game rebuilt the spec)
int g_garage_lowered = 0;

bool readable(const void* p, size_t n) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!p || VirtualQuery(p, &mbi, sizeof mbi) != sizeof mbi || mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    return static_cast<const char*>(p) + n <= static_cast<const char*>(mbi.BaseAddress) + mbi.RegionSize;
}

float* fp(char* spec, size_t off) { return reinterpret_cast<float*>(spec + off); }

int car_model(const char* car) {
    __try {
        const char* info = *reinterpret_cast<char* const*>(car + kOffModelInfo);
        return *reinterpret_cast<const int*>(info + kOffModelId);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

float car_speed(char* car) {
    __try {
        char* iface = *reinterpret_cast<char**>(car + kOffIface);
        if (!iface) return 1e9f;
        const float* v = reinterpret_cast<const float*>(iface - kIfaceToDyn + kDynVelocity);
        const float s  = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        return std::isfinite(s) ? s : 1e9f;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 1e9f;
    }
}

// Re-applies the spec to the live physics the way the game's body loader does.
bool reapply(char* car) {
    __try {
        void* iface = *reinterpret_cast<void**>(car + kOffIface);
        if (!iface) return false;
        void** vt = *reinterpret_cast<void***>(iface);
        typedef void (*SetSpecFn)(void*, void*);
        reinterpret_cast<SetSpecFn>(vt[kSlotSetSpec / 8])(iface, car + kOffSpec);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}


// ---- probe: finds the car's render model objects (CarRenderModel11, DriverModel) and logs their 4x4 matrices -------------------
// Visual-only lowering needs the places where the game stores the body / driver / steering wheel / wheel transforms for drawing.
// This walks the objects reachable from the Car and logs what it finds, so the offsets can be read from the log.
constexpr uintptr_t kVtCarRender1 = 0x4675B40, kVtCarRender2 = 0x4675B50, kVtDriver = 0x468E728;


bool looks_matrix(const float* m) {   // 3 unit rows (rotation) + a position
    for (int r = 0; r < 3; ++r) {
        const float l = m[r * 4] * m[r * 4] + m[r * 4 + 1] * m[r * 4 + 1] + m[r * 4 + 2] * m[r * 4 + 2];
        if (!(l > 0.95f && l < 1.05f)) return false;
    }
    for (int k = 12; k < 15; ++k) if (!std::isfinite(m[k]) || std::fabs(m[k]) > 1e6f) return false;
    return true;
}

void probe_object(const char* tag, const char* obj, size_t span) {
    say("probe: %s object %p", tag, static_cast<const void*>(obj));
    for (size_t off = 0; off + 64 <= span; off += 16) {
        if (!readable(obj + off, 64)) break;
        const float* m = reinterpret_cast<const float*>(obj + off);
        if (looks_matrix(m)) {
            say("probe:   %s +0x%zX matrix pos (%.3f, %.3f, %.3f) row0 (%.2f %.2f %.2f)", tag, off, static_cast<double>(m[12]),
                static_cast<double>(m[13]), static_cast<double>(m[14]), static_cast<double>(m[0]), static_cast<double>(m[1]), static_cast<double>(m[2]));
        }
    }
}

void probe_scan(char* car) {
    constexpr size_t kMax = 6000;
    struct Item { const char* p; int depth; };
    static Item queue[kMax];
    static const char* seen[1 << 14];
    std::memset(seen, 0, sizeof seen);
    size_t head = 0, tail = 0, found = 0;
    const ULONGLONG t0 = GetTickCount64();
    auto mark = [&](const char* q) {   // true if new
        size_t h = (reinterpret_cast<uintptr_t>(q) >> 3) * 0x9E3779B1u & ((1 << 14) - 1);
        while (seen[h]) { if (seen[h] == q) return false; h = (h + 1) & ((1 << 14) - 1); }
        seen[h] = q;
        return true;
    };
    queue[tail++] = {car, 0};
    mark(car);
    say("probe: scanning from the Car %p", static_cast<void*>(car));
    while (head < tail && found < 12 && GetTickCount64() - t0 < 4000) {
        const Item it = queue[head++];
        __try {
            if (!readable(it.p, 0x40)) continue;
            const uintptr_t vt = *reinterpret_cast<const uintptr_t*>(it.p) - reinterpret_cast<uintptr_t>(g_game);
            if (vt == kVtCarRender1 || vt == kVtCarRender2 || vt == kVtDriver) {
                const char* tag = vt == kVtDriver ? "DriverModel" : "CarRenderModel";
                say("probe: found %s at %p (depth %d)", tag, static_cast<const void*>(it.p), it.depth);
                probe_object(tag, it.p, 0x500);
                ++found;
                continue;
            }
            if (it.depth >= 4) continue;
            // one readable() check per object, then plain reads inside __try
            const size_t span = readable(it.p, 0x400) ? 0x400 : 0x40;
            for (size_t off = 8; off < span && tail < kMax; off += 8) {
                const char* q = *reinterpret_cast<const char* const*>(it.p + off);
                const uintptr_t v = reinterpret_cast<uintptr_t>(q);
                if (v < 0x10000 || v > 0x00007FFFFFFFFFFFull || (v & 7)) continue;
                if (mark(q)) queue[tail++] = {q, it.depth + 1};
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    say("probe: done, %zu objects visited in %llu ms, %zu render objects found", head, static_cast<unsigned long long>(GetTickCount64() - t0), found);
}

std::atomic<char*> g_probe_car{nullptr};
DWORD WINAPI probe_thread(LPVOID) {
    char* car = g_probe_car.load();
    if (car) probe_scan(car);
    g_probe_car.store(nullptr);
    return 0;
}
void probe_start(char* car) {
    char* expected = nullptr;
    if (!g_probe_car.compare_exchange_strong(expected, car)) return;
    if (HANDLE t = CreateThread(nullptr, 0, probe_thread, nullptr, 0, nullptr)) CloseHandle(t);
    else g_probe_car.store(nullptr);
}

float ease(float x) {   // smoothstep: ease in, ease out
    x = x < 0 ? 0 : x > 1 ? 1 : x;
    return x * x * (3.0f - 2.0f * x);
}

// Reads the spec values to start from; false (and a log line) if they don't look like the layout this was written for.
bool capture_base(char* spec, int drop_cm) {
    __try {
        for (int a = 0; a < 2; ++a) {
            char* s            = spec + (a ? kSuspRear : kSuspFront);
            g_base_min[a]      = *fp(s, kSuspMinRide);
            g_base_max[a]      = *fp(s, kSuspMaxRide);
            g_base_tune_ride[a] = *fp(spec, a ? kTuneRideRear : kTuneRideFront);
            g_base_camber[a]   = *fp(spec, a ? kTuneCamberRear : kTuneCamberFront);
            if (!std::isfinite(g_base_min[a]) || !std::isfinite(g_base_max[a]) || g_base_min[a] < 0 || g_base_max[a] > 2.0f ||
                g_base_max[a] <= g_base_min[a]) {
                say("airride: the %s suspension's ride heights look wrong (min %g, max %g): not lowering", a ? "rear" : "front",
                    static_cast<double>(g_base_min[a]), static_cast<double>(g_base_max[a]));
                return false;
            }
        }
        // The suspension can't be pushed far under the lowest ride height the game itself allows (tuning slider at 0): the car
        // would sink into the ground. Allowed drop = room left above that lowest height + BelowMinCm, the smaller of the axles.
        float room = 1e9f;
        for (int a = 0; a < 2; ++a) {
            const float t   = g_base_tune_ride[a] < 0 ? 0 : g_base_tune_ride[a] > 1 ? 1 : g_base_tune_ride[a];
            const float r   = (g_base_max[a] - g_base_min[a]) * t;   // above the lowest height
            room            = r < room ? r : room;
        }
        g_room_cm = room * 100.0f + g_below_min_cm;
        if (g_room_cm < 0) g_room_cm = 0;
        const float cmin = *reinterpret_cast<const float*>(g_game + kRvaCamberMin);
        const float cmax = *reinterpret_cast<const float*>(g_game + kRvaCamberMax);
        g_cam_range      = cmax - cmin;
        if (g_camber_on && (!std::isfinite(g_cam_range) || g_cam_range < 1.0f || g_cam_range > 40.0f)) {
            say("airride: the camber tuning range looks wrong (%g to %g degrees): no extra camber", static_cast<double>(cmin), static_cast<double>(cmax));
            g_cam_range = 0;
        }
        say("airride: start values: front ride %.4f..%.4f m, rear %.4f..%.4f m, ride tuning %.3f / %.3f, camber tuning %.3f / %.3f (range %.2f deg), drop %d cm",
            static_cast<double>(g_base_min[0]), static_cast<double>(g_base_max[0]), static_cast<double>(g_base_min[1]),
            static_cast<double>(g_base_max[1]), static_cast<double>(g_base_tune_ride[0]), static_cast<double>(g_base_tune_ride[1]),
            static_cast<double>(g_base_camber[0]), static_cast<double>(g_base_camber[1]), static_cast<double>(g_cam_range), drop_cm);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        say("airride: couldn't read the car's spec");
        return false;
    }
}

// Writes the spec for eased progress e (0 = as it was, 1 = fully lowered) and re-applies it.
bool apply(char* car, float e) {
    char* spec = car + kOffSpec;
    __try {
        for (int a = 0; a < 2; ++a) {
            char* s = spec + (a ? kSuspRear : kSuspFront);
            if (g_height_mode == 0) {
                *fp(s, kSuspMinRide) = g_base_min[a] - g_drop_m * e;
                *fp(s, kSuspMaxRide) = g_base_max[a] - g_drop_m * e;
                g_written[a * 2]     = *fp(s, kSuspMinRide);
                g_written[a * 2 + 1] = *fp(s, kSuspMaxRide);
            } else {
                *fp(spec, a ? kTuneRideRear : kTuneRideFront) = g_base_tune_ride[a] - g_drop_m * e / (g_base_max[a] - g_base_min[a]);
                g_written[a * 2]     = *fp(spec, a ? kTuneRideRear : kTuneRideFront);
                g_written[a * 2 + 1] = 0;
            }
            if (g_camber_on && g_cam_range > 0) {
                *fp(spec, a ? kTuneCamberRear : kTuneCamberFront) = g_base_camber[a] - (g_camber_per_cm * g_drop_m * 100.0f * e) / g_cam_range;
                g_written[4 + a] = *fp(spec, a ? kTuneCamberRear : kTuneCamberFront);
            } else {
                g_written[4 + a] = *fp(spec, a ? kTuneCamberRear : kTuneCamberFront);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return reapply(car);
}

// True if the spec still holds what the last apply wrote (the game didn't rebuild it).
bool spec_intact(char* spec) {
    __try {
        for (int a = 0; a < 2; ++a) {
            char* s = spec + (a ? kSuspRear : kSuspFront);
            if (g_height_mode == 0) {
                if (std::fabs(*fp(s, kSuspMinRide) - g_written[a * 2]) > 1e-5f || std::fabs(*fp(s, kSuspMaxRide) - g_written[a * 2 + 1]) > 1e-5f) return false;
            } else if (std::fabs(*fp(spec, a ? kTuneRideRear : kTuneRideFront) - g_written[a * 2]) > 1e-5f) {
                return false;
            }
            if (std::fabs(*fp(spec, a ? kTuneCamberRear : kTuneCamberFront) - g_written[4 + a]) > 1e-5f) return false;
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void forget() {
    g_have_base      = false;
    g_target         = 0;
    g_p              = 0;
    g_last_applied_p = -1;
}

// The player's Car, once a frame.
void step(char* car, float dt) {
    char* spec = car + kOffSpec;
    if (car != g_car) {
        if (g_car && g_have_base) say("airride: the player's Car changed (%p -> %p): state dropped", g_car, static_cast<void*>(car));
        forget();
        g_car = car;
    }
    if (g_have_base && !spec_intact(spec)) {
        say("airride: the game rebuilt the car's spec; the car is back at its normal height");
        forget();
    }

    const uint32_t garage = g_api->upgrade_active_garage();
    const bool owned      = car_has_airride();
    bool foreign          = false;
    if (g_api->api_version >= 12 && g_api->upgrade_car_model && garage) {
        const int have = car_model(car), want = g_api->upgrade_car_model(static_cast<int32_t>(garage));
        auto model     = [](int v) { return v >= 100000 ? v / 1000 : v; };
        foreign        = have > 0 && want > 0 && model(have) != model(want);
    }
    const bool usable = owned && !foreign;
    const float speed = car_speed(car);
    const bool driving = GetTickCount64() - g_hud_tick.load() < 600;

    const int req = g_request.exchange(0);
    if (req && g_probe) probe_start(car);
    if (req) {
        if (g_target == 1 || g_p > 0) {
            g_target = 0;
            say("airride: raising");
        } else if (!usable) {
            say("airride: stick held, but this car has no Airride (or the garage car is another model)");
        } else if (!driving) {
            say("airride: stick held, but the HUD isn't up (menu or photo mode)");
        } else if (speed > g_still_mps) {
            say("airride: stick held, but the car isn't standing still (%.2f m/s)", static_cast<double>(speed));
        } else {
            const int cm = drop_for(garage);
            if (capture_base(spec, cm)) {
                const float eff = static_cast<float>(cm) < g_room_cm ? static_cast<float>(cm) : g_room_cm;
                if (eff < static_cast<float>(cm))
                    say("airride: %d cm asked, but this car is already low: only %.1f cm of room above the lowest safe height", cm, static_cast<double>(eff));
                g_have_base = true;
                g_drop_m    = eff * 0.01f;
                g_garage_lowered = static_cast<int>(garage);
                g_target    = 1;
                say("airride: lowering %.1f cm", static_cast<double>(eff));
            }
        }
    }
    if (g_target == 1 && (!usable || speed > g_raise_mps)) {
        g_target = 0;
        say("airride: %s, raising", !usable ? "the car lost its Airride" : "the car is moving");
    }
    if (!g_have_base) return;

    const float dir = g_target ? 1.0f : -1.0f;
    float np        = g_p + dir * (dt > 0.1f ? 0.1f : dt < 0 ? 0 : dt) / g_duration;
    np              = np < 0 ? 0 : np > 1 ? 1 : np;
    const bool settled = (np == 0 && g_target == 0) || (np == 1 && g_target == 1);
    g_p = np;

    const ULONGLONG now = GetTickCount64();
    const bool due      = now - g_last_step >= static_cast<ULONGLONG>(g_step_ms);
    if (g_p != g_last_applied_p && (due || settled)) {
        g_last_step      = now;
        g_last_applied_p = g_p;
        if (!apply(car, ease(g_p))) {
            say("airride: couldn't apply the spec (a fault); giving up on this lowering");
            forget();
            return;
        }
        if (settled) {
            say("airride: %s", g_target ? "lowered" : "back at normal height");
            if (!g_target) forget();
        }
    }
}

// ---- hooks -------------------------------------------------------------------------------------------------------------
typedef uint64_t (*CarUpdateFn)(void*, float);
CarUpdateFn g_orig_car_update = nullptr;
uint64_t detour_car_update(void* self, float dt) {
    const uint64_t r = g_orig_car_update(self, dt);
    char* car        = static_cast<char*>(self);
    void* hud        = g_hud_player.load();
    if (!car || !hud) return r;
    __try {
        void* iface = *reinterpret_cast<void**>(car + kOffIface);
        if (static_cast<void*>(car) == hud || (iface && iface == hud)) step(car, dt);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        static int logged = 0;
        if (logged++ < 5) say("airride: a fault in the Car update step");
    }
    return r;
}

typedef uint64_t (*HudCtrlFn)(void*, void*, void*, void*);
HudCtrlFn g_orig_hud = nullptr;
uint64_t detour_hud(void* c, void* a, void* b, void* d) {
    const uint64_t r = g_orig_hud(c, a, b, d);
    if (c && readable(c, 0x130)) {
        void* obj = *reinterpret_cast<void**>(static_cast<char*>(c) + 0x120);
        void* cb  = *reinterpret_cast<void**>(static_cast<char*>(c) + 0x128);
        g_hud_player.store(cb ? obj : nullptr);
        g_hud_tick.store(GetTickCount64());
    }
    return r;
}

DWORD WINAPI install_hooks(LPVOID) {
    g_game = static_cast<char*>(g_api->wait_for_code(0xFFFFFFFFu));
    if (!g_game) { say("the game's code never became ready: no hooks"); return 0; }
    FH3Hook* h1 = g_api->hook_function(g_mod, "Car update (Airride)", g_game + kRvaCarUpdate, reinterpret_cast<void*>(&detour_car_update),
                                       reinterpret_cast<void**>(&g_orig_car_update));
    FH3Hook* h2 = g_api->hook_function(g_mod, "HUD controller (Airride: the player's car)", g_game + kRvaHudCtrl, reinterpret_cast<void*>(&detour_hud),
                                       reinterpret_cast<void**>(&g_orig_hud));
    const bool a = h1 && g_api->enable_hooks(g_mod, h1);
    const bool b = h2 && g_api->enable_hooks(g_mod, h2);
    say("hooks: Car update %s, HUD controller %s", a ? "on" : "FAILED", b ? "on" : "FAILED");
    return 0;
}

// ---- input: the left stick held down ---------------------------------------------------------------------------------------
typedef DWORD(WINAPI* XInputGetStateFn)(DWORD, void*);
struct PadState {
    DWORD packet;
    WORD buttons;
    BYTE lt, rt;
    SHORT lx, ly, rx, ry;
};

bool game_in_front() {
    HWND w = GetForegroundWindow();
    DWORD pid = 0;
    if (w) GetWindowThreadProcessId(w, &pid);
    return pid == GetCurrentProcessId();
}

DWORD WINAPI input_thread(LPVOID) {
    XInputGetStateFn get = nullptr;
    for (const wchar_t* n : {L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll"}) {
        if (HMODULE m = LoadLibraryW(n)) {
            get = reinterpret_cast<XInputGetStateFn>(reinterpret_cast<void*>(GetProcAddress(m, "XInputGetState")));
            if (get) break;
        }
    }
    if (!get) { say("XInput couldn't be loaded: the left stick can't lower the car"); return 0; }
    ULONGLONG held_since = 0;
    bool fired           = false;
    for (;;) {
        Sleep(16);
        bool down = false;
        if (game_in_front()) {
            for (DWORD i = 0; i < 4 && !down; ++i) {
                PadState st{};
                if (get(i, &st) == ERROR_SUCCESS && (st.buttons & 0x0040)) down = true;   // XINPUT_GAMEPAD_LEFT_THUMB
            }
        }
        if (!down) { held_since = 0; fired = false; continue; }
        const ULONGLONG now = GetTickCount64();
        if (!held_since) held_since = now;
        if (!fired && now - held_since >= static_cast<ULONGLONG>(g_hold_ms)) {
            fired = true;
            g_request.store(1);
        }
    }
}

int add_upgrade() {
    static const wchar_t* names[kStages] = {L"Stock", L"Airride"};
    static const wchar_t* descs[kStages] = {L"The standard suspension: no air springs.",
                                            L"Air springs: hold the left stick while standing still to lower the car, again (or drive off) to raise it. "
                                            L"The drop is set on the car's tuning, Springs page."};
    static FH3UpgradeStage stages[kStages];
    for (int i = 0; i < kStages; ++i) stages[i] = {names[i], descs[i], i ? g_price : 0, nullptr, nullptr};
    FH3UpgradeDef d{};
    d.size           = sizeof d;
    d.id             = "airride.kit";
    d.name           = L"Airride";
    d.description    = L"Air suspension: lower the car while it stands still, raise it again to drive.";
    d.area           = 2;   // Platform and handling
    d.stage_count    = kStages;
    d.stages         = stages;
    d.preferred_slot = -1;
    return g_api->register_upgrade(g_mod, &d);
}

}  // namespace

FH3MOD_EXPORT int fh3mod_init(const FH3ModApi* api) {
    if (api->api_version < 11) return 1;
    g_api = api;

    FH3ModInfo info{};
    info.size             = sizeof info;
    info.name             = "Airride";
    info.version          = AIRRIDE_VERSION;
    info.github_repo      = nullptr;
    info.platform_version = FH3MOD_PLATFORM_VERSION;
    info.check_updates    = 0;
    info.description      = "Air suspension for every car: lower it while standing still (hold the left stick), set the drop on the Springs tuning page.";
    info.offline_only     = 1;
    g_mod                 = api->register_mod(&info);
    if (!g_mod) return 1;

    g_cfg_path = std::wstring(api->azimuth_dir) + L"\\fh3-mods\\Airride.ini";
    load_config();

    g_handle = add_upgrade();
    say("registered: upgrade handle %d (price %d, hold %d ms, height mode %d, camber %.2f deg/cm)", g_handle, g_price, g_hold_ms, g_height_mode,
        static_cast<double>(g_camber_per_cm));
    if (g_handle < 0) return 1;

    if (api->api_version >= 13 && api->add_tuning_row) {
        FH3TuningRow row{};
        row.size    = sizeof row;
        row.key     = "Drop";
        row.label   = "Airride drop (cm)";
        row.help    = "How far the Airride lowers the car, in centimetres (1 to 10). Each centimetre also adds 0.3 degrees of negative camber so the wheels fit.";
        row.min     = 1;
        row.max     = 10;
        row.step    = 1;
        row.decimals = 0;
        row.get     = row_get;
        row.set     = row_set;
        row.visible = row_visible;
        row.user    = nullptr;
        say("tuning row: %s", api->add_tuning_row(g_mod, &row) ? "added" : "NOT added (see the platform's lines above)");
    } else {
        say("this platform (API %u) can't add the tuning slider (needs API 13); the drop stays at %d cm", api->api_version, g_default_drop);
    }

    if (HANDLE t = CreateThread(nullptr, 0, install_hooks, nullptr, 0, nullptr)) CloseHandle(t);
    if (HANDLE t = CreateThread(nullptr, 0, input_thread, nullptr, 0, nullptr)) CloseHandle(t);
    return 0;
}
