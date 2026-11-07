import fs from 'node:fs';
import crypto from 'node:crypto';
// The user's NES ROM (iNES / NES 2.0): header, PRG banks and the supported revision. Never copied into the repository.

// Supported revisions by SHA-256 of the ROM without its 16-byte header.
export const SUPPORTED = {
  'f39421a126f3b93caa37d6c3ed899840ddfd51b0587446ca459f72564aca1433': { game: 'gunsmoke', title: 'Gun.Smoke (USA)', mapper: 2, prgBanks: 8 },
};

export function findRom(argument) {
  if (argument) return argument;
  const files = fs.readdirSync('.').filter(f => /\.nes$/i.test(f));
  if (files.length !== 1) throw Error('Specify the NES ROM.'); // several in the folder: name it
  return files[0];
}

export function readNes(filename) {
  const file = fs.readFileSync(filename);
  if (file.subarray(0, 4).toString('latin1') !== 'NES\x1a') throw Error('Not an iNES file.');
  const body = file.subarray(16), sha256 = crypto.createHash('sha256').update(body).digest('hex');
  const known = SUPPORTED[sha256];
  if (!known) throw Error('Unsupported game revision (SHA-256 ' + sha256 + ').');
  const prgSize = file[4] * 16384, chrSize = file[5] * 8192;
  const mapper = (file[6] >> 4) | (file[7] & 0xf0);
  if (mapper !== known.mapper || file[4] !== known.prgBanks) throw Error('Unexpected header for ' + known.title + '.');
  return { ...known, sha256, prg: body.subarray(0, prgSize), chr: body.subarray(prgSize, prgSize + chrSize),
    verticalMirroring: !!(file[6] & 1) };
}

// NROM (mapper 0): 16 or 32 KiB of PRG at $8000 (16 KiB mirrored at $C000), no bank switching.
export class NRom {
  constructor(rom) { this.prg = rom.prg; this.banks = 1; this.bank = 0; }
  read(address) { return this.prg[(address - 0x8000) % this.prg.length]; }
  write() {}
  offset(address) { return (address - 0x8000) % this.prg.length; }
}
// The cartridge for a supported ROM's mapper.
export function cartridge(rom) { return rom.mapper === 0 ? new NRom(rom) : new UxRom(rom); }

// UxROM (mapper 2): $8000-$BFFF the selected 16 KiB bank, $C000-$FFFF the last bank.
export class UxRom {
  constructor(rom) { this.prg = rom.prg; this.banks = rom.prg.length >> 14; this.bank = 0; }
  read(address) {
    if (address >= 0xc000) return this.prg[(this.banks - 1) * 0x4000 + (address - 0xc000)];
    return this.prg[this.bank * 0x4000 + (address - 0x8000)];
  }
  // A write anywhere in $8000-$FFFF selects the bank (the value ANDed with the ROM byte there: bus conflict).
  write(address, value) { this.bank = (value & this.read(address)) % this.banks; }
  // File offset of a CPU address in a given bank (fixed bank for $C000+).
  offset(address, bank = this.bank) { return address >= 0xc000 ? (this.banks - 1) * 0x4000 + address - 0xc000 : bank * 0x4000 + address - 0x8000; }
}
