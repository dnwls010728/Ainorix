# Packaged game data

What `oe package` ships of a project and how far it is kept from prying eyes.

```bash
oe package build/demo                  # dist/demo/demo.exe + game.pak
oe package build/demo --loose          # dist/demo/demo.exe + game/ (the files as they are: debugging, modding)
oe package build/demo --script-lines   # keep file and line in script error messages (bytecode not stripped)
oe package build/demo --web            # dist/demo-web/ (index.html, oe_player.js/.wasm, game.pak)
oe package build/demo --android        # an APK with assets/game.pak
```

## What is in a package

Every file of the project except dot files and folders, `AGENTS.md` / `CLAUDE.md`, `tools/`, `web/` and `server/`
(`GameFiles`). They go into one archive, `game.pak`:

- **Scripts are stripped bytecode.** Each `.lua` that compiles is stored under its own path as Lua bytecode with
  its debug information (local and upvalue names, line numbers, the file name) left out, so a decompiler recovers
  less; the source text does not ship. A runtime error from a stripped script has no file or line — pass
  `--script-lines` to keep them while testing a release. A file that does not compile ships as it is with a warning.
- **Files are packed** (zlib) where that saves at least a twentieth of their size: scenes, scripts, uncompressed
  models and sounds shrink, PNG/JPEG images and fonts stay as they are. `games/RPG`: 9.0 MB of files, a 2.6 MB pak;
  the template game (mostly WAV): 188 KB, 153 KB.
- **The archive is enciphered** (ChaCha20, per file) with a key made from a salt in the pak and the game's master
  secret, and every file carries a hash that is checked when it is read. An archive tool, a text editor or a hex
  viewer shows nothing of the game: no file names, no scene text, no images.
- **Each game has its own key.** `oe package` makes a 32-byte master secret the first time and keeps it in
  `<project>/.oe/package.key` (a git-ignored dot folder), so the same project always packs the same way. The key is
  written into a slot in the copied player binary (the committed runtimes ship the slot all zero, meaning the
  engine's built-in seed). A pak built for one game cannot be read by another game's player, and the engine source
  alone does not open it. Keep `package.key` to keep making paks the current players can read; losing it only means
  the next package is keyed differently. `--loose` has no pak and no key.
- **The players read it in place.** On Windows and the web the pak is mounted as the folder `game/`
  (`MountPakFile` in `core/FileSystem.h`): nothing is unpacked, and a loose file put beside it does not replace a
  packaged one. On Android the pak inside the APK is unpacked into the app's private data folder on the first
  start, as before (a folder other apps cannot read without root).

The same project packs to the same bytes.

## What this does and does not protect

It stops casual copying: browsing the folder, reading scripts and scenes in an editor, swapping a texture or a
value to cheat, and running a game's files through another OwnEngine player. It does not stop someone determined:
the key is in that game's player, so the player can be taken apart, and whatever the game draws or plays can be
captured from memory or the GPU. No engine that ships assets to the
player's machine can prevent that; server-side rules (docs/NETWORK.md, authoritative mode) are the answer for
anything that must not be tampered with. Lua bytecode can be decompiled into readable code without the comments.

`--loose` (or starting the player with a folder: `Name.exe path/to/project`) keeps the old layout. A player started
without a `game.pak` beside it reads `game/`.

## Format

`engine/core/Pak.h`:

```
"OEPAK002" | salt[16] | u32 index size | index | data
index: JSON {"files":[{"path","offset","size","hash","packed"?}]}, enciphered; offsets from the start of the data
packed: the file is a zlib stream of that many bytes; size is the plain size
```

The master secret is the game's key (`.oe/package.key`) or, for an unkeyed build, a constant in the engine; the
per-pak key is derived from it and the salt. A player binary carries the key in a 16-byte-marked slot that
`EmbedPakKey` fills (`core/Pak.h`).

A file is read by deciphering it, unpacking it and checking its hash. The compressor is the PNG encoder's (one
fixed-Huffman block, greedy LZ77: `ZlibPack` in `core/Image.h`); `ZlibUnpack` reads any zlib stream.
`OEPAK001` (packages made before this: no salt, no cipher) is still read. Paths are matched exactly and then
without regard to ASCII case, as a Windows folder would.

## Status

Verified: tests `ZlibPackAndUnpack` (also streams made by zlib itself), `PakKeysAndStrippedBytecode` (a keyed pak
opens only with its key; the slot embeds and reads back; stripped bytecode drops the file name), `GamePakMountAndBytecode`
(the packaged Hello project renders the same frame as its folder) and `AndroidGamePakExtract`; `navlab` packaged
for Windows with its own key (slot patched into the exe, pak unreadable with the engine seed) and run; `navlab` and `games/RPG` packaged for Windows and run from `game.pak`; the web package in
headless Edge. Unverified: the Android runtime was rebuilt but not run on a device or emulator. The cost of unpacking at load time was not measured
(`games/RPG` reached its first frame in the same half second with and without it). Not implemented:
dynamic Huffman codes in the compressor (smaller paks), a key handed in by the project instead of a generated one,
key obfuscation in the player beyond the plain slot, binary scenes, engine-specific texture and model formats.
