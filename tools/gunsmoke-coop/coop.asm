; Gun.Smoke (NES, USA) two-player patch (docs/HACK.md). Built by tools/gunsmoke-coop/build.mjs with
; tools/asm65816.mjs (explicit .b/.w sizes; {NAME} placeholders, {<NAME}/{>NAME} their low/high byte, are filled in by
; build.mjs).
;
; The title screen offers 1 PLAYER / 2 PLAYERS (a cursor, the rifle tile). With 2 PLAYERS both players start, each with
; three lives; the score is shared. A player who is hit while the other plays on dies where they stand (the death figure
; for 72 frames) and comes back blinking at the original's place (line $BC, the first free place from the middle); the
; last player in play dies the original's way (the fade, the lives screen, the stage again). Each player's lives are
; shown in a bottom corner as x and a digit (player 1 left, player 2 right).
;
; The original's player is the RAM block $70-$88, the guns $90-$9F and the pad $F5/$F7; the other player has a saved
; copy ({SAVE}...) and the same original routines run once more with that block swapped in (as the native port's co-op,
; games/gunsmoke). Which player is in RAM: player 1 unless {P2RAM} (the players exchange their places when the one in
; RAM dies while the other plays on). The fixed bank is full: the original's pad and demonstration code ($C15A-$C208)
; moves to bank 4 and its place holds a trampoline that calls bank 4 and returns to the bank that was mapped; each hook
; is a "jsr tramp" that the dispatcher tells apart by its return address. Bank 4's code is at $B800-$BFFF and
; $9000-$93FF (both never read by the original).

; ---------------------------------------------------------------------------------------------------- fixed bank
org $07C15A
; The trampoline: A and Y kept ({TA}/{TY}, a handler may change them), X as the handler leaves it, the carry from
; the handler; the mapped bank back afterwards (a handler may change the pushed byte at $0103 + S).
tramp:
sta.b {TA}
sty.b {TY}
lda.b $40
pha
lda.b #$04
jsr.w $C361
jsr.w dispatch
pla
jsr.w $C361
ldy.b {TY}
lda.b {TA}
rts
; $F256 (lda $0400,x): an active enemy slot with the other player in play through the trampoline.
chk:
lda.b {ACT}
beq chk_off
lda.w $0400,x
bpl chk_r
jmp.w tramp
chk_off:
lda.w $0400,x
chk_r:
rts
; $F011 (jmp $F24C), the end of the player's frame: in two-player play the other player's frame ($EB82 move, $ED89
; shots, $EF13 sprites) with her block in, then on as the original. In her pass the same jump returns to here. The
; handler's A: 0 no pass, 1 her frame, negative: first her place at line $BC ($C733, she comes back).
stubf:
lda.b {PASS}
bne stubf_ret
lda.b {MODE2P}
beq stubf_done
stubf_a:
jsr.w tramp
beq stubf_done
bpl stubf_f
lda.b #$BC
jsr.w $C733
stubf_f:
jsr.w $EB82
stubf_b:
jsr.w tramp
stubf_done:
jmp.w $F24C
stubf_ret:
rts
; The demonstration's exits to the title ($C18C and $C1BC of the original pad code).
exita:
lda.b #$01
jsr.w $C361
jmp.w $B000
exitb:
lda.w {PBANK}
jsr.w $C361
lda.b #$00
sta.b $62
ldx.b #$2E
txs
ldy.b #$0A
sty.b $A4
jsr.w $C49C
lda.b #$01
jsr.w $C361
jmp.w $B000
; $C03C (lda $21 / bne $C06A), a task resumes: with the other player in and a boss fighting ($4B = 1), a boss task (1
; or 2) gets the player nearer the boss as its target ($71/$74) until the next task resumes.
rsm:
lda.b {ACT}
beq rsm_q
lda.b $4B
lsr a
bcs rsm_t
lda.w {BTSWAP}
beq rsm_q
rsm_t:
jsr.w tramp
rsm_q:
lda.b $21
beq rsm_r
pla
pla
jmp.w $C06A
rsm_r:
rts
; Alone (1 PLAYER) the hooks below do the original's instructions here (no bank change: the game keeps its speed);
; otherwise they go through the trampoline.
fast_c945:
lda.b {MODE2P}
bne fast_go
lda.b #$18
sta.b $A1
rts
fast_f286:
lda.b {MODE2P}
bne fast_go
lda.b $76
lsr a
rts
fast_f27e:
lda.b {ACT}
bne fast_go
lda.b $41
jmp.w $C361
fast_e91d:
lda.b {MODE2P}
bne fast_go
jmp.w $C353
fast_go:
jmp.w tramp
; $F359 and $C4C4 (jsr $C945: the objects drawn in the death's and a fade's frames): the original draws them in its
; own bank (their frames may be in the mapped bank: drawn with bank 4 in, the attract mode's figures and the stage's
; horse came out as other pictures), then the trampoline (the return address still the hook's) colours hers.
objs:
jsr.w $C945
jmp.w tramp

; The hooks.
org $07C945
jsr.w fast_c945
nop
org $07F011
jmp.w stubf
org $07F256
jsr.w chk
org $07F27E
jsr.w fast_f27e
nop
nop
org $07F286
jsr.w fast_f286
org $07E91D
jsr.w fast_e91d
org $07E869
jsr.w tramp
org $07F3B6
jsr.w tramp
nop
org $07E72D
jsr.w tramp
nop
org $07C03C
jsr.w rsm
nop
org $07C500
jsr.w tramp
org $07F359
jsr.w objs
org $07C4C4
jsr.w objs
org $07F3DF
jsr.w tramp
nop
org $07F2DC
jsr.w tramp
nop

; ---------------------------------------------------------------------------------------------------- bank 1: the title
; $B04F (lda #$22 / sta $06A0) before the high score's VRAM script: the menu's script, the high score's line chained.
org $01B04F
jsr.w tramp
nop
nop
; $B059 lda #$A0: the script at {MENU} instead of $06A0 (the same page, $B05D's lda #$06 stays).
org $01B05A
db {<MENU}
; $B0AE (cmp #$10 / bne $B079): the cursor; START starts the game in the chosen mode.
org $01B0AE
jsr.w tramp
nop
; $B0BE / $B0CB: the chosen line blinks five times (the original's PUSH START scripts $B25D / $B24F).
org $01B0BF
db {<BLINKB}
org $01B0C3
db {>BLINKB}
org $01B0CC
db {<BLINKT}
org $01B0D0
db {>BLINKT}
; The title's VRAM script ($B150): PUSH START at $21EB becomes the cursor and line 1 at $21C9 (where the variant has
; the entry: tools/gunsmoke-coop/variants.mjs).
{TITLE}

; ---------------------------------------------------------------------------------------------------- bank 4
org $04B800
; The patcher's options: friendly fire (1 = on); player 2's sprite palette (1-3; 2: the enemies' and shots', red clothes);
; her own colours (1 = on) for that palette in two-player play, and the three (NES colour numbers).
config:
db $00,$02,$00,$16,$36,$0F
db $47,$53,$43,$4F,$4F,$50 ; "GSCOOP"

; The trampoline's call: the hook by its return address's low byte ($0104 + S), X kept.
dispatch:
stx.b {TX}
tsx
lda.w $0104,x
ldx.b {TX}
{DISPATCH}
rts

; Sets the trampoline's return to A (low) / Y (high) + 1; called from a handler's top level. Keeps X.
setret:
stx.w {TMP2}
tsx
sta.w $0106,x
tya
sta.w $0107,x
ldx.w {TMP2}
rts

; The RAM's player and the saved one exchanged ($70-$87 two bytes a turn, $88 and $F5/$F7 unrolled; the guns $90-$9F are both
; players'). Keeps X.
swap:
stx.w {TMP3}
ldx.b #$16
swap_b:
lda.b $70,x
ldy.w {SAVE},x
sta.w {SAVE},x
sty.b $70,x
lda.b $71,x
ldy.w {SAVE}+$01,x
sta.w {SAVE}+$01,x
sty.b $71,x
dex
dex
bpl swap_b
ldx.w {TMP3}
{SWAPBLOCK}
lda.b {INRAM}
eor.b #$01
sta.b {INRAM}
rts
; The players exchange their places for good: the one in RAM becomes the saved one.
exchange:
jsr.w swap
lda.b {INRAM}
eor.b #$01
sta.b {INRAM}
lda.b {P2RAM}
eor.b #$01
sta.b {P2RAM}
lda.b #$00
sta.w {OWNED}
rts
; Z clear when player 2's block is in RAM now.
p2here:
lda.b {INRAM}
eor.b {P2RAM}
rts
; $F5/$F7 exchanged with the saved ones.
swappad:
lda.b $F5
ldy.w {SAVEF5}
sta.w {SAVEF5}
sty.b $F5
lda.b $F7
ldy.w {SAVEF7}
sta.w {SAVEF7}
sty.b $F7
rts
; $F5/$F7 exchanged with the saved ones when player 2's block is in RAM (pad 1 is player 1's).
p2pad:
jsr.w p2here
beq p2pad_r
jsr.w swappad
p2pad_r:
rts
; A: bit k set when the player shot in slot $08 + k is out.
shotmask:
lda.b #$00
sta.w {TMP2}
ldx.b #$05
shotm_l:
lda.w $0408,x
asl a
rol.w {TMP2}
dex
bpl shotm_l
lda.w {TMP2}
rts
; Player 2's sprite palette (config + 1) for the six sprites from $0200 (palette1_0) or $0218 (palette1_18).
palette1_0:
ldx.b #$14
lda.b #$00
jmp.w pal_s
palette1_18:
ldx.b #$2C
lda.b #$18
pal_s:
sta.w {PBOT}
pal_l:
lda.w $0202,x
clc
adc.w config+$01
and.b #$03
sta.w {TMP2}
lda.w $0202,x
and.b #$FC
ora.w {TMP2}
sta.w $0202,x
cpx.w {PBOT}
beq pal_r
dex
dex
dex
dex
jmp.w pal_l
pal_r:
rts

; ---- C069: the pad ($C15A-$C208 of the original, moved here; pad 2 in two-player play)
h_pad:
tsx
lda.w $0103,x
sta.w {PBANK}
jsr.w p2pad
ldx.b #$01
stx.w $4016
dex
stx.w $4016
ldx.b #$08
hp_l:
lda.w $4016
lsr a
rol.b $F5
lsr a
rol.b $31
dex
bne hp_l
lda.b $31
ora.b $F5
sta.b $F5
lda.b $F5
tay
eor.b $F7
and.b $F5
sta.b $F5
sty.b $F7
sty.w {TMP5}
jsr.w p2pad
lda.w {MODE2P}
bne hp_2
jmp.w hp_d
hp_2:
ldx.b #$08
hp_m:
lda.w $4017
lsr a
rol.w {TMP4}
lsr a
rol.w {TMP2}
dex
bne hp_m
lda.w {TMP2}
ora.w {TMP4}
sta.w {TMP4}
jsr.w p2here
bne hp_r
lda.w {TMP4}
eor.w {SAVEF7}
and.w {TMP4}
sta.w {SAVEF5}
lda.w {TMP4}
sta.w {SAVEF7}
jmp.w hp_p
hp_r:
lda.w {TMP4}
eor.b $F7
and.w {TMP4}
sta.b $F5
lda.w {TMP4}
sta.b $F7
; START on the other player's pad pauses too (not while a block is swapped in for a window or a death).
; START on the other player's pad pauses too (not while a block is swapped in for a window or a death); a player
; who is out types the code instead (gift_code) and does not pause.
hp_p:
lda.b {INRAM}
bne hp_d
jsr.w out2
bcc hp_q
jsr.w gift_code
jmp.w hp_d
hp_q:
lda.w {SAVEF5}
and.b #$10
ora.b $F5
sta.b $F5
hp_d:
ldx.b #$00
ldy.w {TMP5}
lda.b $68
beq c194
lda.b $F5
and.b #$10
beq c194
jmp.w exita
c194:
lda.b $68
beq c1d3
lda.b $69
beq c1d3
lda.b $F5
and.b #$DF
sta.b $F5
ldx.b $6A
lda.w $0780,x
sta.b $F7
lda.b $6B
bne c1b2
lda.w $0781,x
sta.b $6B
c1b2:
dec.b $6B
bne c1d2
inx
inx
stx.b $6A
bpl c1d2
jmp.w exitb
c1d2:
sta.b {TA}
sty.b {TY}
rts
c1d3:
lda.b $41
bne c208
lda.b $69
beq c208
ldx.b $6A
lda.b $F7
ldy.w $0781
beq c200
cmp.w $0780,x
bne c1f6
lda.w $0781,x
clc
adc.b #$01
beq c1f6
sta.w $0781,x
bne c208
c1f6:
inx
inx
stx.b $6A
bpl c200
dec.b $69
beq c208
c200:
sta.w $0780,x
lda.b #$01
sta.w $0781,x
c208:
sta.b {TA}
sty.b {TY}
rts

; ---- stubf_a: before the other player's frame (her block in; $80: she comes back first)
h_fa:
lda.b {ACT}
beq fa_no
lda.b {BUSY}
bne fa_no
lda.b $68
bne fa_no
ldx.b #$17
fa_c:
lda.w $0200,x
sta.w $0218,x
dex
bpl fa_c
lda.w config
beq fa_s
jsr.w shotmask
sta.w {BEFORE}
fa_s:
jsr.w swap
lda.b #$01
sta.b {PASS}
ldy.w {RESPAWN}
beq fa_1
lda.b #$00
sta.w {RESPAWN}
lda.b #$3C
sta.b $7B
lda.b #$80
fa_1:
sta.b {TA}
rts
fa_no:
jsr.w p2here
beq fa_r
jsr.w palette1_0
fa_r:
lda.b #$00
sta.b {TA}
rts

; ---- stubf_b: after the other player's frame: player 2's sprites in her palette (hers at $0200 from the pass, or the
; RAM's player at $0218 when that is her); friendly fire (the saved player's new shots marked)
h_fb:
lda.b #$00
sta.b {PASS}
jsr.w swap
lda.b {P2RAM}
bne fb_18
jsr.w palette1_0
jmp.w fb_f
fb_18:
jsr.w palette1_18
fb_f:
lda.w config
beq fb_end
jsr.w shotmask
sta.w {TMP}
lda.w {BEFORE}
eor.b #$FF
and.w {TMP}
ora.w {OWNED}
sta.w {OWNED}
jsr.w friendly
fb_end:
rts
; Friendly fire: a player's shot ($08-$0D) on the other player (as $CA83's box: the shot's half sizes $DF7B/$DF86)
; when she is not hit already, blinking, invincible or riding.
friendly:
ldx.b #$05
ff_l:
lda.w $0408,x
bpl ff_n
lda.w {OWNED}
and.w bits,x
beq ff_p1
lda.b $76
ora.b $77
ora.b $7B
ora.b $7C
and.b #$FF
bne ff_n
lda.b $71
sta.w {TMP}
lda.b $74
sta.w {TMP2}
jsr.w ff_box
bcs ff_n
lda.b $76
ora.b #$01
sta.b $76
lda.b #$00
sta.w $0408,x
jmp.w ff_n
ff_p1:
lda.w {SAVE}+$06
ora.w {SAVE}+$07
ora.w {SAVE}+$0B
ora.w {SAVE}+$0C
bne ff_n
lda.w {SAVE}+$01
sta.w {TMP}
lda.w {SAVE}+$04
sta.w {TMP2}
jsr.w ff_box
bcs ff_n
lda.w {SAVE}+$06
ora.b #$01
sta.w {SAVE}+$06
lda.b #$00
sta.w $0408,x
ff_n:
dex
bpl ff_l
rts
; Carry clear when shot x (slot $08 + x) is on the player at y {TMP}, x {TMP2}.
ff_box:
ldy.w $0448,x
lda.w $DF7B,y
clc
adc.b #$04
sta.w {TMP3}
lda.w $DF86,y
sta.w {TMP4}
lda.w {TMP}
sec
sbc.w $05C8,x
bcs ffb_1
eor.b #$FF
adc.b #$01
ffb_1:
cmp.w {TMP3}
bcs ffb_r
lda.w {TMP2}
sec
sbc.w $05E8,x
bcs ffb_2
eor.b #$FF
adc.b #$01
ffb_2:
cmp.w {TMP4}
ffb_r:
rts
bits:
db $01,$02,$04,$08,$10,$20

; ---- C947: $C945's first sprite: $18, or $30 with the other player's sprites at $0218; in two-player play the lives
; first (six sprites kept for them: never crowded out by the objects)
h_c945:
lda.b #$18
ldy.b {ACT}
beq hc_1
ldy.b {BUSY}
bne hc_1
lda.b #$30
hc_1:
sta.b $A1
ldy.b {BUSY}
bne hc_s
ldy.w {INGAME}
beq hc_s
jsr.w hud
lda.b $A1
hc_s:
sta.b $A1
sta.b {TA}
rts

; The saved player against the objects once a frame after $C945 drew them ($CA83 with her block in when she touches
; one; a shop or the horse ($CC51) done here: her window, the enemy shots gone ($CDD4)).
touch2:
ldx.b #$1F
t2_l:
stx.w {LOOPX}
lda.w $0400,x
and.b #$90
cmp.b #$90
bne t2_n
ldy.w $0440,x
lda.w $DF7B,y
sta.b $36
lda.w $DF86,y
sta.b $37
jsr.w near2
bcs t2_n
jsr.w swap
lda.w $0420,x
cmp.b #$10
bcc t2_c
cmp.b #$20
bcs t2_c
sta.b $A7
ldy.w $0560,x
iny
sty.b $A5
lda.b #$00
sta.w $0400,x
lda.b #$3C
sta.b $7B
lda.b #$01
sta.w {WINO}
jsr.w swap
jsr.w $CDD4
jmp.w t2_n
t2_c:
jsr.w $CA83
ldx.w {LOOPX}
jsr.w swap
t2_n:
ldx.w {LOOPX}
dex
bpl t2_l
rts
; $CA83's test on the saved player: carry clear when object x touches her.
near2:
lda.w {SAVE}+$06
lsr a
bcs near_no
lda.w {SAVE}+$01
sec
sbc.w $05C0,x
bcs near_1
eor.b #$FF
adc.b #$01
near_1:
sec
sbc.b $36
bcc near_x
sec
sbc.b #$04
bcc near_x
ldy.w {SAVE}+$07
beq near_no
sbc.b #$08
bcs near_no
near_x:
lda.w {SAVE}+$04
sec
sbc.w $05E0,x
bcs near_2
eor.b #$FF
adc.b #$01
near_2:
sec
sbc.b $37
rts
near_no:
sec
rts

; ---- F258: before enemy x's handler: the nearer player as its target ($71/$74)
h_target:
jsr.w untarget
lda.b {BUSY}
bne tg_end
lda.w $0400,x
bpl tg_end
lda.w $0420,x
cmp.b #$40
bcc tg_end
lda.b $71
ldy.b $74
jsr.w dist
sta.w {TMP3}
sty.w {TMP4}
lda.w {SAVE}+$01
ldy.w {SAVE}+$04
jsr.w dist
cpy.w {TMP4}
bcc tg_swap
bne tg_end
cmp.w {TMP3}
bcs tg_end
tg_swap:
jsr.w xtarget
lda.b #$01
sta.w {TSWAP}
tg_end:
lda.w $0400,x
sta.b {TA}
rts
; |A - $05C0,x| + |Y - $05E0,x| as A (low) / Y (high).
dist:
sty.w {TMP2}
sec
sbc.w $05C0,x
bcs dist_1
eor.b #$FF
adc.b #$01
dist_1:
sta.w {TMP}
lda.w {TMP2}
sec
sbc.w $05E0,x
bcs dist_2
eor.b #$FF
adc.b #$01
dist_2:
ldy.b #$00
clc
adc.w {TMP}
bcc dist_3
iny
dist_3:
rts
untarget:
lda.w {TSWAP}
beq untg_r
jsr.w xtarget
lda.b #$00
sta.w {TSWAP}
untg_r:
rts
xtarget:
lda.b $71
ldy.w {SAVE}+$01
sta.w {SAVE}+$01
sty.b $71
lda.b $74
ldy.w {SAVE}+$04
sta.w {SAVE}+$04
sty.b $74
rts

; ---- F280: the enemies' loop done ($F27E lda $41 / jsr $C361): the target back, the stage's bank
h_eend:
jsr.w untarget
stx.b {TX}
tsx
lda.b $41
sta.w $0103,x
ldx.b {TX}
sta.b {TA}
sta.b {TY}
rts

; ---- F288: $F286 (lda $76 / lsr a) in two-player play: the lives in the corners; the other player's touches and her
; hit (she dies where she stands), horse or bomb (with her block in, the original's code); then the RAM's player: hit
; while the other plays on, the players exchange places and that one dies where they stand; else the original's death
hh_d0:
jmp.w hh_d
h_hit:
lda.b {BUSY}
bne hh_d0
lda.b $68
bne hh_d0
jsr.w her_figure
lda.b {ACT}
beq hh_r
jsr.w touch2
lda.w {SAVE}+$06
lsr a
bcc hh_o
jsr.w die_saved
jmp.w hh_r
hh_o:
and.b #$03
beq hh_r
jsr.w swap
lda.b #$01
sta.b {BUSY}
sta.b {BSWAP}
jmp.w hh_d
hh_r:
lda.b $76
lsr a
bcc hh_d
lda.b {ACT}
beq hh_b
jsr.w exchange
jsr.w die_saved
jmp.w hh_d
hh_b:
lda.b #$01
sta.b {BUSY}
hh_d:
lda.b $76
lsr a
sta.b {TA}
rts

; Player 2 dying where she stands (the saved player, {P2RAM} clear): the death figure's sprites the game draws in
; palette 0 (tiles below $36, the objects' sprites from $18) get her palette, as her_sprites in the original's death.
her_figure:
lda.w {DYING}
beq hf_r
lda.b {P2RAM}
bne hf_r
stx.w {TMP3}
ldx.b #$30
jmp.w her_from
hf_r:
rts

; ---- E91F: the play loop's start ($E91D jsr $C353) in two-player play: after a shop both get what was bought; after a
; hit or a window the players back; the dying player's time (the death figure away; back when lives are left)
h_loop:
lda.w {SHOPW}
beq hl_0
lda.b #$00
sta.w {SHOPW}
lda.b {ACT}
beq hl_0
jsr.w share
hl_0:
lda.b {BUSY}
beq hl_j
lda.b {BSWAP}
beq hl_1
jsr.w swap
hl_1:
lda.b #$00
sta.b {BUSY}
sta.b {BSWAP}
sta.w {WINO}
hl_j:
lda.b {ACT}
beq hl_d
lda.w config
beq hl_c
jsr.w shotmask
and.w {OWNED}
sta.w {OWNED}
jmp.w hl_c
hl_d:
lda.w {GIFT}
beq hl_e
jsr.w gift
jmp.w hl_c
hl_e:
lda.w {DYING}
beq hl_c
dec.w {DYING}
bne hl_c
jsr.w dfig_off
lda.w {SAVE}+$0A
beq hl_c
lda.b #$80
jsr.w revive
hl_c:
jsr.w $C353
sta.b {TA}
rts
; After a shop: the buyer's (the RAM player's) better boots ($78) and rifle ($79) to the other too. The guns and their
; shots ($90-$9F) are both players' anyway; the gun in hand ($88) stays each player's own choice (her status window).
share:
lda.b $78
cmp.w {SAVE}+$08
bcc share_1
sta.w {SAVE}+$08
share_1:
lda.b $79
cmp.w {SAVE}+$09
bcc share_2
sta.w {SAVE}+$09
share_2:
rts
; Her own colours (config + 2 on) into her sprite palette in the palette buffer ($03F0 + 4 * palette + 1..3). Keeps X.
own_colours:
lda.w config+$02
beq own_r
stx.w {TMP3}
lda.w config+$01
and.b #$03
asl a
asl a
tax
lda.w config+$03
sta.w $03F1,x
lda.w config+$04
sta.w $03F2,x
lda.w config+$05
sta.w $03F3,x
ldx.w {TMP3}
own_r:
rts

; ---- F35B: $F359 (jsr $C945) in the death's frames, C4C6: $C4C4 (jsr $C945) in a fade's frames (after objs drew them),
; and F3E1: $F3DF
; (lda #$14 / sta $FE) after the lives screen's sprites: when player 2 dies the original's way (her block in RAM) the
; player's and the death's sprites (tiles below $36) the game draws in palette 0 get hers.
h_death:
jsr.w her_sprites
rts
h_lives_screen:
lda.b #$14
sta.b $FE
sta.b {TA}
jsr.w her_sprites
rts
her_sprites:
lda.b {BUSY}
beq hs_r
jsr.w p2here
beq hs_r
stx.w {TMP3}
ldx.b #$04
her_from:
hs_l:
lda.w $0201,x
cmp.b #$36
bcs hs_n
lda.w $0202,x
and.b #$03
bne hs_n
lda.w $0202,x
ora.w config+$01
sta.w $0202,x
hs_n:
inx
inx
inx
inx
bne hs_l
ldx.w {TMP3}
hs_r:
rts

; ---- C502: $C500 (sty $3B / rts) at the end of $C4E4, a palette set loaded (also each fade step): her own colours
; in two-player play (before a fade darkens them), then $C4E4's rts (the one at $C086)
h_pal:
lda.b {TY}
sta.b $3B
lda.w {MODE2P}
beq hpal_r
jsr.w own_colours
hpal_r:
lda.b #$85
ldy.b #$C0
jsr.w setret
rts

; ---- F2DE: $F2DC (ldy $A5 / bne $F2E6), a shop or the status window: the saved player's when she touched the shop or
; pressed SELECT, with her block in
h_window:
ldy.b $A5
sty.b {TY}
beq hw_sel
lda.b #$01
sta.w {SHOPW}
lda.w {WINO}
beq hw_open
jsr.w p2win
hw_open:
lda.b #$00
sta.w {WINO}
lda.b #$E5
ldy.b #$F2
jsr.w setret
rts
hw_sel:
lda.b $F5
and.b #$20
bne hw_ret
lda.b {ACT}
beq hw_ret
lda.b {BUSY}
bne hw_ret
lda.w {SAVEF5}
and.b #$20
beq hw_ret
jsr.w p2win
jmp.w hw_open
hw_ret:
rts
p2win:
lda.b {ACT}
beq p2w_r
lda.b {BUSY}
bne p2w_r
jsr.w swap
lda.b #$01
sta.b {BUSY}
sta.b {BSWAP}
p2w_r:
rts
end_b800:

; ---------------------------------------------------------------------------------------------------- bank 4 $9000
org $049000
; ---- rsm_t: a task resumes ($21): the last boss task's target back; a boss task (1, 2) while a boss fights ($4B = 1)
; gets the player nearer the boss (slot $0E)
h_rsm:
jsr.w unboss
lda.b {BUSY}
bne rs_e
lda.b $4B
cmp.b #$01
bne rs_e
lda.b $21
beq rs_e
cmp.b #$03
bcs rs_e
ldx.b #$0E
lda.b $71
ldy.b $74
jsr.w dist
sta.w {TMP3}
sty.w {TMP4}
lda.w {SAVE}+$01
ldy.w {SAVE}+$04
jsr.w dist
cpy.w {TMP4}
bcc rs_swap
bne rs_e
cmp.w {TMP3}
bcs rs_e
rs_swap:
jsr.w xtarget
lda.b #$01
sta.w {BTSWAP}
rs_e:
ldx.b {TX}
rts
unboss:
lda.w {BTSWAP}
beq unb_r
jsr.w xtarget
lda.b #$00
sta.w {BTSWAP}
unb_r:
rts

; The saved player hit while the other plays on ($F31C's for her, where she stands): the death sound ($2A), the death
; figure ($35, $C796) in slot 0 following the scroll (bit 5) for 72 frames; the boots and the rifle a level down ($CC67
; on her block; the shared guns kept); a life less.
die_saved:
stx.w {TMP3}
lda.b #$00
sta.w {SAVE}+$06
sta.w {SAVE}+$07
sta.b {ACT}
sta.w {RESPAWN}
lda.b #$48
sta.w {DYING}
lda.b #$35
ldx.b #$00
jsr.w $C796
lda.w $0400
ora.b #$20
sta.w $0400
lda.w $0420
sta.w {DFIG}
lda.w {SAVE}+$01
sta.w $05C0
lda.w {SAVE}+$04
sta.w $05E0
lda.b #$2A
jsr.w $C36A
lda.w {SAVE}+$08
beq ds_1
dec.w {SAVE}+$08
ds_1:
lda.w {SAVE}+$09
beq ds_2
dec.w {SAVE}+$09
bne ds_2
lda.w {SAVE}+$18
cmp.b #$01
bne ds_2
lda.b #$00
sta.w {SAVE}+$18
ds_2:
lda.w {SAVE}+$0A
beq ds_3
dec.w {SAVE}+$0A
ds_3:
ldx.w {TMP3}
rts
; The death figure away (when slot 0 still holds it).
dfig_off:
lda.w $0400
bpl dfo_r
lda.w $0420
cmp.w {DFIG}
bne dfo_r
lda.b #$00
sta.w $0400
dfo_r:
rts
; The saved player back in play from X = A (the original's $E88D on her block: the shots and the hit cleared, gun 0 or
; the rifle's 1), blinking; her first pass places her at line $BC ($C733, stubf).
revive:
sta.w {SAVE}+$04
ldy.b #$07
lda.b #$00
rv_1:
sta.w {SAVE}+$10,y
dey
bpl rv_1
sta.w {SAVE}+$06
sta.w {SAVE}+$07
sta.w {SAVE}+$18
sta.w {DYING}
lda.w {SAVE}+$09
beq rv_2
lda.b #$01
sta.w {SAVE}+$18
rv_2:
lda.b #$3C
sta.w {SAVE}+$0B
lda.b #$01
sta.w {RESPAWN}
sta.b {ACT}
rts

; The lives in the bottom corners as the original's lives screen shows them ($F3BA: the player's figure and a digit):
; the figure's hat (tiles 0 and 1) in the player's palette and the digit in the score's palette 1 (white with a dark
; edge); player 1 left, player 2 right. After the objects' sprites ($A1), when six sprites are free.
hud:
lda.b $A1
beq hud_r
cmp.b #$E9
bcs hud_r
tax
ldy.b #$00
hud_l:
lda.w hudt,y
sta.w $0200,x
inx
iny
cpy.b #$18
bne hud_l
stx.b $A1
txa
sec
sbc.b #$18
tax
lda.b {P2RAM}
bne hud_1
lda.b $7A
ldy.w {SAVE}+$0A
jmp.w hud_2
hud_1:
lda.w {SAVE}+$0A
ldy.b $7A
hud_2:
jsr.w digit
sta.w $0209,x
tya
jsr.w digit
sta.w $0215,x
lda.w config+$01
and.b #$03
sta.w $020E,x
sta.w $0212,x
hud_r:
rts
; The six sprites (Y, tile, attribute, X); the digits' tiles and player 2's palette are set by hud.
hudt:
db $D0,$00,$00,$10, $D0,$01,$00,$18, $D0,$58,$01,$22
db $D0,$00,$02,$D4, $D0,$01,$02,$DC, $D0,$58,$01,$E6
; A (0-9) as its sprite tile ($58 + digit, as the lives screen's $F3BF).
digit:
cmp.b #$0A
bcc dg_1
lda.b #$09
dg_1:
clc
adc.b #$58
rts

; ---- E86B: $E869 (jsr $E88D) when the player enters (a stage, after the lives screen): in two-player play the other
; player with lives left enters too, beside the first.
h_place:
jsr.w $E88D
lda.w {MODE2P}
beq pl_e
lda.b $68
bne pl_e
lda.b #$01
sta.w {INGAME}
lda.b #$00
sta.w {DYING}
sta.b {ACT}
lda.w {SAVE}+$0A
beq pl_e
ldx.b #$05
pl_1:
lda.b $70,x
sta.w {SAVE},x
dex
bpl pl_1
lda.b $74
clc
adc.b #$18
cmp.b #$E8
bcc pl_2
lda.b $74
sec
sbc.b #$18
pl_2:
jsr.w revive
pl_e:
ldx.b {TX}
rts

; ---- F3B8: $F3B6 (dec $7A / beq $F3F4) after the original's death of the RAM's player: a life less; in two-player play
; with none left the other player (with lives) takes the RAM's place and the lives screen is hers; the game over when
; nobody has lives
h_lives:
dec.b $7A
bne lv_ret
lda.w {MODE2P}
beq lv_go
lda.w {SAVE}+$0A
beq lv_go
jsr.w exchange
lda.b #$00
sta.b {ACT}
sta.w {DYING}
sta.w {RESPAWN}
lv_ret:
rts
lv_go:
lda.b #$00
sta.w {INGAME}
lda.b #$F3
ldy.b #$F3
jsr.w setret
rts

; ---- E72F: $E72D (lda #$03 / sta $7A) when a game starts or continues: player 1 in RAM, three lives; with 2 PLAYERS
; player 2 three lives too (a new game: her block cleared; a continue keeps her boots and rifle like the original's $AB)
h_init:
lda.b {MODE2P}
beq hi_0
jsr.w unboss
hi_0:
lda.b {P2RAM}
beq hi_1
jsr.w exchange
hi_1:
lda.b #$03
sta.b $7A
sta.b {TA}
lda.b #$00
sta.b {ACT}
sta.b {BUSY}
sta.b {BSWAP}
sta.b {INRAM}
sta.b {PASS}
sta.b {P2RAM}
sta.w {OWNED}
sta.w {WINO}
sta.w {TSWAP}
sta.w {DYING}
sta.w {RESPAWN}
sta.w {GIFT}
sta.w {CODEI}
lda.w {MODE2P}
beq hi_r
lda.b $AB
bne hi_3
ldy.b #$18
lda.b #$00
hi_2:
sta.w {SAVE},y
dey
bpl hi_2
hi_3:
lda.b #$03
sta.w {SAVE}+$0A
lda.b #$00
sta.w {SAVEF5}
sta.w {SAVEF7}
sta.w {SHOPW}
hi_r:
rts

; ---- B051: $B04F (lda #$22 / sta $06A0) on the title: the menu's VRAM script at {MENU}: both lines with the cursor at
; the chosen one, then the high score's line ($06A0: $2273, its count and digits) chained
h_menu:
lda.b #$00
sta.w {MODE2P}
sta.w {INGAME}
sta.w {GIFT}
lda.b #$22
sta.w $06A0
lda.b #$73
sta.w $06A1
jsr.w cursor_ok
ldx.b #$00
ldy.b #$00
mn_1:
lda.w lines,y
sta.w {MENU},x
inx
iny
cpy.b #$1C
bne mn_1
jsr.w marks
sta.w {MENU}+$03
sty.w {MENU}+$11
ldy.b #$00
mn_2:
lda.w $06A0,y
cmp.b #$FF
beq mn_3
sta.w {MENU},x
inx
iny
cpy.b #$10
bne mn_2
mn_3:
lda.b #$FF
sta.w {MENU},x
rts
; The cursor's choice: $A0 (1 PLAYER) or $A1 (2 PLAYERS); anything else (RAM after power-on, never cleared there)
; becomes $A0.
cursor_ok:
lda.w {CURSOR}
and.b #$FE
cmp.b #$A0
beq cok_r
lda.b #$A0
sta.w {CURSOR}
cok_r:
rts
; A / Y: the cursor tile ($FC, the rifle) or a space ($F4) for line 1 / line 2.
marks:
lda.w {CURSOR}
and.b #$01
bne mk_2
lda.b #$FC
ldy.b #$F4
rts
mk_2:
lda.b #$F4
ldy.b #$FC
rts
; The two lines' VRAM entries (address, count, the cursor's place, the text): line 1 (1 PLAYER) at $21C9, line 2
; (2 PLAYERS) at $2209; the text the variant's.
lines:
{LINES}

; ---- B0B0: $B0AE (cmp #$10 / bne $B079) in the title's loop, A = the new presses ($F5): up, down or SELECT move the
; cursor (the attract mode's count starts again); the cursor drawn; START: the mode chosen and its line to blink
h_title:
lda.b #$00
sta.w {MODE2P}
sta.b {P2RAM}
sta.w {DYING}
sta.w {RESPAWN}
sta.w {BTSWAP}
sta.w {TSWAP}
jsr.w cursor_ok
lda.b {TA}
cmp.b #$10
beq ht_start
and.b #$2C
beq ht_draw
lda.w {CURSOR}
eor.b #$01
sta.w {CURSOR}
lda.b #$00
sta.b {TX}
ht_draw:
lda.b $27
bne ht_loop
jsr.w marks
sta.w {CURS}+$03
sty.w {CURS}+$07
lda.b #$21
sta.w {CURS}
lda.b #$C9
sta.w {CURS}+$01
lda.b #$01
sta.w {CURS}+$02
sta.w {CURS}+$06
lda.b #$22
sta.w {CURS}+$04
lda.b #$09
sta.w {CURS}+$05
lda.b #$FF
sta.w {CURS}+$08
lda.b #{<CURS}
sta.b $26
lda.b #{>CURS}
sta.b $27
ht_loop:
ldx.b {TX}
lda.b #$78
ldy.b #$B0
jsr.w setret
rts
ht_start:
lda.b #$00
sta.w {SAVEF5}
sta.w {SAVEF7}
lda.w {CURSOR}
and.b #$01
sta.w {MODE2P}
ldy.b #$00
cmp.b #$00
beq ht_s1
ldy.b #$0E
ht_s1:
ldx.b #$00
ht_s2:
lda.w lines,y
sta.w {BLINKT},x
sta.w {BLINKB},x
iny
inx
cpx.b #$0E
bne ht_s2
lda.b #$FC
sta.w {BLINKT}+$03
sta.w {BLINKB}+$03
lda.b #$FF
sta.w {BLINKT}+$0E
sta.w {BLINKB}+$0E
ldx.b #$09
lda.b #$F4
ht_s3:
sta.w {BLINKB}+$04,x
dex
bpl ht_s3
ldx.b {TX}
rts
; ---- A life from the other player: the saved player out (no lives, not dying, in two-player play) types A B B A
; START on her pad; when the RAM's player has a life to spare (2 or more: the count includes the life in play, as
; the original's lives screen), one moves over at the play loop's start and she comes back as after a death (line
; $BC from the middle, blinking).
; Carry set when the saved player is out.
out2:
lda.b {ACT}
bne o2_n
lda.w {DYING}
bne o2_n
lda.w {SAVE}+$0A
bne o2_n
lda.w {INGAME}
beq o2_n
lda.b $68
bne o2_n
sec
rts
o2_n:
clc
rts
; The code on the saved pad's new presses ({SAVEF5}); a wrong press starts it again (or as its first step).
gift_code:
lda.w {SAVEF5}
beq gc_r
ldy.w {CODEI}
cpy.b #$05
bcs gc_0
cmp.w code,y
beq gc_n
gc_0:
ldy.b #$00
cmp.w code
bne gc_s
gc_n:
iny
cpy.b #$05
bne gc_s
lda.b #$01
sta.w {GIFT}
ldy.b #$00
gc_s:
sty.w {CODEI}
gc_r:
rts
code:
db $80,$40,$40,$80,$10
; From h_loop with the code typed: the life moves over when she is still out and the other has one to spare.
gift:
lda.b #$00
sta.w {GIFT}
jsr.w out2
bcc gf_r
lda.b $7A
cmp.b #$02
bcc gf_r
dec.b $7A
lda.b #$01
sta.w {SAVE}+$0A
lda.b #$80
jsr.w revive
gf_r:
rts
end_9000:
