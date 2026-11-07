import { Cpu6502 } from './cpu6502.mjs';
import { cartridge } from './rom.mjs';
// A small NES for analysis and ROM oracles: the 6502, 2 KiB RAM, the cartridge (UxROM), the PPU's registers and
// memories (CHR RAM or ROM, two nametables with the cartridge's mirroring, palette, OAM, OAM DMA) and the pads. Frames are
// counted in CPU cycles (29781 a frame, NTSC; vblank from cycle 27393 of a frame, about scanline 241) with the NMI
// at vblank when $2000 bit 7 is set. The PPU does not draw.
export const FRAME_CYCLES = 29781, VBLANK_CYCLES = 27393;

export class NesMachine {
  constructor(rom) {
    this.cart = cartridge(rom);
    this.ram = new Uint8Array(0x800);
    this.chrRom = !!(rom.chr && rom.chr.length);   // CHR ROM (NROM games) or CHR RAM (Gun.Smoke)
    this.chr = this.chrRom ? Uint8Array.from(rom.chr.subarray(0, 0x2000)) : new Uint8Array(0x2000);
    this.ciram = new Uint8Array(0x800);          // two nametables
    this.palette = new Uint8Array(32);
    this.oam = new Uint8Array(256);
    this.vertical = rom.verticalMirroring;
    this.ctrl = 0; this.mask = 0; this.status = 0; this.oamAddress = 0;
    this.v = 0; this.t = 0; this.x = 0; this.w = 0; this.readBuffer = 0;
    this.pads = [0, 0, 0, 0]; this.shift = [0, 0]; this.strobe = 0;
    // A Four Score (NES Satellite) on the ports: each port's 24 bits are its pad, pad 3/4, then the signature
    // (reads 17-24: 0,0,0,1,0,0,0,0 on $4016 and 0,0,1,0,0,0,0,0 on $4017); after that 1s.
    this.fourScore = false;
    // A Famicom 4-player adapter (Hori) on the expansion port instead: the same 24 bits a port on bit 1, its signatures
    // (reads 17-24) 0,0,1,0,0,0,0,0 on $4016 and 0,0,0,1,0,0,0,0 on $4017; the console's own pads (bit 0) idle.
    this.hori = false; this.horiShift = [0, 0];
    this.apuWrites = [];                          // APU register writes (register << 8 | value) since the owner cleared it
    this.frame = 0; this.frameStart = 0; this.vblankRaised = false;
    this.cpu = new Cpu6502(this);
    this.hooks = new Map();                       // address -> function(machine) before the instruction there
    this.vramWrites = 0;
    this.coverage = null;                          // a Set of bank * $10000 + PC of executed instructions, when set
  }
  // PPU addresses: $0000-$1FFF CHR RAM, $2000-$2FFF nametables (mirrored), $3F00 palette.
  ppuIndex(address) {
    address &= 0x3fff;
    if (address < 0x2000) return ['chr', address];
    if (address >= 0x3f00) { let p = address & 0x1f; if ((p & 0x13) === 0x10) p &= 0x0f; return ['palette', p]; }
    const n = (address >> 10) & 3, offset = address & 0x3ff;
    const page = this.vertical ? n & 1 : n >> 1;
    return ['ciram', page * 0x400 + offset];
  }
  ppuRead(address) { const [m, i] = this.ppuIndex(address); return this[m][i]; }
  ppuWrite(address, value) { const [m, i] = this.ppuIndex(address); if (m !== 'chr' || !this.chrRom) this[m][i] = value; this.vramWrites++; }
  inVblank() { return this.cpu.cycles - this.frameStart >= VBLANK_CYCLES; }
  read(address) {
    if (address < 0x2000) return this.ram[address & 0x7ff];
    if (address < 0x4000) {
      switch (address & 7) {
        case 2: {
          // Sprite 0 hit (bit 6), approximately: once the beam passes sprite 0's line with rendering on, until the
          // frame's end (113.67 CPU cycles a scanline; the picture starts about line 0 of the frame here).
          const at = this.cpu.cycles - this.frameStart, hit = (this.mask & 0x18) && at >= (this.oam[0] + 2) * 114 && at < VBLANK_CYCLES;
          const v = (this.inVblank() && !this.vblankRead ? 0x80 : 0) | (hit ? 0x40 : 0);
          if (this.inVblank()) this.vblankRead = true; this.w = 0; return v;
        }
        case 4: return this.oam[this.oamAddress];
        case 7: {
          const a = this.v & 0x3fff; let value;
          if (a >= 0x3f00) { value = this.ppuRead(a); this.readBuffer = this.ppuRead(a - 0x1000); }
          else { value = this.readBuffer; this.readBuffer = this.ppuRead(a); }
          this.v = (this.v + (this.ctrl & 4 ? 32 : 1)) & 0x7fff; return value;
        }
        default: return 0;
      }
    }
    if (address === 0x4016 || address === 0x4017) {
      const p = address & 1; const bit = this.strobe ? this.pads[p] & 1 : this.shift[p] & 1;
      if (this.hori) {
        const hbit = this.strobe ? this.pads[p] & 1 : this.horiShift[p] & 1;
        if (!this.strobe) this.horiShift[p] = (this.horiShift[p] >>> 1) | 0x800000;
        return 0x40 | hbit << 1;
      }
      if (!this.strobe) this.shift[p] = this.fourScore ? (this.shift[p] >>> 1) | 0x800000 : (this.shift[p] >> 1) | 0x80; return 0x40 | bit;
    }
    if (address < 0x8000) return 0;
    return this.cart.read(address);
  }
  write(address, value) {
    if (address < 0x2000) { this.ram[address & 0x7ff] = value; return; }
    if (address < 0x4000) {
      switch (address & 7) {
        case 0: this.ctrl = value; this.t = (this.t & ~0x0c00) | ((value & 3) << 10); break;
        case 1: this.mask = value; break;
        case 3: this.oamAddress = value; break;
        case 4: this.oam[this.oamAddress] = value; this.oamAddress = (this.oamAddress + 1) & 0xff; break;
        case 5: if (!this.w) { this.t = (this.t & ~0x1f) | (value >> 3); this.x = value & 7; this.w = 1; }
                else { this.t = (this.t & ~0x73e0) | ((value & 7) << 12) | ((value & 0xf8) << 2); this.w = 0; } break;
        case 6: if (!this.w) { this.t = (this.t & 0xff) | ((value & 0x3f) << 8); this.w = 1; }
                else { this.t = (this.t & 0x7f00) | value; this.v = this.t; this.w = 0; } break;
        case 7: this.ppuWrite(this.v, value); this.v = (this.v + (this.ctrl & 4 ? 32 : 1)) & 0x7fff; break;
      }
      return;
    }
    if (address === 0x4014) { // OAM DMA from page value
      for (let i = 0; i < 256; i++) this.oam[(this.oamAddress + i) & 0xff] = this.read((value << 8) | i);
      this.cpu.cycles += 513; return;
    }
    if (address === 0x4016) {
      this.strobe = value & 1;
      if (this.strobe) this.horiShift = [this.pads[0] | this.pads[2] << 8 | 0x04 << 16, this.pads[1] | this.pads[3] << 8 | 0x08 << 16];
      if (this.strobe) this.shift = this.fourScore ? [this.pads[0] | this.pads[2] << 8 | 0x08 << 16, this.pads[1] | this.pads[3] << 8 | 0x04 << 16] : this.pads.slice(0, 2);
      return;
    }
    if (address <= 0x4017) { this.apuWrites.push((address - 0x4000) << 8 | value); return; } // APU: logged, not modelled
    if (address < 0x8000) return;
    this.cart.write(address, value);
  }
  reset() { this.cpu.reset(); this.frameStart = this.cpu.cycles; }
  // Runs until the frame's end (the NMI comes at vblank when enabled). Throws past `limit` instructions a frame.
  runFrame(limit = 200000) {
    let steps = 0;
    while (this.cpu.cycles - this.frameStart < FRAME_CYCLES) {
      if (!this.vblankRaised && this.inVblank()) {
        this.vblankRaised = true; this.vblankRead = false;
        if (this.ctrl & 0x80) this.cpu.nmi();
      }
      const hook = this.hooks.get(this.cpu.pc); if (hook) hook(this);
      if (this.coverage && this.cpu.pc >= 0x8000) this.coverage.add((this.cpu.pc >= 0xc000 ? this.cart.banks - 1 : this.cart.bank) * 0x10000 + this.cpu.pc);
      this.cpu.step();
      if (++steps > limit) throw new Error('Frame did not end');
    }
    this.frameStart += FRAME_CYCLES; this.frame++; this.vblankRaised = false; this.vblankRead = false;
  }
}
