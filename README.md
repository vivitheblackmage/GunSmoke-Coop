# Gun.Smoke Co-op

A patch that turns Gun.Smoke for the NES into a game for one or two players at the same time. The patched game runs on a real NES, in emulators and on the MiSTer.

**Download** from the [Releases](../../releases) page:

- `GunSmoke-Coop-Patcher.exe`: the patcher with options and a preview. Windows 10/11 x64, a single file, no install.
- `Gun.Smoke (USA) (Co-op).ips` and `Gun.Smoke (Europe) (Co-op).ips`: plain IPS patches with the default options (friendly fire off, player 2 in palette 2), for any IPS patcher on any system. The European patch also writes the PAL header. Apply each patch to an unmodified ROM with the SHA-256 listed below.

**No game data is included.** The patcher and the patches only contain the patch's own code. Bring your own ROM: the patcher checks it and writes a patched copy where you choose.

## Features

- The title screen has **1 PLAYER** and **2 PLAYERS** with a cursor. Up, down or SELECT moves it, START picks.
- With 1 PLAYER the game is the original.
- With 2 PLAYERS both players start together, each with **three lives**, and share one score.
- A player who is hit dies in place and is back a moment later. There is no black lives screen while the partner is still playing.
- Remaining lives are shown in the bottom corners: player 1 on the left, player 2 on the right.
- Out of lives? Type **A, B, B, A, START** on your controller to take a life from your partner, if they have one to spare.
- Upgrades bought in a shop go to both players. Each player picks their own weapon in their status window.
- Enemies and bosses aim at the nearest player.
- Options in the patcher:
  - friendly fire (off by default);
  - which sprite palette player 2 uses, and optional custom colours for player 2.

## Supported ROMs

| ROM | SHA-256 of the ROM without its 16-byte header |
|---|---|
| Gun.Smoke (USA) | `f39421a126f3b93caa37d6c3ed899840ddfd51b0587446ca459f72564aca1433` |
| Gun.Smoke (Europe), PAL | `f82e65b91b7ce609146bc8c3975f2c1780fe965c1208695892a2be2cb281616e` |

Any other ROM is refused with "Unsupported game revision".

Patched PAL games get an NES 2.0 header that marks them as PAL, so emulators and the MiSTer run them at 50 Hz.

## Using the patcher

1. Start `GunSmoke-Coop-Patcher.exe`.
2. Choose your ROM, or drop it on the window.
3. Pick the options. The picture on the left is drawn from your ROM and shows player 2 in the colours you chose.
4. Press **Patch ROM…** and choose where to save the patched copy.

Mouse and keyboard both work: Tab or the arrow keys move, Enter picks, left and right change a setting.

### Mesen states (optional)

**Mesen states** makes Mesen 2 save states for the patched ROM, so you can test the stages without playing through:

- F1–F6 load stages 1–6, with 2 PLAYERS and three lives each;
- F7 loads stage 1's boss.

The patcher asks for your `Mesen.exe`. Save states that already exist are kept as `.bak`. The states belong to the patched file's name. If you patch again with other options, make them again.

### Command line

```
GunSmoke-Coop-Patcher --verify <rom> <patched rom>          compares the patcher's output with a patched file
GunSmoke-Coop-Patcher --states <Mesen.exe> <patched rom>    makes the Mesen states without the window
GunSmoke-Coop-Patcher --preview <out.bmp> [rom]             saves a picture of the window
```

## Building from source

### The patcher (Windows, MSVC)

Requires Visual Studio with C++ and CMake (tested with Visual Studio 2026). SDL 3 is downloaded and linked statically unless CMake finds an installed copy.

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

The result is a single file: `build/GunSmoke-Coop-Patcher.exe`.

### The patch (Node.js 20 or later)

The hack is 6502 assembly in `tools/gunsmoke-coop/coop.asm`, assembled by a small assembler in `tools/asm65816.mjs`. Put your ROMs in this folder (they are git-ignored) and run:

```
node tools/gunsmoke-coop/build.mjs
```

This regenerates `tools/gunsmoke-coop/patch_data.hpp`, the patcher's data. For each supported ROM present, it also writes into `out/`:

- the patched ROM;
- an IPS patch;
- `coop.sym`, the labels.

Then rebuild the patcher. Set `GS_FRIENDLY_FIRE=1` to build the ROMs and IPS patches with friendly fire on.

### Tests

```
node tools/gunsmoke-coop/test.mjs ["<rom>"]
```

`test.mjs` runs the original and the patched ROM in a small NES CPU model (`tools/nes/`). It defaults to the USA ROM. It checks:

- one player behaves like the original;
- the attract mode is unchanged;
- the title menu;
- the horse, and taking a life from the partner;
- full two-player games until game over.

`tools/gunsmoke-coop/mesen-coop.lua` plays the same in Mesen 2's test runner.

### Layout

| Path | What |
|---|---|
| `tools/gunsmoke-coop/coop.asm` | the hack (6502) |
| `tools/gunsmoke-coop/build.mjs` | builds the patch for every supported ROM and checks where it writes |
| `tools/gunsmoke-coop/variants.mjs` | the supported ROMs |
| `tools/gunsmoke-coop/patcher.cpp` | the patcher's window |
| `tools/gunsmoke-coop/patch_data.hpp` | generated: the hack's bytes |
| `tools/gunsmoke-coop/test.mjs`, `*.lua`, `make-states.mjs` | tests and Mesen scripts |
| `games/gunsmoke/`, `src/compat/nes/` | a re-implementation of the game's engine, used only to draw the patcher's preview from your ROM |
| `docs/HACK.md` | how the hack works |

## Known limits

- With two players, frames are heavier, and there are more slow frames than in the original.
- There is one horse. If both players ride, they share it.
- The lives display uses six sprites on one line. When both players stand at the very bottom, that line can have more than eight sprites, and the last ones disappear (the NES limit).
- The corner number counts the life being played, like the original's lives screen: 1 means the last life, 0 means out.
