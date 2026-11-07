// 6502 (NES 2A03: no decimal mode) opcode table, disassembler helpers and an interpreter for analysis and ROM
// oracles. The bus is supplied by the caller (read(address), write(address, value)); tools/nes/rom.mjs has the
// cartridge side. Unofficial opcodes stop the interpreter (the game is not expected to use them).

// [mnemonic, mode] per opcode; modes: imp acc imm zp zpx zpy abs abx aby ind izx izy rel.
export const OPCODES = new Array(256).fill(null);
const table = {
  ADC: { imm: 0x69, zp: 0x65, zpx: 0x75, abs: 0x6d, abx: 0x7d, aby: 0x79, izx: 0x61, izy: 0x71 },
  AND: { imm: 0x29, zp: 0x25, zpx: 0x35, abs: 0x2d, abx: 0x3d, aby: 0x39, izx: 0x21, izy: 0x31 },
  ASL: { acc: 0x0a, zp: 0x06, zpx: 0x16, abs: 0x0e, abx: 0x1e },
  BCC: { rel: 0x90 }, BCS: { rel: 0xb0 }, BEQ: { rel: 0xf0 }, BMI: { rel: 0x30 }, BNE: { rel: 0xd0 },
  BPL: { rel: 0x10 }, BVC: { rel: 0x50 }, BVS: { rel: 0x70 },
  BIT: { zp: 0x24, abs: 0x2c }, BRK: { imp: 0x00 },
  CLC: { imp: 0x18 }, CLD: { imp: 0xd8 }, CLI: { imp: 0x58 }, CLV: { imp: 0xb8 },
  CMP: { imm: 0xc9, zp: 0xc5, zpx: 0xd5, abs: 0xcd, abx: 0xdd, aby: 0xd9, izx: 0xc1, izy: 0xd1 },
  CPX: { imm: 0xe0, zp: 0xe4, abs: 0xec }, CPY: { imm: 0xc0, zp: 0xc4, abs: 0xcc },
  DEC: { zp: 0xc6, zpx: 0xd6, abs: 0xce, abx: 0xde }, DEX: { imp: 0xca }, DEY: { imp: 0x88 },
  EOR: { imm: 0x49, zp: 0x45, zpx: 0x55, abs: 0x4d, abx: 0x5d, aby: 0x59, izx: 0x41, izy: 0x51 },
  INC: { zp: 0xe6, zpx: 0xf6, abs: 0xee, abx: 0xfe }, INX: { imp: 0xe8 }, INY: { imp: 0xc8 },
  JMP: { abs: 0x4c, ind: 0x6c }, JSR: { abs: 0x20 },
  LDA: { imm: 0xa9, zp: 0xa5, zpx: 0xb5, abs: 0xad, abx: 0xbd, aby: 0xb9, izx: 0xa1, izy: 0xb1 },
  LDX: { imm: 0xa2, zp: 0xa6, zpy: 0xb6, abs: 0xae, aby: 0xbe },
  LDY: { imm: 0xa0, zp: 0xa4, zpx: 0xb4, abs: 0xac, abx: 0xbc },
  LSR: { acc: 0x4a, zp: 0x46, zpx: 0x56, abs: 0x4e, abx: 0x5e }, NOP: { imp: 0xea },
  ORA: { imm: 0x09, zp: 0x05, zpx: 0x15, abs: 0x0d, abx: 0x1d, aby: 0x19, izx: 0x01, izy: 0x11 },
  PHA: { imp: 0x48 }, PHP: { imp: 0x08 }, PLA: { imp: 0x68 }, PLP: { imp: 0x28 },
  ROL: { acc: 0x2a, zp: 0x26, zpx: 0x36, abs: 0x2e, abx: 0x3e },
  ROR: { acc: 0x6a, zp: 0x66, zpx: 0x76, abs: 0x6e, abx: 0x7e },
  RTI: { imp: 0x40 }, RTS: { imp: 0x60 },
  SBC: { imm: 0xe9, zp: 0xe5, zpx: 0xf5, abs: 0xed, abx: 0xfd, aby: 0xf9, izx: 0xe1, izy: 0xf1 },
  SEC: { imp: 0x38 }, SED: { imp: 0xf8 }, SEI: { imp: 0x78 },
  STA: { zp: 0x85, zpx: 0x95, abs: 0x8d, abx: 0x9d, aby: 0x99, izx: 0x81, izy: 0x91 },
  STX: { zp: 0x86, zpy: 0x96, abs: 0x8e }, STY: { zp: 0x84, zpx: 0x94, abs: 0x8c },
  TAX: { imp: 0xaa }, TAY: { imp: 0xa8 }, TSX: { imp: 0xba }, TXA: { imp: 0x8a }, TXS: { imp: 0x9a }, TYA: { imp: 0x98 },
};
for (const [mnemonic, modes] of Object.entries(table)) for (const [mode, op] of Object.entries(modes)) OPCODES[op] = [mnemonic, mode];
export const SIZE = { imp: 1, acc: 1, imm: 2, zp: 2, zpx: 2, zpy: 2, izx: 2, izy: 2, rel: 2, abs: 3, abx: 3, aby: 3, ind: 3 };

const hex = (v, n) => v.toString(16).toUpperCase().padStart(n, '0');
// One instruction at address (bytes from read): { size, text, mnemonic, mode, operand, target }.
export function decode(read, address) {
  const op = read(address), entry = OPCODES[op];
  if (!entry) return { size: 1, text: `.db $${hex(op, 2)}`, mnemonic: null };
  const [mnemonic, mode] = entry, size = SIZE[mode];
  const lo = size > 1 ? read((address + 1) & 0xffff) : 0, hi = size > 2 ? read((address + 2) & 0xffff) : 0;
  const word = lo | (hi << 8);
  let operand = '', target = null;
  switch (mode) {
    case 'acc': operand = 'a'; break;
    case 'imm': operand = `#$${hex(lo, 2)}`; break;
    case 'zp': operand = `$${hex(lo, 2)}`; break;
    case 'zpx': operand = `$${hex(lo, 2)},x`; break;
    case 'zpy': operand = `$${hex(lo, 2)},y`; break;
    case 'izx': operand = `($${hex(lo, 2)},x)`; break;
    case 'izy': operand = `($${hex(lo, 2)}),y`; break;
    case 'abs': operand = `$${hex(word, 4)}`; target = word; break;
    case 'abx': operand = `$${hex(word, 4)},x`; break;
    case 'aby': operand = `$${hex(word, 4)},y`; break;
    case 'ind': operand = `($${hex(word, 4)})`; break;
    case 'rel': target = (address + 2 + (lo < 128 ? lo : lo - 256)) & 0xffff; operand = `$${hex(target, 4)}`; break;
  }
  return { size, mnemonic, mode, operand, target, text: (mnemonic.toLowerCase() + (operand ? ' ' + operand : '')) };
}

// Interpreter. bus: { read(address), write(address, value) }. Cycle counts are the documented base counts plus
// page-cross and branch penalties (enough for frame budgets in analysis, not cycle-exact PPU timing).
export class Cpu6502 {
  constructor(bus) { this.bus = bus; this.a = 0; this.x = 0; this.y = 0; this.s = 0xfd; this.p = 0x24; this.pc = 0; this.cycles = 0; }
  read(a) { return this.bus.read(a & 0xffff) & 0xff; }
  write(a, v) { this.bus.write(a & 0xffff, v & 0xff); }
  read16(a) { return this.read(a) | (this.read(a + 1) << 8); }
  push(v) { this.write(0x100 | this.s, v); this.s = (this.s - 1) & 0xff; }
  pull() { this.s = (this.s + 1) & 0xff; return this.read(0x100 | this.s); }
  flag(bit, on) { this.p = on ? this.p | bit : this.p & ~bit; }
  nz(v) { this.flag(0x02, (v & 0xff) === 0); this.flag(0x80, v & 0x80); return v & 0xff; }
  reset() { this.pc = this.read16(0xfffc); this.s = 0xfd; this.p = 0x24; }
  interrupt(vector, brk = false) {
    this.push(this.pc >> 8); this.push(this.pc & 0xff); this.push((this.p | 0x20 | (brk ? 0x10 : 0)) & ~(brk ? 0 : 0x10));
    this.p |= 0x04; this.pc = this.read16(vector); this.cycles += 7;
  }
  nmi() { this.interrupt(0xfffa); }
  // Effective address of a memory mode; extra cycle on page cross for reads (penalty flag).
  address(mode, penalty) {
    const pc = this.pc;
    switch (mode) {
      case 'zp': this.pc += 1; return this.read(pc);
      case 'zpx': this.pc += 1; return (this.read(pc) + this.x) & 0xff;
      case 'zpy': this.pc += 1; return (this.read(pc) + this.y) & 0xff;
      case 'abs': this.pc += 2; return this.read16(pc);
      case 'abx': case 'aby': {
        this.pc += 2; const base = this.read16(pc), a = (base + (mode === 'abx' ? this.x : this.y)) & 0xffff;
        if (penalty && (base & 0xff00) !== (a & 0xff00)) this.cycles += 1; return a;
      }
      case 'izx': { this.pc += 1; const z = (this.read(pc) + this.x) & 0xff; return this.read(z) | (this.read((z + 1) & 0xff) << 8); }
      case 'izy': {
        this.pc += 1; const z = this.read(pc), base = this.read(z) | (this.read((z + 1) & 0xff) << 8), a = (base + this.y) & 0xffff;
        if (penalty && (base & 0xff00) !== (a & 0xff00)) this.cycles += 1; return a;
      }
      case 'ind': { this.pc += 2; const p = this.read16(pc); return this.read(p) | (this.read((p & 0xff00) | ((p + 1) & 0xff)) << 8); }
    }
    throw new Error('Bad mode ' + mode);
  }
  operand(mode) { if (mode === 'imm') { const v = this.read(this.pc); this.pc += 1; return v; } return this.read(this.address(mode, true)); }
  adc(v) {
    const c = this.p & 1, r = this.a + v + c;
    this.flag(0x40, (~(this.a ^ v) & (this.a ^ r)) & 0x80); this.flag(0x01, r > 0xff); this.a = this.nz(r);
  }
  branch(cond) {
    const off = this.read(this.pc); this.pc = (this.pc + 1) & 0xffff;
    if (cond) { const t = (this.pc + (off < 128 ? off : off - 256)) & 0xffff; this.cycles += (t & 0xff00) !== (this.pc & 0xff00) ? 2 : 1; this.pc = t; }
  }
  // Runs one instruction; returns its mnemonic.
  step() {
    const at = this.pc, op = this.read(at), entry = OPCODES[op];
    if (!entry) throw new Error(`Unofficial opcode $${hex(op, 2)} at $${hex(at, 4)}`);
    const [m, mode] = entry; this.pc = (at + 1) & 0xffff;
    this.cycles += BASE[op];
    const rmw = (f) => {
      if (mode === 'acc') { this.a = this.nz(f(this.a)); return; }
      const a = this.address(mode, false), v = this.read(a); this.write(a, v); const r = this.nz(f(v)); this.write(a, r);
    };
    switch (m) {
      case 'LDA': this.a = this.nz(this.operand(mode)); break;
      case 'LDX': this.x = this.nz(this.operand(mode)); break;
      case 'LDY': this.y = this.nz(this.operand(mode)); break;
      case 'STA': this.write(this.address(mode, false), this.a); break;
      case 'STX': this.write(this.address(mode, false), this.x); break;
      case 'STY': this.write(this.address(mode, false), this.y); break;
      case 'ADC': this.adc(this.operand(mode)); break;
      case 'SBC': this.adc(this.operand(mode) ^ 0xff); break;
      case 'AND': this.a = this.nz(this.a & this.operand(mode)); break;
      case 'ORA': this.a = this.nz(this.a | this.operand(mode)); break;
      case 'EOR': this.a = this.nz(this.a ^ this.operand(mode)); break;
      case 'CMP': case 'CPX': case 'CPY': {
        const r = m === 'CMP' ? this.a : m === 'CPX' ? this.x : this.y, v = this.operand(mode);
        this.flag(0x01, r >= v); this.nz(r - v); break;
      }
      case 'BIT': { const v = this.operand(mode); this.flag(0x02, (this.a & v) === 0); this.flag(0x80, v & 0x80); this.flag(0x40, v & 0x40); break; }
      case 'ASL': rmw(v => { this.flag(0x01, v & 0x80); return v << 1; }); break;
      case 'LSR': rmw(v => { this.flag(0x01, v & 1); return v >> 1; }); break;
      case 'ROL': rmw(v => { const c = this.p & 1; this.flag(0x01, v & 0x80); return (v << 1) | c; }); break;
      case 'ROR': rmw(v => { const c = this.p & 1; this.flag(0x01, v & 1); return (v >> 1) | (c << 7); }); break;
      case 'INC': rmw(v => v + 1); break;
      case 'DEC': rmw(v => v - 1); break;
      case 'INX': this.x = this.nz(this.x + 1); break;
      case 'INY': this.y = this.nz(this.y + 1); break;
      case 'DEX': this.x = this.nz(this.x - 1); break;
      case 'DEY': this.y = this.nz(this.y - 1); break;
      case 'TAX': this.x = this.nz(this.a); break;
      case 'TAY': this.y = this.nz(this.a); break;
      case 'TXA': this.a = this.nz(this.x); break;
      case 'TYA': this.a = this.nz(this.y); break;
      case 'TSX': this.x = this.nz(this.s); break;
      case 'TXS': this.s = this.x; break;
      case 'PHA': this.push(this.a); break;
      case 'PHP': this.push(this.p | 0x30); break;
      case 'PLA': this.a = this.nz(this.pull()); break;
      case 'PLP': this.p = (this.pull() & ~0x10) | 0x20; break;
      case 'CLC': this.p &= ~0x01; break; case 'SEC': this.p |= 0x01; break;
      case 'CLI': this.p &= ~0x04; break; case 'SEI': this.p |= 0x04; break;
      case 'CLV': this.p &= ~0x40; break; case 'CLD': this.p &= ~0x08; break; case 'SED': this.p |= 0x08; break;
      case 'BCC': this.branch(!(this.p & 0x01)); break; case 'BCS': this.branch(this.p & 0x01); break;
      case 'BNE': this.branch(!(this.p & 0x02)); break; case 'BEQ': this.branch(this.p & 0x02); break;
      case 'BPL': this.branch(!(this.p & 0x80)); break; case 'BMI': this.branch(this.p & 0x80); break;
      case 'BVC': this.branch(!(this.p & 0x40)); break; case 'BVS': this.branch(this.p & 0x40); break;
      case 'JMP': this.pc = this.address(mode, false); break;
      case 'JSR': { const t = this.read16(this.pc), ret = (this.pc + 1) & 0xffff; this.push(ret >> 8); this.push(ret & 0xff); this.pc = t; break; }
      case 'RTS': this.pc = ((this.pull() | (this.pull() << 8)) + 1) & 0xffff; break;
      case 'RTI': this.p = (this.pull() & ~0x10) | 0x20; this.pc = this.pull() | (this.pull() << 8); break;
      case 'BRK': this.pc = (this.pc + 1) & 0xffff; this.interrupt(0xfffe, true); break;
      case 'NOP': break;
      default: throw new Error('Unhandled ' + m);
    }
    return m;
  }
}
// Base cycle counts of the official opcodes.
const BASE = new Uint8Array(256);
{
  const c = { imp: 2, acc: 2, imm: 2, zp: 3, zpx: 4, zpy: 4, abs: 4, abx: 4, aby: 4, izx: 6, izy: 5, rel: 2, ind: 5 };
  for (let op = 0; op < 256; op++) {
    const e = OPCODES[op]; if (!e) continue; const [m, mode] = e; let n = c[mode];
    if (['ASL', 'LSR', 'ROL', 'ROR', 'INC', 'DEC'].includes(m) && mode !== 'acc') n = { zp: 5, zpx: 6, abs: 6, abx: 7 }[mode];
    if (m === 'STA' && (mode === 'abx' || mode === 'aby')) n = 5;
    if (m === 'STA' && mode === 'izy') n = 6;
    if (m === 'JMP') n = mode === 'abs' ? 3 : 5;
    if (m === 'JSR' || m === 'RTS' || m === 'RTI') n = 6;
    if (m === 'PHA' || m === 'PHP') n = 3;
    if (m === 'PLA' || m === 'PLP') n = 4;
    if (m === 'BRK') n = 7;
    BASE[op] = n;
  }
}
