# Texture export

Extracted, viewable copies of the game's runtime textures (24-bit BMP), used to
assess and prototype higher-resolution textures. This folder is **git-ignored**
(the dumps are large — ~0.5 GB for all levels).

## What's here

- `all_levels/` — every unique texture instantiated while cycling through the 8
  game levels, one BMP per texture, de-duplicated by pixel content.
  - Filenames: `<contentHash>_<W>x<H>_<bpp>bpp.bmp`
  - The 16-bit (`16bpp`) files are the real in-game colour textures (the software
    renderer runs in 16-bit 565; decoded correctly to RGB).
  - The handful of `8bpp` files are solid-colour placeholder textures.

## Key finding

~57% of textures are exactly **256×256** — pinned at the engine's texture cap
(`TextureManager` rejects >256; the pack surface is 256-wide). That is the
headroom for a higher-resolution texture upgrade.

## How to regenerate

The dumper lives in the game (diagnostic hooks, kept intentionally):

- `Lib/Renderer/Texture.cpp` — `DumpTextureToBMP()` writes each texture's base
  mip to `tex_dump/` (relative to the game's working dir, i.e. the `data/`
  folder). Gated behind the `TRESPASS_DUMP_TEX` environment variable.
- `Trespass/gamewnd.cpp` — **Ctrl+Shift+L** in-game cycles to the next level
  (`be → jr → ij → lab → it → as → as2 → sum`) so every level's textures stream
  in during one session.

Steps:

1. Set `TRESPASS_DUMP_TEX=1` in the environment, launch the Release build.
2. Start a New Game, then press **Ctrl+Shift+L** to advance through all 8
   levels, pausing on each so textures stream in.
3. Textures land in `…/Release/data/tex_dump/`; move them here.

Without `TRESPASS_DUMP_TEX` set, the hooks are inert (no files written); the
Ctrl+Shift+L level-warp still works and is handy for testing.
