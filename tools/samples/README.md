# Sample project generator

`generate_showcase.py` writes `samples/showcase`, the project that uses every feature area. Python 3.8 or newer, no packages.

```
python tools/samples/generate_showcase.py [output_dir]
```

The output folder is deleted and rewritten, and the result is deterministic: GUIDs come from a fixed namespace and each asset's path,
and nothing reads the clock, so a rerun changes no file. Change the sample here, regenerate, and commit the result together with the script.

The script copies a few fixtures from `tests/fixtures` (models and audio), writes the textures itself (an 8-bit RGBA PNG encoder is in the
script), writes the `.terrain` file in the engine's `OWTR` format, and writes the world partition text files. Their formats are the ones in
`engine/world/src/terrain_edit.cpp` and `engine/world/src/partition.cpp`; `tests/src/showcase_tests.cpp` loads the output through the
engine's own readers, so a format drift fails a test.
