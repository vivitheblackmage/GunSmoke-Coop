import fs from 'node:fs';
import crypto from 'node:crypto';
// The ROMs the two-player patch supports (docs/HACK.md, "Variants"), by SHA-256 of the ROM without
// its 16-byte header. Only the title differs between them: the line PUSH START had (the title script's entry that
// becomes 1 PLAYER) and the two lines' text. Everything else the patch writes, calls and reads is byte for byte the
// same in each (checked by build.mjs). Kept apart from tools/nes/rom.mjs SUPPORTED
// (the analysis machine's list).
//   title: the title script's PUSH START entry (bank 1 address, its length), replaced by line 1 with the cursor
//   lines: line 1 and line 2 after the cursor and a space (11 characters each with the cursor's two)
//   edits: more of the title's text replaced (the bank 1 address of the first character, the new text; the same length)
//   pal:   a PAL game (50 Hz): the patched file gets an NES 2.0 header saying so (PAL_HEADER), as its own iNES 1.0
//          header says nothing and emulators and the MiSTer would run it as NTSC (60 Hz: the music too fast)
export const VARIANTS = [
  { id: 'usa', name: 'USA', sha256: 'f39421a126f3b93caa37d6c3ed899840ddfd51b0587446ca459f72564aca1433',
    file: 'Gun.Smoke (USA).nes', out: 'Gun.Smoke (USA) (Co-op).nes',
    title: { at: 0xb1c0, length: 13 }, lines: ['1 PLAYER ', '2 PLAYERS'] },
  // The European (PAL) ROM against the USA one: bank 6 $8001-$AE5F (its tiles), the player's movement tables larger
  // ($DF91-$E118: for 50 Hz), cmp #$40 / bne made bcc at $A03C (banks 1, 2, 4) and $5:A048, the title's lines below
  // PUSH START ($1:B1DD-$B224) and a few bytes of banks 0 and 1; the rest of the code the same, PUSH START at $B1C0.
  { id: 'eur', name: 'Europe (PAL)', sha256: 'f82e65b91b7ce609146bc8c3975f2c1780fe965c1208695892a2be2cb281616e',
    file: 'Gun.Smoke (Europe).nes', out: 'Gun.Smoke (Europe) (Co-op).nes',
    title: { at: 0xb1c0, length: 13 }, lines: ['1 PLAYER ', '2 PLAYERS'], pal: true },
  // Krut.Rök: a Swedish fan translation of the European ROM ("MODERMODEM 2013, KASSETT 01 AV 33"): banks 4-6 and the
  // code as the European one's; the texts and its own tiles (banks 0-3, the fixed bank's game over and shop texts)
  // differ. Its title script moved TRYCK START (11 letters) to $B1BF.
  { id: 'krut', name: 'Krut.Rök (Swedish, PAL)', sha256: 'bf7939dddcde7dbb849b83565b49915497c187ab1c192a9643fb5c2ff62e7c80',
    file: 'Krut.Rök - cart #1 (SW).nes', out: 'Krut.Rok (SW) (Co-op).nes',
    title: { at: 0xb1bf, length: 14 }, lines: ['1 SPELARE', '2 SPELARE'], pal: true,
    // the cassette's number off the title (the user's wish): KASSETT 01 AV 33 ($2324, 24 characters from $B20D)
    // spaces; MODERMODEM 0300 2013 stays
    edits: [{ at: 0xb20d, text: ' '.repeat(24) }] },
];

// The header of a PAL variant's patched file, as the European ROM's own: NES 2.0 (byte 7 bits 2-3 = 10), mapper 2 with
// horizontal mirroring (bytes 6-7), 8 x 16 KiB PRG, PRG RAM and CHR RAM 8 KiB (bytes 10-11: 64 << 7), timing PAL (byte
// 12 = 1), standard controllers (byte 15 = 1). Header values, not game data.
export const PAL_HEADER = Buffer.from([0x4e, 0x45, 0x53, 0x1a, 0x08, 0x00, 0x20, 0x08, 0x00, 0x00, 0x07, 0x07, 0x01, 0x00, 0x00, 0x01]);

export const sha256Of = body => crypto.createHash('sha256').update(body).digest('hex');

// The user's ROM as a variant: { variant, rom } (rom in tools/nes/rom.mjs's shape: prg, chr, mapper, mirroring).
export function readVariant(filename) {
  const file = fs.readFileSync(filename);
  if (file.subarray(0, 4).toString('latin1') !== 'NES\x1a') throw Error('Not an iNES file.');
  const body = file.subarray(16), sha = sha256Of(body);
  const variant = VARIANTS.find(v => v.sha256 === sha);
  if (!variant) throw Error('Unsupported game revision (SHA-256 ' + sha + ').');
  const mapper = (file[6] >> 4) | (file[7] & 0xf0);
  if (mapper !== 2 || file[4] !== 8 || body.length !== 0x20000) throw Error('Unexpected header for ' + variant.name + '.');
  return { variant, file, rom: { game: 'gunsmoke', title: variant.name, mapper: 2, prgBanks: 8, sha256: sha, prg: body,
    chr: body.subarray(0x20000), verticalMirroring: !!(file[6] & 1) } };
}

// The variant's ROM in the working directory, when there.
export const variantFile = variant => fs.existsSync(variant.file) ? variant.file : null;
