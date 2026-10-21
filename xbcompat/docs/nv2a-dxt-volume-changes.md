# Compressed volume texture upload

DXT1, DXT3 and DXT5 volume mip levels now decode into tightly packed RGBA bytes before normal OpenGL 3D texture upload. Existing compressed 2D and cube uploads retain their previous path. The temporary decoded buffer is freed after upload; the normal texture cache owns the resulting GPU image.

The decoder was independently written from the [Khronos S3TC format specification](https://registry.khronos.org/OpenGL/extensions/EXT/EXT_texture_compression_s3tc.txt), Appendix. No reference-emulator decoder source was read or copied by the implementer. The same specification prohibits S3TC for true three-dimensional compressed texture uploads; its ES3 allowance applies to texture arrays, which do not provide volume filtering.

Volume block order comes from the sanitized reference-model specification: depth slabs hold up to four slices; XY block positions advance in row order; each position stores its slice blocks consecutively. A short final slab stores its actual slice count. This layout has not been independently measured on NV2A hardware. Logical dimensions remain distinct from block-rounded storage dimensions, and the existing mip-offset calculation advances by the packed block footprint.

Endpoint expansion and interpolation produce eight-bit channels. Exact low-bit agreement with every hardware decoder is not claimed; driver decoding of the same format can differ in rounding. DXT1 transparency and the separate DXT3/DXT5 alpha encodings are preserved.

`tests/dxt_test.c` checks explicit color and alpha palettes, transparent DXT1, both DXT5 alpha cases, multi-tile/slab addressing, cropped edges with buffer guards, undersized inputs and overflow rejection. Its non-power-of-two volume fixture exercises the generic decoder API, not an assertion that Xbox texture creation accepts those dimensions.

The renderer integration fixture samples distinct slices/XY blocks of a power-of-two compressed volume and a smaller mip through the HLE path. Execution results are recorded separately.

## LockBox boundary

The upload and regression fixtures use whole mip locks with a null box. Existing nonzero sub-box pointer arithmetic does not implement the compressed volume slab layout; arbitrary compressed-volume sub-box locks remain unverified and are not claimed fixed by this change.
