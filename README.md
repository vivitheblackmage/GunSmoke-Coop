# Gun.Smoke Co-op

A patch that turns Gun.Smoke for the NES into a game for one or two players at the same time. The patched game runs on a real NES, in emulators and on the MiSTer.

**Download:** `GunSmoke-Coop-Patcher.exe` from the [Releases](../../releases) page. Windows 10/11 x64, a single file, no install.

**No game data is included.** The patcher only contains the patch's own code. Bring your own ROM: the patcher checks it and writes a patched copy where you choose.

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

## Known limits

- With two players, frames are heavier, and there are more slow frames than in the original.
- There is one horse. If both players ride, they share it.
- The lives display uses six sprites on one line. When both players stand at the very bottom, that line can have more than eight sprites, and the last ones disappear (the NES limit).
- The corner number counts the life being played, like the original's lives screen: 1 means the last life, 0 means out.
