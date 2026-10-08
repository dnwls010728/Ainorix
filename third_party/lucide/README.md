# Lucide icons

`lucide.ttf` is the icon font of [Lucide](https://lucide.dev) from the npm package `lucide-static`
0.460.0 (`font/lucide.ttf`), unmodified, ISC license (see `LICENSE`; portions from Feather, MIT).
The editor embeds it at build time (CMake writes `build/.../generated/EditorIconFont.cpp`) and merges
its glyphs into the UI font; the icons it uses are listed with their codes in
`engine/editor/EditorIcons.h` (codes from the same package's `font/info.json`). Games never link it.
SHA-256: 55e868f053500d4e4298fac5515f2d4f63a10aa7b263b022ff96c126cd9e95dc
