# Game Distribution (US-073)

Build Game packages a project as a standalone game. It runs in the editor (File > Build game...) or in code through
`engine::editor::build_game` (`engine/editor/src/distribution.cpp`), and `GameBuilder` runs it on a worker thread with progress and
cancel. The editor window shows the stage, the cook count, and the outcome; Cancel stops the build before its next step.

## Steps

1. **Validate.** The converter's Validate command checks every sidecar, importer, setting, and dependency. A failed check stops the build.
   The start scene must be a registered asset with the `json` importer.
2. **Cook.** The converter cooks every asset into `<output>/<name>.build/cooked`. Cached imports and cooks are reused. A cancelled
   cook publishes the assets it finished, and the build stops.
3. **Pack.** The start scene's closure is packed (`engine/pack`). A missing dependency or cooked file aborts the build.
4. **Package.** The package is assembled in `<output>/<name>.staging`.
5. **Verify.** The staged package is copied into an empty folder under `<output>/<name>.build/verify`, and the runtime is started there
   with the launcher's arguments. The build fails unless it exits 0 and reports `game_runtime: started scene <guid>`.
6. **Publish.** The staging folder is renamed to `<output>/<name>`. An existing folder there is replaced only when it is a package.

A failed or cancelled build removes the staging and verify folders. The cooked cache stays for the next build.

## Package layout

```
<name>/
  game_runtime(.exe)        the standalone runtime, built for this host (Windows or Linux)
  data/game.pack            the start scene's pack (the resource pack format is in docs/runtime-packs.md)
  shaders/                  every compiled shader of the engine build
  NOTICES.txt               third-party notices, generated at configure time from third_party/manifest.json
  run_game.bat | run_game.sh   the launcher: passes the pack, start scene, backend, and profile
  game.json                 the manifest: format engine.package, name, platform, start_scene, backend, profile, arguments,
                            packs, and every other file's size and SHA-256
```

The settings a game runs with are the backend, the profile, and the start scene. They are in `game.json` and in the launcher.

## Scope

- Native builds only. A Windows build produces a Windows package and a Linux build produces a Linux package; cross-compilation is not
  promised.
- The runtime links no shared libraries of its own, so it needs only the C and C++ runtime libraries of its platform.
- Payloads are not compressed (the pack format stores them as cooked).

## Tests

- `tests/src/editor_distribution_tests.cpp` [editor][integration]: the package layout, the manifest and launcher, the launch from a
  clean folder, the refusals (an unknown start scene, a non-scene start, a bad package name, a folder that is not a package), a
  cancel that leaves nothing behind, and a rebuild that replaces the package.
