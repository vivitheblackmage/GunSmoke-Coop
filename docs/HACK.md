# How the hack works

Gun.Smoke (NES, UxROM, mapper 2, 128 KiB PRG). Addresses are CPU addresses; `$1:B04F` means bank 1.

## Free space

Measured before choosing places: read tracing over 100000 frames of play (all stages), 20000 frames of attract mode, and 4 × 30000 frames of random buttons with deaths and game over.

- **ROM:** the fixed bank (7) has no free bytes. Bank 4 `$9000–$93FF` and `$B800–$BFFF` (pages of `$00`/`$FF` fill) are never read by the original. `$B430–$B7FF` looks free but is read (data).
- **RAM:**
  - `$0600–$069F` is never written by the original. It is not cleared at power-on, so everything there is set before it is read.
  - Zero page `$D6–$DF` is cleared at power-on and never used afterwards (`$D4/$D5` belong to the sound engine). The mode (`$D9`) and `P2RAM` (`$DF`) live there because they are read before the title.
- **Stack:** task 0 goes down to S = `$1C`. The trampoline adds at most about 10 bytes.

## Room in the fixed bank

The pad and demo code (`$C15A–$C208`, called only from `$C067`) moves unchanged to bank 4, with pad 2 added. Its 175 bytes then hold:

- the trampoline `tramp`: saves A and Y, pushes the mapped bank, maps bank 4, calls `dispatch`, then maps the bank back;
- the demo's two exits to the title (`exita`/`exitb`);
- the end of the second player's frame (`stubf`, with `$C733` first when she comes back) and the task switch hook (`rsm`);
- fast paths that run the original instructions directly with 1 PLAYER, so the game keeps its speed alone;
- `objs`: `jsr $C945` / `jmp tramp`, so objects are drawn in their own bank.

The dispatcher tells the hooks apart by the trampoline's return address (the most frequent first). A handler can change that address (`setret`) to continue elsewhere, like the original's own `pla/pla` exits.

## Two players in one game

The original's player is a RAM block:

- `$70–$88`: position, hit, horse, boots, rifle, lives, blinking, invulnerability, walk, shots, weapon;
- the weapons at `$90–$9F` (four bytes each);
- the pad at `$F5/$F7`.

The other player has a saved copy at `$0600`, and the same original routines run a second time with her block swapped in. Player 1 is in RAM unless `P2RAM` (`$DF`) is set. The players trade places for good when the one in RAM dies while the other plays on, because the game loop always runs the RAM player. The pads follow the players.

| Hook | The original | The handler |
|---|---|---|
| `$1:B04F` lda #$22 / sta $06A0 | the high score line's VRAM script | `h_menu`: the menu's script at `$0650` (both lines and the cursor), the high score line chained after it |
| `$1:B0AE` cmp #$10 / bne $B079 | the title loop | `h_title`: up/down/SELECT move the cursor, START picks the mode, and the chosen line blinks |
| `$1:B1C0` (data) | PUSH START in the title script | `1 PLAYER` with the cursor |
| `$C067` → `$C15A` | the pad, the demo | `h_pad`: pad 1 to player 1's block. With 2 PLAYERS, pad 2 from `$4017` to player 2's block. START on either pad pauses, except for a player who is out: their START ends the life code |
| `$C03C` lda $21 / bne | a task resumes | `h_rsm`: the boss task's target restored. While a boss fights, the boss tasks get the player nearest the boss |
| `$F011` jmp $F24C | the end of the player's frame | `stubf`: the other player's frame (`$EB82`, `$ED89`, `$EF13`) with her block; `$C733` first when she comes back; friendly fire |
| `$C945` lda #$18 / sta $A1 | the objects' first sprite | the other player's sprites; the lives in the corners first (six sprites) |
| `$F256` lda $0400,x | each enemy's handler | `h_target`: the nearest player while it runs |
| `$F27E` lda $41 / jsr $C361 | the enemies done | the target restored |
| `$F286` lda $76 / lsr | hit, horse, bomb | `h_hit`: the other player against the objects; her hit means death in place (`die_saved`). A hit on the RAM player while the other plays on: trade places, then death in place. Otherwise the original's death |
| `$E91D` jsr $C353 | the game loop's start | `h_loop`: shop purchases to both; the dying player's countdown (figure off, back with lives left); the life code typed: a life from the other player (`gift`) |
| `$E869` jsr $E88D | the player enters (stage, after the lives screen) | the other player with lives enters beside |
| `$F3B6` dec $7A / beq | a life lost (the original's death) | with no lives: the other player with lives takes the RAM place and the lives screen; game over when nobody has lives |
| `$E72D` lda #3 / sta $7A | a game starts or continues | player 1 in RAM, three lives each |
| `$F2DC` ldy $A5 / bne | shop / status window | the other player's window with her block |
| `$C500`, `$F359`, `$C4C4`, `$F3DF` | palette loaded; the death and lives screen sprites | her own colours; her death figure and lives screen in her palette |

**Death in place.** The player who was hit shows the original's death figure (object `$35` in slot 0, which follows the scroll) for 72 frames. Then they come back blinking where the original puts a player after a death (line `$BC`, the first free place from the middle). The dying player loses one step of their boots and rifle (the original's `$CC67` on their block).

**The last player alive** dies the original way: the fade, the lives screen, then the stage again.

**The horse.** The original's death (`$F379`) clears the stage's objects, the horse included. Before the fix, this could happen while the other player was riding (`$77`): she rode an invisible horse. Now the original's death only runs when nobody else is in play, and a returning player has `$77` = 0.

**Objects in the right bank.** Object frames can live in the mapped bank (the attract mode's figures in bank 1, a stage's objects in its bank). The original draws them (`objs`) before the trampoline maps bank 4. A test requires that no object is drawn with bank 4 mapped.

**Optimisation.** The block swap is unrolled for `$70–$88` and `$F5/$F7`. The other player's contacts are tested once per frame after the objects, with a cheap distance test before the block is swapped. The shot mask is computed only with friendly fire on.

## Variants

`tools/gunsmoke-coop/variants.mjs` lists the supported ROMs. Only the title differs between them.

Gun.Smoke (Europe) differs from USA in:

- bank 6's tiles;
- the player's movement tables (`$DF91–$E118`, larger for 50 Hz);
- `bne`→`bcc` at `$A03C`;
- the title text.

`build.mjs` checks every variant present against what the patch relies on:

- the fixed bank equals USA's except the movement tables, the texts and `$FFF0/1`;
- the hooked title code is the same;
- every hooked byte is the same;
- bank 4's places are fill.

A PAL variant's patched file gets an NES 2.0 header with PAL timing (`PAL_HEADER`).

## Debug states

`mesen-states.lua` runs in Mesen 2's test runner:

- It picks 2 PLAYERS on the title and keeps that moment.
- For each slot it loads that moment again. At the stage load `$E757` it sets the stage `$41`, as if the earlier stages were cleared. Nothing else changes.
- It prints the states as hex on stdout, so Mesen's script file access can stay off.
- The patcher or `make-states.mjs` writes them as `<ROM name>_<slot>.mss` in Mesen's SaveStates folder.
