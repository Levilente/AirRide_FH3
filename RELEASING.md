# Publishing a release

The release must contain **only the DLL** as an asset.

1. Build (see the README): the result is `build/Airride.dll`.
2. Test the DLL in game once (the platform's `fh3-mods\platform.log` should show the `[Airride]` lines without errors; buy it, lower a car).
3. Make sure the version in `CMakeLists.txt` (`project(... VERSION x.y.z)`), CHANGELOG.md and the tag agree (current: `1.0.1`). The platform's update check compares the GitHub release tag (a leading `v` is ignored) with the version the mod registers.
4. Tag and publish:

```sh
git tag v1.0.1
git push origin v1.0.1
gh release create v1.0.1 build/Airride.dll --title "Airride for FH3 1.0.1" --notes-file release-notes.md
```

`release-notes.md` can be the matching section of [CHANGELOG.md](CHANGELOG.md) plus the requirements line from the README (FH3 Mod Platform 1.0.0 or newer). Do not attach source archives, ini files or anything else; GitHub adds the source zip by itself.

The DLL is build output and is listed in `.gitignore`; it is never committed.
