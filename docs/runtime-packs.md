# Standalone Runtime and Resource Packs (US-060)

The standalone game runs from cooked files only. It needs no source assets, no build tree, and no editor. Code is in
`engine/pack/` (the pack format), `engine/runtime/` (the `game_runtime` executable and its option parser), and
`tools/asset_packer/` (the `asset_packer` command-line tool). The runtime links `engine_pack`, `engine_assets`, and
`engine_scene`. It never links `engine_converter`, the vendored codecs, or Assimp (FR-03). A configure-time check in
`engine/runtime/CMakeLists.txt` walks the link graph and fails the configure step if one of them appears.

## Flow

1. `asset_converter cook --project <dir> --output <cooked>` writes `<guid>.cooked` for each asset.
2. `asset_packer --project <dir> --cooked <cooked> --root <start-scene guid> --output <file>.pack` packs the roots and their
   dependency closure.
3. `game_runtime --pack <file>.pack --start-scene <guid> [--backend dx12|vulkan] [--profile native|full]` opens the packs
   and starts the scene. The working directory does not matter.

## Pack format

The layout is in `engine/pack/include/engine/pack/pack.hpp`. In short: a 48-byte header (magic `OWPK`, format version,
index size, SHA-256 of the index), a canonical JSON index, then the payloads. Each payload is a full `<guid>.cooked` file, so
the runtime reads it with `read_cooked_container()`.

- **Index.** `roots` holds the assets the pack was built for. `entries` holds, for each asset in the closure: `guid`,
  `source`, `hash` (SHA-256 of the payload, the asset's version in the pack), `offset`, `size`, and `dependencies`.
- **Closure.** A pack holds every asset reachable from its roots through dependency edges, and nothing else.
- **Missing dependency.** A reachable dependency that is not registered, or whose cooked file is missing, aborts the build
  (`MissingDependency`, `MissingCooked`). No partial pack is written. `UnknownRoot` and `NoRoots` abort too.
- **Determinism.** Entries are in GUID order, so the same inputs give the same bytes whatever the order of the roots.

## Integrity

`PackReader::open` verifies a whole pack before any of it is used. It checks the magic, the version, the index hash, the
JSON and format name, every payload range (payloads must tile the file, with no gap and no trailing bytes), every payload
hash, and that every root and every dependency of every entry is an entry of the pack (`IncompleteClosure`). A damaged or
incomplete file is refused as a whole.

## Runtime

- The runtime refuses a pack GUID that appears in two packs, a start scene that is not a root of one of the packs, and a
  scene that names an asset outside the packs. Each check runs before the scene is loaded.
- The scene loads through `ComponentCatalog`, which this build leaves empty. A scene with no components loads; a scene that
  names a component is rejected rather than loaded partly. A game adds its component types to that catalog.
- The backend defaults to `dx12` on Windows and `vulkan` elsewhere. The profile defaults to `native` (PRD FR-09). Both are
  validated. The runtime records them in its start message; the render path that uses them is not part of this build.

## Tests

- `tests/src/pack_tests.cpp` [unit]: the closure, the byte layout, round trip with hashes, order-independent output, cycles,
  a missing dependency or cooked file, unknown roots, damaged payloads and indexes, truncation, wrong magic, version, and
  format, an incomplete closure, and payloads that do not tile the file.
- `tests/src/runtime_options_tests.cpp` [unit]: accepted values, defaults, and the usage errors.
- `asset_packer` and `game_runtime` CLI contracts (integration): help, usage errors, a missing dependency that aborts,
  a missing pack file.
- `engine_pack_runtime_round_trip` (integration, `tests/pack/PackRuntimeRoundTrip.cmake`): cooks `tests/fixtures/pack/project`,
  packs its start scene, starts the packed game from an empty directory, and refuses a non-root start scene.

## Not yet covered

- The Build Game flow (validation, cooking, packing, the package layout, and the launch check) is in `docs/distribution.md` (US-073).
- Compression of payloads (the PRD lists zstd for pack compression; this format stores payloads uncompressed).
- The runtime does not open a window or start a frame loop. The lifecycle and the scene load are the parts this story
  covers.
