# Test fixture provenance

Every file under `tests/fixtures` is listed here with its SHA-256, its source, and its license. The `engine_tests`
unit case `Fixtures: every file has provenance and an unchanged hash` reads this table, hashes each file, and fails on a
missing, extra, or changed file, so a fixture cannot change without this table changing with it.

## Licensing

Every fixture is authored for this project: it is either hand-written here or produced by one of the project's own scripts
or ffmpeg commands listed in the table. No third-party file, sound, or model is included, and nothing is downloaded to build
or run the tests.

The repository has no `LICENSE` file yet, so the license of these fixtures is the project's license once one is chosen. That
choice is open; see `docs/benchmarks.md`.

## Files

| File | SHA-256 | Source | License |
|---|---|---|---|
| `assets/registry_project/materials/стена.mat` | a6d289b2c93f0f8b06e94c2488326ce653600dcaf56744c9b5417d3ae6b6439a | Hand-written project for the asset registry tests | project-authored |
| `assets/registry_project/materials/стена.mat.meta` | e40b5bdb8ef54d5f344213f4fef43b04ad36fc4dd2f1a206e78cd338856c87f4 | Hand-written project for the asset registry tests | project-authored |
| `assets/registry_project/notes/readme.txt` | e089cebdd34629e14aaf1c8230b172ce3c48e3ac4f75868ab117ee94cb67f1a7 | Hand-written project for the asset registry tests | project-authored |
| `assets/registry_project/scenes/hall.scene` | 67bc439cbeb83913bec158c44a25a5be893c5ceb3f0a74bd701c841f8a7fa75d | Hand-written project for the asset registry tests | project-authored |
| `assets/registry_project/scenes/hall.scene.meta` | 534c9876b7b40a90362abfad9849d310b9b4eec23435feb6e9ed1da4cc8260fe | Hand-written project for the asset registry tests | project-authored |
| `assets/registry_project/textures/brick.tex` | 37dc955c4901e68fc6b8041fc3ec9a93984039ecdeea110e1b9864f9d44fa10e | Hand-written project for the asset registry tests | project-authored |
| `assets/registry_project/textures/brick.tex.meta` | 3a39c0331c9d7bc31b8d87906a0d78e7391ef692f8dbfa372afb197ff085e25d | Hand-written project for the asset registry tests | project-authored |
| `audio/README.md` | 36e05f55b1fde3829faf7ab8d6e1a69eaf4f9e7e578a81480a7b9895e029770c | Hand-written note on the audio fixtures and their commands | project-authored |
| `audio/tone_mono.ogg` | 302eb47931afab6bd3862040ad1043c59bd2d7890ed447177c1b3f86a1c1bf41 | ffmpeg lavfi sine, command in `audio/README.md`. WAV reproduces byte for byte; Ogg decodes to identical PCM (stream serial and page checksums differ) | project-authored |
| `audio/tone_mono_s16.wav` | 6eae90b9b31e00df661c8f317c159308002f93d3b372b6346e3f8d3a26a48900 | ffmpeg lavfi sine, command in `audio/README.md`. WAV reproduces byte for byte; Ogg decodes to identical PCM (stream serial and page checksums differ) | project-authored |
| `audio/tone_stereo.ogg` | dd4a7f705a8edd77560748ca666433df1bde9abdc82ee048a25fafc2c71869be | ffmpeg lavfi sine, command in `audio/README.md`. WAV reproduces byte for byte; Ogg decodes to identical PCM (stream serial and page checksums differ) | project-authored |
| `audio/tone_stereo_f32.wav` | 124d14d6af805ff9937cccd300cf254026aa802def7b008205b063829f1e5dfa | ffmpeg lavfi sine, command in `audio/README.md`. WAV reproduces byte for byte; Ogg decodes to identical PCM (stream serial and page checksums differ) | project-authored |
| `converter/project/config/settings.json` | b07e5f82b1c5d39f9b72af438dc119312be2d0a2a8d03a67d7b5b91d0575071d | Hand-written converter project (config, data, notes) | project-authored |
| `converter/project/config/settings.json.meta` | c5dbd8283d3c944c65062b83089e7f80ea590483a20d07cd668731b7187ea2c3 | Hand-written converter project (config, data, notes) | project-authored |
| `converter/project/data/blob.bin` | 211d7694e0c919e7a8d647a02639ea6fe6a343c92de8e3fc505529fc3c85d944 | Hand-written converter project (config, data, notes) | project-authored |
| `converter/project/data/blob.bin.meta` | 4c8334ac184c8de0a4d23f7467d35cffccb1bd2c717180e4a180d9ded1a670d7 | Hand-written converter project (config, data, notes) | project-authored |
| `converter/project/notes/readme.txt` | 13187ebc90c47a525637071656826b946089b1806bc3c94555c6acb529ab0bf8 | Hand-written converter project (config, data, notes) | project-authored |
| `converter/project/notes/readme.txt.meta` | 821e3dda2597d7b1c9a817241440a6115c9d9aff9bfad59113fed13d8cbe9eea | Hand-written converter project (config, data, notes) | project-authored |
| `core/text/привет_мир.txt` | 234213c5aad789362f0601772f44a1c3a2c4fb4d353be0eefd275a4df1fb8633 | Hand-written UTF-8 text with Cyrillic, for the path and file tests | project-authored |
| `input/shared_replay.txt` | c94003822163d7198762ac7f1c3f561ff43cf0d3d6d101c0c0f252ae22723071 | Hand-written shared input replay (US-010); see the file header | project-authored |
| `models/fbx_cm_yup.fbx` | b08ffd765b8e165e39f16b77a2f59108115a6a4a66266b5b2796d946e088efb8 | `models/generate_fixtures.py` (run with `python3 -I`); reproduces byte for byte | project-authored |
| `models/fbx_m_zup.fbx` | 8840dc39984d5074eeee69bbd8bbff0928d010ed6fbe3616b357e93d830279cd | `models/generate_fixtures.py` (run with `python3 -I`); reproduces byte for byte | project-authored |
| `models/generate_fixtures.py` | 7af6538b8b06784015a0f6ebc0a3b65e095b666c9e43ebe50330e6e995d4714a | Hand-written generator script in this directory | project-authored |
| `models/triangle.obj` | 2fccdbc0cb978264ab5f97fc8efaade17a05cdb8c771408bc7b6c40a7df8e26b | `models/generate_fixtures.py` (run with `python3 -I`); reproduces byte for byte | project-authored |
| `models/triangle_tree.glb` | f2060cb38431613c58cd8120916ee3deee79e11d9f3d8065b94638cb525d58d3 | `models/generate_fixtures.py` (run with `python3 -I`); reproduces byte for byte | project-authored |
| `models/triangle_tree.gltf` | cdf0b26599077a244323d483558d47f08ca7d09cc762a88fb35d3f7ecd9aea78 | `models/generate_fixtures.py` (run with `python3 -I`); reproduces byte for byte | project-authored |
| `pack/missing_dependency/project/scenes/start.scene` | aa98c52581889b4e85dfbcf4f26c8feab5842a2294b037e3db5101fb7a1cc46d | Hand-written pack project for the pack tests | project-authored |
| `pack/missing_dependency/project/scenes/start.scene.meta` | c236097acfe339ce333225d20baa40e5370a9372dd6901aca3a9596915008340 | Hand-written pack project for the pack tests | project-authored |
| `pack/project/data/palette.bin` | 0cfad56987f8d4b0f5f0b62acd5355c2dfd791f153b706f0db2094fa812966f1 | Hand-written pack project for the pack tests | project-authored |
| `pack/project/data/palette.bin.meta` | 1c1d9d219223ae267989a66c7418e212504cc8ecef31678264dbf4703682883a | Hand-written pack project for the pack tests | project-authored |
| `pack/project/scenes/start.scene` | ae46f71b921afb9370934f8180dce8f69685f87d31d9b3404c95941407d14ef3 | Hand-written pack project for the pack tests | project-authored |
| `pack/project/scenes/start.scene.meta` | fa90a171338e7767b61aa98c264ed35635c81740a5fe1de5b4f77bfb35ac0952 | Hand-written pack project for the pack tests | project-authored |
| `textures/generate_fixtures.py` | 534cd58052e2556c10c6566e690ef92fe94f04fa84db88654bacea5fe1609bfa | Hand-written generator script in this directory | project-authored |
| `textures/generate_jpeg.ps1` | eeb0f4c4b2cc4ad4e58539d3386d6e414b397abb187bbc28da94cb5573fa4952 | Hand-written generator script in this directory | project-authored |
| `textures/gradient_4x4.png` | 429945ba0265343c83524faf33b44fd0dc0caa2b3d1bb3190183cc1bdd5316cd | `textures/generate_fixtures.py` (standard library); reproduces byte for byte | project-authored |
| `textures/gradient_9x7.jpg` | 3915f0228552d323fe0a984f690b31ffda393b3e9baa58c6025ffe2f47b7402a | `textures/generate_jpeg.ps1` (Windows System.Drawing, quality 100); the JPEG encoder is platform-specific, so it is not regenerated here | project-authored |
| `textures/normal_5x3.png` | 8fd89b5c343295ea9d13a8fc6d5ef1c8aa9ebd83e31732a348e3e06e1d32708f | `textures/generate_fixtures.py` (standard library); reproduces byte for byte | project-authored |
| `textures/sixteen_bit_2x2.png` | 7f10cf3dc68990d50ac0756b453ac0d286f112036c426af4fbf30ef087aa4fc7 | `textures/generate_fixtures.py` (standard library); reproduces byte for byte | project-authored |
| `textures/srgb_2x2.png` | 8cbf621d0583aaa70fc4987a9b81f8420fd0d0dfda4d24047c91d3c5f721c2fa | `textures/generate_fixtures.py` (standard library); reproduces byte for byte | project-authored |
