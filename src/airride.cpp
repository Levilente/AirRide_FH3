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
bool g_menu_diag = false;
float g_camber_per_cm = 0.65f;   // degrees of negative camber per cm of drop
float g_duration      = 1.4f;   // seconds a lowering or raising takes
int g_step_ms         = 33;     // the spec is re-applied at most this often while moving
float g_still_mps     = 0.5f;   // the car counts as standing still below this speed
float g_raise_mps     = 2.0f;   // a lowered car that moves faster than this is raised again
int g_height_mode     = 0;      // 0 shifts the suspension's ride height range, 1 lowers the ride-height tuning value
float g_below_min_cm  = 1.5f;  // how far under the game's own lowest ride height (the tuning slider at 0) the car may be pushed
bool g_camber_on      = true;
int g_price           = 3000;
bool g_verbose        = true;
bool g_probe          = false;   // log the render objects found from the Car on every button hold (for the visual-only lowering)
bool g_visual         = true;   // lower the picture only: the car's render parts move, the physics does not (Visual=0: the physical lowering)
bool g_trace          = true;   // log who reads and writes the body matrix for 2 s after a lowering request
int g_camber_sign     = 1;      // CamberSign=-1 if the wheels lean the wrong way in the visual mode

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
    g_probe          = cfg_int(L"Probe", 0) != 0;
    g_visual         = cfg_int(L"Visual", 1) != 0;
    g_trace          = cfg_int(L"Trace", 1) != 0;
    g_camber_sign    = cfg_int(L"CamberSign", 1) < 0 ? -1 : 1;
    if (g_default_drop < 1) g_default_drop = 1;
    if (g_default_drop > 10) g_default_drop = 10;
    if (g_hold_ms < 200) g_hold_ms = 200;
    if (g_duration < 0.2f) g_duration = 0.2f;
    if (g_step_ms < 8) g_step_ms = 8;
}

// ---- the drop in cm, per garage car (Airride.ini [Drop]) ----------------------------------------------------------------
float drop_for(uint32_t garage) {
    wchar_t key[24], buf[32];
    _snwprintf(key, 24, L"%u", garage);
    GetPrivateProfileStringW(L"Drop", key, L"", buf, 32, g_cfg_path.c_str());
    float v = static_cast<float>(g_default_drop);
    if (buf[0]) {
        wchar_t* end = nullptr;
        const double d = std::wcstod(buf, &end);
        if (end != buf && std::isfinite(d)) v = static_cast<float>(d);
    }
    return v < 1.0f ? 1.0f : v > 10.0f ? 10.0f : v;
}
void set_drop(uint32_t garage, float cm) {
    wchar_t key[24], val[32];
    _snwprintf(key, 24, L"%u", garage);
    _snwprintf(val, 32, L"%.1f", static_cast<double>(cm));
    WritePrivateProfileStringW(L"Drop", key, val, g_cfg_path.c_str());
}

// ---- the tuning menu row: Springs > Airride drop (cm) -----------------------------------------------------------------------
bool car_has_airride() { return g_handle >= 0 && g_api->upgrade_stage(g_mod, g_handle, 0) > 0; }

float row_get(void*) { return drop_for(g_api->upgrade_active_garage()); }
void row_set(void*, float v) {
    const uint32_t garage = g_api->upgrade_active_garage();
    const float cm        = std::round(v * 10.0f) / 10.0f;
    set_drop(garage, cm);
    say("tuning: Airride drop for garage car %u set to %.1f cm", garage, static_cast<double>(cm));
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

struct Part { char* obj; size_t off; float base_y; float last_y; bool wheel; uintptr_t vt; float base_r[9]; float last_r[9]; float side; };
Part g_parts[64];
std::atomic<int> g_nparts{0};
std::atomic<int> g_parts_ready{0};


void probe_scan(char* car) {
    constexpr size_t kMax = 6000;
    struct Item { const char* p; int depth; int parent; unsigned off; };
    static Item queue[kMax];
    struct Hit { const char* obj; size_t off; uintptr_t vt; float pos[3]; char chain[96]; };
    constexpr int kMaxHits = 2500;
    static Hit hits[kMaxHits];
    int nhits = 0;
    static const char* seen[1 << 14];
    std::memset(seen, 0, sizeof seen);
    size_t head = 0, tail = 0, found = 0;
    static Part tmp[64];
    int np = 0;
    const ULONGLONG t0 = GetTickCount64();
    auto mark = [&](const char* q) {   // true if new
        size_t h = (reinterpret_cast<uintptr_t>(q) >> 3) * 0x9E3779B1u & ((1 << 14) - 1);
        while (seen[h]) { if (seen[h] == q) return false; h = (h + 1) & ((1 << 14) - 1); }
        seen[h] = q;
        return true;
    };
    queue[tail++] = {car, 0, -1, 0};
    mark(car);
    say("probe: scanning from the Car %p", static_cast<void*>(car));
    while (head < tail && found < 80 && GetTickCount64() - t0 < 4000) {
        const int self = static_cast<int>(head);
        const Item it = queue[head++];
        __try {
            if (!readable(it.p, 0x40)) continue;
            const uintptr_t vt = *reinterpret_cast<const uintptr_t*>(it.p) - reinterpret_cast<uintptr_t>(g_game);
            if (vt == kVtCarRender1 || vt == kVtCarRender2 || vt == kVtDriver) {
                const char* tag = vt == kVtDriver ? "DriverModel" : "CarRenderModel";
                {
                    char* o = const_cast<char*>(it.p);
                    size_t moff = 0;
                    for (size_t cand : {size_t(0x30), size_t(0x20)}) if (readable(o + cand, 64) && looks_matrix(reinterpret_cast<const float*>(o + cand))) { moff = cand; break; }
                    if (moff && vt != kVtDriver && np < 64) {
                        const float* m = reinterpret_cast<const float*>(o + moff);
                        tmp[np] = {o, moff, m[13], m[13], std::fabs(m[12]) > 0.3f && std::fabs(m[14]) > 0.5f, vt + reinterpret_cast<uintptr_t>(g_game), {}, {}, m[12] > 0 ? 1.0f : -1.0f};
                        for (int r = 0; r < 3; ++r) for (int c = 0; c < 3; ++c) tmp[np].base_r[r * 3 + c] = tmp[np].last_r[r * 3 + c] = m[r * 4 + c];
                        ++np;
                    }
                    const float* m = moff ? reinterpret_cast<const float*>(o + moff) : nullptr;
                    say("probe: %s at %p (depth %d) vt %s matrix +0x%zX pos (%.3f, %.3f, %.3f)", tag, static_cast<const void*>(o), it.depth,
                        vt == kVtCarRender1 ? "b40" : vt == kVtCarRender2 ? "b50" : "driver", moff, m ? static_cast<double>(m[12]) : 0.0,
                        m ? static_cast<double>(m[13]) : 0.0, m ? static_cast<double>(m[14]) : 0.0);
                }
                ++found;
                continue;
            }
            if (nhits < kMaxHits && readable(it.p, 0x400)) {
                for (size_t off = 0; off + 64 <= 0x400 && nhits < kMaxHits; off += 16) {
                    const float* m = reinterpret_cast<const float*>(it.p + off);
                    if (!looks_matrix(m)) continue;
                    if (std::fabs(m[12]) < 3.0f && std::fabs(m[13]) < 3.0f && std::fabs(m[14]) < 3.0f) continue;
                    Hit& h = hits[nhits++];
                    h.obj = it.p; h.off = off; h.vt = *reinterpret_cast<const uintptr_t*>(it.p) - reinterpret_cast<uintptr_t>(g_game);
                    h.pos[0] = m[12]; h.pos[1] = m[13]; h.pos[2] = m[14];
                    int n = 0;
                    for (int c = self; c >= 0 && n < 90; c = queue[c].parent)
                        n += snprintf(h.chain + n, sizeof h.chain - n, "%s%X", c == self ? "" : "<-", queue[c].off);
                }
            }
            if (it.depth >= 4) continue;
            // one readable() check per object, then plain reads inside __try
            const size_t span = readable(it.p, 0x400) ? 0x400 : 0x40;
            for (size_t off = 8; off < span && tail < kMax; off += 8) {
                const char* q = *reinterpret_cast<const char* const*>(it.p + off);
                const uintptr_t v = reinterpret_cast<uintptr_t>(q);
                if (v < 0x10000 || v > 0x00007FFFFFFFFFFFull || (v & 7)) continue;
                if (mark(q)) queue[tail++] = {q, it.depth + 1, self, static_cast<unsigned>(off)};
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    // Where is the car in the physics? Look for world-sized float triples in the car and its dynamics object, then list the
    // matrices stored within 5 m of each: the body's and the wheels' render matrices should be among them.
    {
        float cand[6][3];
        int ncand = 0;
        __try {
            char* iface = *reinterpret_cast<char**>(car + kOffIface);
            const char* regions[2] = {iface ? iface - kIfaceToDyn : nullptr, car};
            const char* names[2]   = {"dyn", "car"};
            for (int r = 0; r < 2; ++r) {
                if (!regions[r]) continue;
                for (size_t off = 0; off + 12 <= 0x1000 && ncand < 6; off += 4) {
                    if (!readable(regions[r] + off, 12)) break;
                    const float* f = reinterpret_cast<const float*>(regions[r] + off);
                    if (!std::isfinite(f[0]) || !std::isfinite(f[1]) || !std::isfinite(f[2])) continue;
                    if (std::fabs(f[0]) < 100 || std::fabs(f[2]) < 100 || std::fabs(f[0]) > 50000 || std::fabs(f[2]) > 50000 || std::fabs(f[1]) > 3000) continue;
                    say("probe: %s +0x%zX holds a world position (%.2f, %.2f, %.2f)", names[r], off, static_cast<double>(f[0]), static_cast<double>(f[1]), static_cast<double>(f[2]));
                    bool dup = false;
                    for (int k = 0; k < ncand; ++k) if (std::fabs(cand[k][0] - f[0]) < 0.5f && std::fabs(cand[k][2] - f[2]) < 0.5f) dup = true;
                    if (!dup) { cand[ncand][0] = f[0]; cand[ncand][1] = f[1]; cand[ncand][2] = f[2]; ++ncand; }
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        for (int k = 0; k < ncand && k < 3; ++k) {
            int printed = 0;
            say("probe: matrices within 5 m of (%.2f, %.2f, %.2f):", static_cast<double>(cand[k][0]), static_cast<double>(cand[k][1]), static_cast<double>(cand[k][2]));
            for (int i = 0; i < nhits && printed < 90; ++i) {
                const float dx = hits[i].pos[0] - cand[k][0], dy = hits[i].pos[1] - cand[k][1], dz = hits[i].pos[2] - cand[k][2];
                if (dx * dx + dy * dy + dz * dz > 25.0f) continue;
                ++printed;
                say("probe:   %p (vt %llX) +0x%zX rel (%.2f, %.2f, %.2f) via %s", static_cast<const void*>(hits[i].obj), static_cast<unsigned long long>(hits[i].vt),
                    hits[i].off, static_cast<double>(dx), static_cast<double>(dy), static_cast<double>(dz), hits[i].chain);
            }
        }
    }
    (void)np;
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



// ---- trace: who writes and who reads the body matrix? (page guard on its page; the faulting instruction's address is the answer) ----

struct TraceHit { uint64_t rip, addr, rw, stack[10]; DWORD tid; };
TraceHit g_th[400];
std::atomic<int> g_nth{0};
std::atomic<int> g_trace_on{0};
std::atomic<int> g_rearm{0};
uintptr_t g_tr_lo[4], g_tr_hi[4], g_tr_page[4];
DWORD g_tr_prot[4];
int g_tr_n = 0;
ULONGLONG g_tr_start = 0;

LONG CALLBACK trace_veh(EXCEPTION_POINTERS* ep) {
    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code == STATUS_SINGLE_STEP) {
        if (g_rearm.exchange(0)) {
            if (g_trace_on.load()) for (int i = 0; i < g_tr_n; ++i) { DWORD old; VirtualProtect(reinterpret_cast<void*>(g_tr_page[i]), 1, g_tr_prot[i] | PAGE_GUARD, &old); }
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (code != STATUS_GUARD_PAGE_VIOLATION || !g_trace_on.load()) return EXCEPTION_CONTINUE_SEARCH;
    const uintptr_t addr = ep->ExceptionRecord->ExceptionInformation[1];
    bool ours = false;
    for (int i = 0; i < g_tr_n; ++i) if ((addr & ~uintptr_t(0xFFF)) == g_tr_page[i]) ours = true;
    if (!ours) return EXCEPTION_CONTINUE_SEARCH;
    for (int i = 0; i < g_tr_n; ++i) {
        if (addr >= g_tr_lo[i] && addr < g_tr_hi[i]) {
            const int k = g_nth.fetch_add(1);
            if (k < 400) {
                TraceHit& h = g_th[k];
                h.rip = ep->ContextRecord->Rip; h.addr = addr; h.rw = ep->ExceptionRecord->ExceptionInformation[0]; h.tid = GetCurrentThreadId();
                const uint64_t* sp = reinterpret_cast<const uint64_t*>(ep->ContextRecord->Rsp);
                int n = 0;
                const uint64_t lo = reinterpret_cast<uint64_t>(g_game), hi = lo + 0x3000000;
                for (int j = 0; j < 48 && n < 10; ++j) { if (sp[j] >= lo && sp[j] < hi) h.stack[n++] = sp[j] - lo; }
                while (n < 10) h.stack[n++] = 0;
            }
            break;
        }
    }
    ep->ContextRecord->EFlags |= 0x100;   // single-step, then guard the page again
    g_rearm.store(1);
    return EXCEPTION_CONTINUE_EXECUTION;
}

void trace_arm() {
    if (!g_trace || g_trace_on.load()) return;
    g_tr_n = 0;
    g_nth.store(0);
    const int n = g_nparts.load();
    for (int i = 0; i < n && g_tr_n < 4; ++i) {
        const uintptr_t a = reinterpret_cast<uintptr_t>(g_parts[i].obj + g_parts[i].off);
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) continue;
        g_tr_lo[g_tr_n] = a; g_tr_hi[g_tr_n] = a + 64; g_tr_page[g_tr_n] = a & ~uintptr_t(0xFFF); g_tr_prot[g_tr_n] = mbi.Protect & ~(PAGE_GUARD);
        ++g_tr_n;
    }
    if (!g_tr_n) return;
    g_trace_on.store(1);
    g_tr_start = GetTickCount64();
    for (int i = 0; i < g_tr_n; ++i) { DWORD old; VirtualProtect(reinterpret_cast<void*>(g_tr_page[i]), 1, g_tr_prot[i] | PAGE_GUARD, &old); }
    say("trace: watching %d body matrix page(s) for 2 s", g_tr_n);
}

void trace_poll() {   // from the per-frame step
    if (!g_trace_on.load()) return;
    if (GetTickCount64() - g_tr_start < 2000 && g_nth.load() < 380) return;
    g_trace_on.store(0);
    Sleep(5);
    for (int i = 0; i < g_tr_n; ++i) { DWORD old; VirtualProtect(reinterpret_cast<void*>(g_tr_page[i]), 1, g_tr_prot[i], &old); }
    const int n = g_nth.load() < 400 ? g_nth.load() : 400;
    say("trace: %d access(es) to the body matrix recorded", n);
    struct Agg { uint64_t rip, rw; int count; DWORD tid; uint64_t stack[10]; };
    static Agg agg[40];
    int na = 0;
    for (int i = 0; i < n; ++i) {
        int j = 0;
        for (; j < na; ++j) if (agg[j].rip == g_th[i].rip && agg[j].rw == g_th[i].rw) break;
        if (j == na && na < 40) { agg[na] = {g_th[i].rip, g_th[i].rw, 0, g_th[i].tid, {}}; std::memcpy(agg[na].stack, g_th[i].stack, sizeof agg[na].stack); ++na; }
        if (j < 40) ++agg[j].count;
    }
    for (int j = 0; j < na; ++j) {
        say("trace: %s by game+0x%llX x%d (thread %lu) callers: %llX %llX %llX %llX %llX %llX %llX %llX", agg[j].rw == 1 ? "WRITE" : "read ",
            static_cast<unsigned long long>(agg[j].rip - reinterpret_cast<uint64_t>(g_game)), agg[j].count, static_cast<unsigned long>(agg[j].tid),
            static_cast<unsigned long long>(agg[j].stack[0]), static_cast<unsigned long long>(agg[j].stack[1]), static_cast<unsigned long long>(agg[j].stack[2]),
            static_cast<unsigned long long>(agg[j].stack[3]), static_cast<unsigned long long>(agg[j].stack[4]), static_cast<unsigned long long>(agg[j].stack[5]),
            static_cast<unsigned long long>(agg[j].stack[6]), static_cast<unsigned long long>(agg[j].stack[7]));
    }
}

// ---- visual-only lowering: the car's render parts (body, interior, lights ...) move down, the wheels and the physics stay ----------

// The car's body render matrix: found by the 20:23 probe log. The car's dynamics keep the world position at dyn+0x30; the matrices
// sitting 0.35 m below that point (body origin) are reached from the Car by these pointer chains (offsets, then the matrix offset).
struct AnchorPath { size_t hops[3]; int nhops; size_t mat; };
const AnchorPath kAnchors[] = {{{0x38, 0x1A0, 0}, 2, 0x20}, {{0x118, 0x78, 0xD0}, 3, 0x220}};

int resolve_anchors(char* car) {
    int added = 0;
    __try {
        char* iface = *reinterpret_cast<char**>(car + kOffIface);
        if (!iface) return 0;
        const float* bp = reinterpret_cast<const float*>(iface - kIfaceToDyn + 0x30);
        if (!std::isfinite(bp[0]) || !std::isfinite(bp[1]) || !std::isfinite(bp[2])) return 0;
        for (const AnchorPath& a : kAnchors) {
            char* ptr = car;
            bool ok   = true;
            for (int h = 0; h < a.nhops && ok; ++h) {
                if (!readable(ptr + a.hops[h], 8)) { ok = false; break; }
                ptr = *reinterpret_cast<char**>(ptr + a.hops[h]);
                if (reinterpret_cast<uintptr_t>(ptr) < 0x10000) ok = false;
            }
            if (!ok || !readable(ptr, a.mat + 64)) { say("airride(visual): anchor via 0x%zX... not readable", a.hops[0]); continue; }
            const float* m = reinterpret_cast<const float*>(ptr + a.mat);
            const float dx = m[12] - bp[0], dy = m[13] - bp[1], dz = m[14] - bp[2];
            if (!looks_matrix(m) || dx * dx + dy * dy + dz * dz > 9.0f) {
                say("airride(visual): anchor via 0x%zX is not the body matrix any more (rel %.2f %.2f %.2f)", a.hops[0], static_cast<double>(dx), static_cast<double>(dy), static_cast<double>(dz));
                continue;
            }
            Part pt{};
            pt.obj = ptr; pt.off = a.mat; pt.base_y = pt.last_y = m[13]; pt.wheel = false; pt.vt = *reinterpret_cast<const uintptr_t*>(ptr);
            if (added < 64) g_parts[added++] = pt;
            say("airride(visual): body matrix at %p +0x%zX, rel (%.2f, %.2f, %.2f)", static_cast<void*>(ptr), a.mat, static_cast<double>(dx), static_cast<double>(dy), static_cast<double>(dz));
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        say("airride(visual): a fault while looking for the body matrix");
    }
    return added;
}

char* g_vis_car = nullptr;
bool g_body_hook_on = false;
bool g_vis_pending = false;
int g_vis_target = 0;
float g_vis_p = 0, g_vis_drop_m = 0;
int g_vis_rewrites = 0, g_vis_wheel_rewrites = 0;
bool g_vis_active = false;

bool part_alive(const Part& pt) {
    __try {
        return readable(pt.obj, pt.off + 64) && *reinterpret_cast<const uintptr_t*>(pt.obj) == pt.vt;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

float g_vis_camber_deg = 0;

CRITICAL_SECTION g_vis_cs;
std::atomic<int> g_cache_calls{0};
std::atomic<int> g_body_hits{0};
std::atomic<int> g_wc[5][8][2];   // [tire/wheel setter][wheel index][corrected?] while lowered
float g_body_rot[12], g_body_pos[3];
void* g_player_scenes[8] = {};
std::atomic<int> g_player_scene_n{0};
bool known_scene(void* sc) { const int n = g_player_scene_n.load(); for (int i = 0; i < n && i < 8; ++i) if (g_player_scenes[i] == sc) return true; return false; }
void add_scene(void* sc) { if (known_scene(sc)) return; const int n = g_player_scene_n.load(); if (n < 8) { g_player_scenes[n] = sc; g_player_scene_n.store(n + 1); } }

std::atomic<int> g_wheel_hits{0};
std::atomic<float> g_vis_e{0};
std::atomic<int> g_vis_thread_on{0};

void vis_write_locked(float e);
void vis_write(float e) {
    if (g_trace_on.load()) return;   // while the page guard watches the matrix our own accesses would drown the game's
    EnterCriticalSection(&g_vis_cs);
    vis_write_locked(e);
    LeaveCriticalSection(&g_vis_cs);
}

void vis_write_part(Part& pt, float e) {
    {
        if (!part_alive(pt)) return;
        float* m = reinterpret_cast<float*>(pt.obj + pt.off);
        __try {
            if (!pt.wheel) {
                const float cur = m[13];
                if (std::fabs(cur - pt.last_y) > 1e-4f) {   // the game wrote the matrix since: start from what it wrote
                    pt.base_y = cur;
                    ++g_vis_rewrites;
                }
                pt.last_y = pt.base_y - g_vis_drop_m * e;
                m[13]     = pt.last_y;
            } else if (g_camber_on) {
                float diff = 0;
                for (int r = 0; r < 3; ++r) for (int c = 0; c < 3; ++c) diff = std::fmax(diff, std::fabs(m[r * 4 + c] - pt.last_r[r * 3 + c]));
                if (diff > 1e-4f) {
                    for (int r = 0; r < 3; ++r) for (int c = 0; c < 3; ++c) pt.base_r[r * 3 + c] = m[r * 4 + c];
                    ++g_vis_wheel_rewrites;
                }
                // negative camber: the top of the wheel leans toward the car (rotation about the car's long axis, z)
                const float th = pt.side * g_camber_sign * g_vis_camber_deg * e * 0.01745329252f;
                const float cs = std::cos(th), sn = std::sin(th);
                for (int r = 0; r < 3; ++r) {
                    const float x = pt.base_r[r * 3], y = pt.base_r[r * 3 + 1], z = pt.base_r[r * 3 + 2];
                    pt.last_r[r * 3]     = x * cs - y * sn;
                    pt.last_r[r * 3 + 1] = x * sn + y * cs;
                    pt.last_r[r * 3 + 2] = z;
                    m[r * 4] = pt.last_r[r * 3]; m[r * 4 + 1] = pt.last_r[r * 3 + 1]; m[r * 4 + 2] = pt.last_r[r * 3 + 2];
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
}


void vis_write_locked(float e) {
    const int n = g_nparts.load();
    for (int i = 0; i < n; ++i) vis_write_part(g_parts[i], e);
}

void vis_forget() {
    g_player_scene_n.store(0);
    g_vis_thread_on.store(0);
    g_vis_e.store(0);
    g_vis_pending = false;
    g_vis_target  = 0;
    g_vis_p       = 0;
    g_vis_active  = false;
    g_parts_ready.store(0);
    g_nparts.store(0);
}

// The game writes the body matrix again every frame after Car::Update, so a single write per frame is overwritten before it is drawn.
// This thread re-applies the offset every millisecond or so while the car is lowered (a test of whether this matrix is what is drawn).
DWORD WINAPI vis_writer(LPVOID) {
    for (;;) {
        if (g_vis_thread_on.load()) vis_write(g_vis_e.load());
        Sleep(g_vis_thread_on.load() ? 0 : 20);
    }
}

void visual_step(char* car, float dt, int req, bool usable, bool driving, float speed, uint32_t garage) {
    trace_poll();
    if (req) {
        if (g_vis_active && (g_vis_target == 1 || g_vis_p > 0)) {
            g_vis_target = 0;
            say("airride(visual): raising");
        } else if (!usable) {
            say("airride: stick held, but this car has no Airride (or the garage car is another model)");
        } else if (!driving) {
            say("airride: stick held, but the HUD isn't up (menu or photo mode)");
        } else if (speed > g_still_mps) {
            say("airride: stick held, but the car isn't standing still (%.2f m/s)", static_cast<double>(speed));
        } else if (!g_vis_pending) {
            int n = 0;
            if (g_body_hook_on) { g_parts[0] = Part{}; n = 1; } else n = resolve_anchors(car);
            if (g_probe) probe_start(car);
            if (n > 0) {
                g_nparts.store(n);
                g_parts_ready.store(1);
                g_vis_pending = true;
            } else {
                say("airride(visual): found no body matrix: nothing to move");
            }
        }
    }
    if (g_vis_pending && g_parts_ready.load()) {
        g_vis_pending = false;
        const float cm = drop_for(garage);
        g_vis_drop_m  = cm * 0.01f;
        g_vis_rewrites = 0;
        g_vis_wheel_rewrites = 0;
        g_vis_camber_deg = g_camber_per_cm * cm;
        int moving = 0;
        for (int i = 0; i < g_nparts.load(); ++i) if (!g_parts[i].wheel) ++moving;
        say("airride(visual): %d render parts found, %d of them move down, %d wheel parts lean by %.2f deg; lowering %.1f cm", g_nparts.load(), moving, g_nparts.load() - moving, g_camber_on ? static_cast<double>(g_vis_camber_deg) : 0.0, static_cast<double>(cm));
        if (moving > 0) {
            g_vis_active = true;
            g_vis_target = 1;
        } else {
            vis_forget();
        }
    }
    if (!g_vis_active) return;
    if (g_vis_target == 1 && (!usable || speed > g_raise_mps)) {
        g_vis_target = 0;
        say("airride(visual): %s, raising", !usable ? "the car lost its Airride" : "the car is moving");
    }
    float np = g_vis_p + (g_vis_target ? 1.0f : -1.0f) * (dt > 0.1f ? 0.1f : dt < 0 ? 0 : dt) / g_duration;
    np       = np < 0 ? 0 : np > 1 ? 1 : np;
    g_vis_p  = np;
    g_vis_e.store(ease(g_vis_p));
    vis_write(g_vis_e.load());
    if ((np == 0 && !g_vis_target) || (np == 1 && g_vis_target)) {
        static int last_state = -1;
        if (last_state != g_vis_target && g_vis_target == 1) {
            for (int k = 0; k < 5; ++k) {
                char line[300];
                int n = snprintf(line, sizeof line, "airride(visual): %s setter calls (idx: corrected/not):", k == 0 ? "tire" : k == 1 ? "wheel" : k == 2 ? "part A" : k == 3 ? "part B" : "part C");
                for (int w = 0; w < 8; ++w) n += snprintf(line + n, sizeof line - n, " %d:%d/%d", w, g_wc[k][w][1].load(), g_wc[k][w][0].load());
                say("%s", line);
            }
            for (int k = 0; k < 5; ++k) for (int w = 0; w < 8; ++w) { g_wc[k][w][0].store(0); g_wc[k][w][1].store(0); }
        }
        if (last_state != g_vis_target) {
            say("airride(visual): %s (the game rewrote the body matrix %d times; the cache hook ran %d times, the body setter was changed %d times, the wheel setters %d times)", g_vis_target ? "lowered" : "back at normal height", g_vis_rewrites, g_cache_calls.load(), g_body_hits.load(), g_wheel_hits.load());
            last_state = g_vis_target;
        }
        if (!g_vis_target) { last_state = -1; vis_forget(); }
    }
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
    if (g_visual) {
        if (car != g_vis_car) { vis_forget(); g_vis_car = car; }
        visual_step(car, dt, req, usable, driving, speed, garage);
        return;
    }
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
            const int cm = static_cast<int>(std::lround(drop_for(garage)));
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

// game+0x1B41CC0 refreshes a body object's cached matrix (this+0x20, 64 bytes, copied from its source when stale) and its caller
// reads the cache right away (found with the page-guard trace). Changing the cache after the original runs is seen by that reader.
typedef uint64_t (*CacheFn)(void*);
CacheFn g_orig_cache = nullptr;
uint64_t detour_cache(void* self) {
    const uint64_t r = g_orig_cache(self);
    if (g_vis_active && !g_trace_on.load()) {
        const int n = g_nparts.load();
        for (int i = 0; i < n; ++i) {
            if (g_parts[i].obj != static_cast<char*>(self)) continue;
            EnterCriticalSection(&g_vis_cs);
            vis_write_part(g_parts[i], g_vis_e.load());
            LeaveCriticalSection(&g_vis_cs);
            g_cache_calls.fetch_add(1);
        }
    }
    return r;
}


// game+0x21FF980 is the FH3 twin of the FH2 body setter the StanceAirride mod intercepts: it copies the body's 4x4 matrix into the
// scene's body node (this+0x10) and tail-jumps on. Three callers, like in FH2. The matrix is a scratch copy on the caller's stack, so
// it can be changed before the original runs. Only a matrix within 2 m of the player's car (the dynamics' world position) is touched.

// Diagnostics for a menu preview (the garage's upgrade list): while F9 is held, the first 24 calls of each 2 s are logged, 400 at most.
void menu_diag(const char* what, uintptr_t ret, void* target, const float* m, unsigned wheel) {
    if (!g_menu_diag || !(GetAsyncKeyState(VK_F9) & 0x8000)) return;   // only while F9 is held (hold it in the garage's upgrade list)
    static std::atomic<ULONGLONG> win{0};
    static std::atomic<int> n{0}, total{0};
    const ULONGLONG now = GetTickCount64();
    if (now - win.load() > 2000) { win.store(now); n.store(0); }
    if (n.fetch_add(1) >= 24 || total.fetch_add(1) >= 400) return;
    __try {
        if (m && readable(m, 64))
            say("menu: %s ret game+0x%llX target %p wheel %u pos (%.3f, %.3f, %.3f) rows x(%.2f %.2f %.2f) y(%.2f %.2f %.2f) thread %lu", what, static_cast<unsigned long long>(ret), target, wheel,
                static_cast<double>(m[12]), static_cast<double>(m[13]), static_cast<double>(m[14]), static_cast<double>(m[0]), static_cast<double>(m[1]), static_cast<double>(m[2]),
                static_cast<double>(m[4]), static_cast<double>(m[5]), static_cast<double>(m[6]), GetCurrentThreadId());
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}
typedef uint64_t (*BodySetFn)(void*, float*);
BodySetFn g_orig_body = nullptr;
thread_local int t_wheel_ctx_player = 0;   // per thread (the game renders cars on several threads at once); the car whose body was just set from game+0x1E32C05 is the player's (its wheels follow in the same function)
// Garage preview: while the garage's upgrade list has the cursor on the Airride stage, the shop's car is lowered by PreviewCm, smoothly, and put back when the cursor leaves, the stage goes into the basket or the shop closes.
float g_preview_cm = 3.0f;
std::atomic<float> g_pv_p{0}, g_pv_e{0};   // the preview's progress, and its eased value for the wheel setters
std::atomic<ULONGLONG> g_pv_last{0};
float preview_ease() {
    const ULONGLONG now = GetTickCount64();
    const ULONGLONG last = g_pv_last.exchange(now);
    float p = g_pv_p.load();
    float dt = last ? static_cast<float>(now - last) * 0.001f : 0.0f;
    if (dt > 1.0f) { p = 0.0f; dt = 0.0f; }   // the shop wasn't drawn for a while: start from normal height
    const float raw_dt = dt;
    if (dt > 0.1f) dt = 0.1f;
    const int stage = (g_api && g_api->api_version >= 15 && g_api->upgrade_preview_stage && g_handle >= 0) ? g_api->upgrade_preview_stage(g_mod, g_handle) : -1;
    static std::atomic<int> last_stage{-2}, shown{0};
    if (last_stage.exchange(stage) != stage && shown.fetch_add(1) < 60) say("garage preview: the list shows stage %d (ease %.2f)", stage, static_cast<double>(p));
    if (stage <= 0 && raw_dt > 0.25f) p = 0.0f;   // the setters come rarely in the garage: after a pause, going back is immediate
    p += (stage > 0 ? dt : -dt) / 0.6f;
    p = p < 0.0f ? 0.0f : p > 1.0f ? 1.0f : p;
    g_pv_p.store(p);
    const float ev = p * p * (3.0f - 2.0f * p);
    g_pv_e.store(ev);
    return ev;
}
thread_local bool t_ctx_preview = false;
thread_local float t_ctx_drop = 0.0f, t_ctx_camber = 0.0f, t_ctx_rot[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};

uint64_t detour_body(void* target, float* m) {
    menu_diag("body", reinterpret_cast<uintptr_t>(__builtin_return_address(0)) - reinterpret_cast<uintptr_t>(g_game), target, m, 99);
    if (m) {
        __try {
            const uintptr_t ret = reinterpret_cast<uintptr_t>(__builtin_return_address(0)) - reinterpret_cast<uintptr_t>(g_game);
            if (readable(m, 64)) {
                bool ctx = false, pv_ctx = false;
                float drop = 0.0f, camber = 0.0f;
                if (g_vis_car) {
                    char* iface = *reinterpret_cast<char**>(g_vis_car + kOffIface);
                    if (iface) {
                        const float* bp = reinterpret_cast<const float*>(iface - kIfaceToDyn + 0x30);
                        const float dx = m[12] - bp[0], dy = m[13] - bp[1], dz = m[14] - bp[2];
                        const bool near_player = dx * dx + dz * dz < 0.25f && std::fabs(dy) < 1.5f;   // the player's own body: its matrix sits within a few cm (horizontally) of the dynamics' position; another car beside it is more than a metre off
                        const float e = g_vis_e.load();
                        if (g_vis_active && near_player && e > 0.0f) {
                            static int logged = 0;
                            if (logged < 6) {
                                ++logged;
                                say("airride(visual): body setter near the car: target %p from game+0x%llX, rel (%.2f, %.2f, %.2f)", target, static_cast<unsigned long long>(ret),
                                    static_cast<double>(dx), static_cast<double>(dy), static_cast<double>(dz));
                            }
                            ctx = true;
                            drop = g_vis_drop_m * e;
                            camber = g_vis_camber_deg * e;
                            g_body_hits.fetch_add(1);
                        }
                    }
                }
                if (!ctx && g_visual) {   // the garage's showroom car (its position isn't known: while the upgrade list has the cursor on the Airride, every car drawn is lowered, only the shop's is on screen)
                    const float e = preview_ease();
                    if (e > 0.0f) {
                        ctx = true;
                        pv_ctx = true;
                        drop = g_preview_cm * 0.01f * e;   // no camber in the preview: the showroom car's wheel frame isn't the driven car's
                    }
                }
                if (ret == 0x1E32C05) {   // the wheels of this car follow in the same function, on this thread
                    t_wheel_ctx_player = ctx ? 1 : 0;
                    t_ctx_preview = pv_ctx;
                    t_ctx_drop = drop;
                    t_ctx_camber = camber;
                    if (ctx) for (int k = 0; k < 12; ++k) { t_ctx_rot[k] = m[k]; g_body_rot[k] = m[k]; }   // lateral, up, forward rows of the body (before the offset)
                }
                if (ctx) m[13] -= drop;   // everything built from this matrix (body, driver, steering wheel, and the wheels) goes down; the wheels are lifted back in their own setters
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    return g_orig_body(target, m);
}

// The wheels' setters (twins of the FH2 tire / wheel hooks): the matrix is a stack scratch passed to both in turn. The wheels were built
// from the lowered body matrix, so they are lifted back by the drop, and leaned for the negative camber. The scratch is put back after
// each call because the second setter gets the same buffer.
typedef uint64_t (*PartSetFn)(void*, unsigned, float*, void*, void*);
PartSetFn g_orig_tire = nullptr, g_orig_wheel = nullptr;

uint64_t part_detour(int kind, PartSetFn orig, void* scene, unsigned wheel, float* m, void* ctx, void* aux) {
    menu_diag(kind == 0 ? "tire" : kind == 1 ? "wheel" : "brake part", 0, scene, m, wheel);
    float saved[16];
    bool changed = false;
    const bool flag = t_wheel_ctx_player != 0;
    if (flag && g_vis_active && t_ctx_drop > 0.0f && g_vis_e.load() > 0.0f) add_scene(scene);
    const bool scene_mine = g_vis_active && !flag && known_scene(scene);
    const float pv = g_pv_e.load();
    const bool pv_all = pv > 0.0f && !(flag && !t_ctx_preview) && !scene_mine;   // the garage preview: every wheel part drawn is lifted back by the drop (all bodies were lowered)
    const bool mine = flag || scene_mine || pv_all;
    if (g_vis_active && g_vis_e.load() > 0.0f && wheel < 8) g_wc[kind][wheel][mine ? 1 : 0].fetch_add(1);
    if (mine && m && wheel < 8) {
        __try {
            const float e = 1.0f;
            static const float kUp[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
            const float drop_now   = pv_all ? g_preview_cm * 0.01f * pv : flag ? t_ctx_drop : g_vis_drop_m * g_vis_e.load();
            const float camber_now = pv_all ? 0.0f : flag ? t_ctx_camber : g_vis_camber_deg * g_vis_e.load();
            const float* rot       = pv_all ? kUp : flag ? t_ctx_rot : g_body_rot;
            if (drop_now > 0.0f && readable(m, 64) && std::isfinite(m[12]) && std::isfinite(m[13]) && std::isfinite(m[14]) && std::fabs(m[12]) < 20.0f && std::fabs(m[13]) < 20.0f && std::fabs(m[14]) < 20.0f) {
                std::memcpy(saved, m, 64);
                changed = true;
                // The matrix is the wheel's, in the car's own frame (x lateral, y up, z forward): it moves with the body matrix, which was
                // lowered by `drop` along the world's up. Undo that in the car's frame: drop times the world-up's components in it.
                const float lift = drop_now;
                m[12] += lift * rot[1];
                m[13] += lift * rot[5];
                m[14] += lift * rot[9];
                if (g_camber_on && camber_now != 0.0f) {
                    static int plog = 0;
                    if (plog < 16 && kind == 0) { ++plog; say("airride(visual): wheel %u local pos (%.3f, %.3f, %.3f) rows x(%.2f %.2f %.2f) y(%.2f %.2f %.2f)", wheel, double(m[12]), double(m[13]), double(m[14]), double(m[0]), double(m[1]), double(m[2]), double(m[4]), double(m[5]), double(m[6])); }
                    const float side = m[14] > 0 ? 1.0f : -1.0f;   // the car's frame: x is along the car (front/rear), z across it (the log's wheel positions: x +-1.4, z +-0.9)
                    const float th   = side * g_camber_sign * camber_now * e * 0.01745329252f;   // top of the wheel toward the car
                    const float cs = std::cos(th), sn = std::sin(th);
                    for (int r = 0; r < 3; ++r) {   // rotate each basis row about the car's long (x) axis through the hub: the camber
                        float* v = m + r * 4;
                        const float vy = v[1], vz = v[2];
                        v[1] = vy * cs + vz * sn;
                        v[2] = -vy * sn + vz * cs;
                    }
                }
                g_wheel_hits.fetch_add(1);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            changed = false;
        }
    }
    const uint64_t r = orig(scene, wheel, m, ctx, aux);
    if (changed) std::memcpy(m, saved, 64);
    return r;
}
uint64_t detour_tire(void* scene, unsigned wheel, float* m, void* ctx, void* aux) { return part_detour(0, g_orig_tire, scene, wheel, m, ctx, aux); }
uint64_t detour_wheel(void* scene, unsigned wheel, float* m, void* ctx, void* aux) { return part_detour(1, g_orig_wheel, scene, wheel, m, ctx, aux); }
// three more per-wheel part setters of the same function (the brake parts): same arguments, same treatment
PartSetFn g_orig_pa = nullptr, g_orig_pb = nullptr, g_orig_pc = nullptr;
uint64_t detour_pa(void* scene, unsigned wheel, float* m, void* ctx, void* aux) { return part_detour(2, g_orig_pa, scene, wheel, m, ctx, aux); }
uint64_t detour_pb(void* scene, unsigned wheel, float* m, void* ctx, void* aux) { return part_detour(3, g_orig_pb, scene, wheel, m, ctx, aux); }
uint64_t detour_pc(void* scene, unsigned wheel, float* m, void* ctx, void* aux) { return part_detour(4, g_orig_pc, scene, wheel, m, ctx, aux); }

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
    FH3Hook* h3 = g_api->hook_function(g_mod, "body matrix cache (Airride)", g_game + 0x1B41CC0, reinterpret_cast<void*>(&detour_cache),
                                       reinterpret_cast<void**>(&g_orig_cache));
    if (h3) g_api->enable_hooks(g_mod, h3);
    say("hooks: body matrix cache %s", h3 ? "on" : "FAILED");
    FH3Hook* h4 = g_api->hook_function(g_mod, "body setter (Airride)", g_game + 0x21FF980, reinterpret_cast<void*>(&detour_body),
                                       reinterpret_cast<void**>(&g_orig_body));
    if (h4) g_api->enable_hooks(g_mod, h4);
    g_body_hook_on = h4 != nullptr;
    say("hooks: body setter %s", h4 ? "on" : "FAILED");
    FH3Hook* h5 = g_api->hook_call(g_mod, "tire matrix (Airride)", g_game + 0x1E717BA, reinterpret_cast<void*>(&detour_tire), reinterpret_cast<void**>(&g_orig_tire));
    FH3Hook* h6 = g_api->hook_call(g_mod, "wheel matrix (Airride)", g_game + 0x1E717D7, reinterpret_cast<void*>(&detour_wheel), reinterpret_cast<void**>(&g_orig_wheel));
    if (h5) g_api->enable_hooks(g_mod, h5);
    if (h6) g_api->enable_hooks(g_mod, h6);
    say("hooks: tire setter %s, wheel setter %s", h5 ? "on" : "FAILED", h6 ? "on" : "FAILED");
    FH3Hook* h7 = g_api->hook_call(g_mod, "part A matrix (Airride)", g_game + 0x1E71B98, reinterpret_cast<void*>(&detour_pa), reinterpret_cast<void**>(&g_orig_pa));
    FH3Hook* h8 = g_api->hook_call(g_mod, "part B matrix (Airride)", g_game + 0x1E71D04, reinterpret_cast<void*>(&detour_pb), reinterpret_cast<void**>(&g_orig_pb));
    FH3Hook* h9 = g_api->hook_call(g_mod, "part C matrix (Airride)", g_game + 0x1E71F25, reinterpret_cast<void*>(&detour_pc), reinterpret_cast<void**>(&g_orig_pc));
    if (h7) g_api->enable_hooks(g_mod, h7);
    if (h8) g_api->enable_hooks(g_mod, h8);
    if (h9) g_api->enable_hooks(g_mod, h9);
    say("hooks: brake parts %s/%s/%s", h7 ? "on" : "FAILED", h8 ? "on" : "FAILED", h9 ? "on" : "FAILED");
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
        row.label   = "Airride|Drop (cm)";
        static char help[200];
        snprintf(help, sizeof help, "How far the Airride lowers the car, in centimetres (1 to 10, in steps of 0.1). Each centimetre also adds %.2f degrees of negative camber so the wheels fit.", static_cast<double>(g_camber_per_cm));
        row.help    = help;
        row.min     = 1;
        row.max     = 10;
        row.step    = 0.1f;
        row.decimals = 1;
        row.get     = row_get;
        row.set     = row_set;
        row.visible = row_visible;
        row.user    = nullptr;
        say("tuning row: %s", api->add_tuning_row(g_mod, &row) ? "added" : "NOT added (see the platform's lines above)");
    } else {
        say("this platform (API %u) can't add the tuning slider (needs API 13); the drop stays at %d cm", api->api_version, g_default_drop);
    }

    if (HANDLE t = CreateThread(nullptr, 0, install_hooks, nullptr, 0, nullptr)) CloseHandle(t);
    InitializeCriticalSection(&g_vis_cs);
    AddVectoredExceptionHandler(1, trace_veh);
    if (HANDLE t = CreateThread(nullptr, 0, vis_writer, nullptr, 0, nullptr)) CloseHandle(t);
    if (HANDLE t = CreateThread(nullptr, 0, input_thread, nullptr, 0, nullptr)) CloseHandle(t);
    return 0;
}
