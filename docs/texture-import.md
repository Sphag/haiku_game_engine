# Texture Import

The `texture` importer (US-021) turns PNG and JPEG sources into the engine's cooked texture payload: every mip, block-compressed for the GPU. It runs inside the asset converter, so the runtime reads only the cooked file. Code is in `tools/asset_converter/`: `src/texture_importer.cpp` decodes, builds the mips, and encodes; the payload format lives in `engine/assets/` (`src/texture_format.cpp` with `include/engine/assets/texture_format.hpp`) so the runtime can read it; `src/bc5_codec.cpp` is the BC4/BC5 encoder and decoder; `src/bc7_encode.cpp` and `src/bc7_decode.cpp` wrap the vendored BC7 codec in `third_party/bc7enc_rdo`. Decoding uses the vendored `stb_image` in `third_party/stb`, with only its PNG and JPEG decoders compiled.

## Using it

A source is registered with a sidecar whose importer is `texture`:

```json
"importer": {"type": "texture", "settings": {"usage": "color"}}
```

- `usage` sets the rules for the data. `color` (the default) is sRGB. `normal` is a tangent-space normal map. `linear` is any other data, such as roughness, metallic, or a mask.
- `format` is `auto` (the default) or `rgba8`. `auto` chooses BC7 for `color` and `linear`, and BC5 for `normal`. `rgba8` stores uncompressed RGBA8 for any usage. It is the explicit fallback, and both backends can read it without a block format.
- Unknown settings and values other than those above fail with `INVALID_SETTINGS`.

The format is recognized from the bytes. A source that is neither PNG nor JPEG, a file cut short, or one that cannot be decoded fails with `IMPORT_FAILED`, and no output is written for it. A PNG must end with its `IEND` chunk, and a JPEG must end with its end-of-image marker. Each side must be between 1 and 16384 pixels.

## Mip chain

Mip 0 is the full image. Each following mip is a 2×2 box filter of the one before it, down to 1×1. Each usage averages in the space its data lives in:

- **Color** is decoded from sRGB to linear light, averaged, and encoded again. Alpha is averaged as a plain number. Averaging the encoded values would make the mips too dark, so the mip-1 texel of black and white is 188, not 128.
- **Normal** texels are decoded to vectors in [-1, 1], averaged as vectors, and renormalized. Mip 0 is renormalized before the chain is built, so every texel of every mip is unit length to within 8-bit quantization. A zero vector becomes +Z.
- **Linear** data is averaged as stored. Nothing is converted.

Odd sizes are handled by ceil halving. A side of `n` gives `max(1, ceil(n / 2^i))` at mip `i`, so 5×3 gives 5×3, 3×2, 2×1, and 1×1. The last row or column of an odd mip reads its edge texel twice. Partial 4×4 blocks at the edge repeat the last row and column before they are encoded.

## Cooked payload

The payload `OWTX` version 1 holds the usage, the encoding (`rgba8`, `bc7`, or `bc5`), the sRGB flag, and every mip with its size and bytes. The layout, the byte size of each mip, and the rules that tie usage, encoding, and color space together are in `engine/assets/include/engine/assets/texture_format.hpp`. The reader checks every count against the bytes that remain before it allocates. It also recomputes each mip size from mip 0, so a file cannot describe a mip that the runtime would misread.

The cook step wraps the payload in the same `OWCK` container as every other asset. The runtime chooses the sRGB or linear GPU format from the `srgb` flag, so the block bytes are the same for either choice.

## Conventions

- **Normal maps** are stored as the source has them. The engine does not flip the green channel. BC5 keeps the X and Y components, and the runtime rebuilds Z from them, so the stored X and Y must describe a unit vector. Renormalization makes that hold.
- **Color** is sRGB-encoded in the payload, and the runtime samples it through an sRGB format. Linear data is never converted.

## Encoders

- **BC7** comes from the vendored `bc7enc_rdo` at its default quality. Color uses perceptual error weights, and linear data uses equal weights.
- **BC5** is encoded in the converter, as the PRD assigns it. Each channel is a BC4 block: the endpoints are the channel's minimum and maximum, and each texel takes the nearest palette entry. This is simpler than an optimal search, so it is less accurate than a full search would be.

## Not handled

- The decoder reduces 16-bit samples to 8 bits. That case is reported as an `IMPORT_UNSUPPORTED` note.
- EXIF orientation, embedded ICC profiles, and PNG gamma chunks are not read, and the importer does not report them. Authors who need them should bake them into the source.
- Animated PNG and other image formats are rejected.

## Verification

`tests/src/converter_texture_tests.cpp` checks:

- the mip count and every extent for odd sizes, and the byte size of every mip for each encoding;
- payload round trips, and the rejection of wrong extents, byte counts, usage and encoding mismatches, color-space mismatches, truncated files, trailing bytes, and oversized counts;
- hand-worked BC4 palette values and bit packing, the BC5 and BC7 round trips, and the BC5 and BC7 decoders;
- the sRGB mip rule (188 against 128), normal renormalization, odd dimensions for PNG and JPEG, and the size of every mip;
- corrupt, truncated, empty, and unrecognized input, with every prefix of a PNG checked for a clean result;
- identical bytes from identical inputs, and a PNG cooked through `run()` into an `OWCK` container that holds the payload.

Fixtures are in `tests/fixtures/textures`. `generate_fixtures.py` writes the PNGs with the standard library. `gradient_9x7.jpg` comes from `generate_jpeg.ps1`, which uses Windows' System.Drawing encoder because no JPEG encoder is available elsewhere in the repository.

## Performance

A 2048×2048 color PNG cooks to BC7 with 12 mips in about 5 seconds in a Debug build on WSL. The conversion is offline and is not on the runtime path, so the PRD's benchmark contract does not apply to it. `engine_bench` does not exist yet, and this story reports no benchmark result.
