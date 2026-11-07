import { opcodeMatrix } from './rom.mjs';

// Minimal assembler for the asar-compatible subset written by disasm-export.mjs:
// lorom, org, labels, db, and 65816 instructions with explicit .b/.w/.l sizes.
// It exists to prove that an exported listing rebuilds the identical ROM; it is
// not a general-purpose asar replacement.

const opcodes = new Map();
opcodeMatrix.forEach(([mnemonic, mode], opcode) => {
  const key = `${mnemonic}|${mode === 'm' || mode === 'x' ? 'imm' : mode}`;
  if (!opcodes.has(key)) opcodes.set(key, opcode);
});
const branchModes = new Map(opcodeMatrix.filter(([, mode]) => mode === 'r' || mode === 'rl').map(([m, mode]) => [m, mode]));

function number(text) {
  if (text.startsWith('$')) return parseInt(text.slice(1), 16);
  if (/^\d+$/.test(text)) return parseInt(text, 10);
  return NaN;
}

// Returns [mode, value text] for an operand, given the size suffix.
function classify(operand, size) {
  if (operand === '') return ['i', null];
  if (operand.toLowerCase() === 'a') return ['A', null];
  let m;
  if ((m = /^#(.+)$/.exec(operand))) return ['#', m[1]];
  if ((m = /^\((.+),s\),y$/i.exec(operand))) return ['siy', m[1]];
  if ((m = /^(.+),s$/i.exec(operand))) return ['sr', m[1]];
  if ((m = /^\((.+),x\)$/i.exec(operand))) return [size === 'b' ? 'dxi' : 'axi', m[1]];
  if ((m = /^\((.+)\),y$/i.exec(operand))) return ['diy', m[1]];
  if ((m = /^\((.+)\)$/.exec(operand))) return [size === 'b' ? 'di' : 'ai', m[1]];
  if ((m = /^\[(.+)\],y$/i.exec(operand))) return ['dly', m[1]];
  if ((m = /^\[(.+)\]$/.exec(operand))) return [size === 'b' ? 'dl' : 'ali', m[1]];
  if ((m = /^(.+),x$/i.exec(operand))) return [{ b: 'dx', w: 'ax', l: 'lx' }[size], m[1]];
  if ((m = /^(.+),y$/i.exec(operand))) return [{ b: 'dy', w: 'ay' }[size], m[1]];
  return [{ b: 'd', w: 'a', l: 'l' }[size], operand];
}

// offsetOf: file offset of an address (default LoROM: 32 KiB banks); the NES export passes bank * 16 KiB.
export function assemble(sources, size, offsetOf = address => ((address >> 16) & 0x7f) * 0x8000 + (address & 0x7fff)) {
  const image = new Uint8Array(size), written = new Uint8Array(size);
  const labels = new Map();
  const lines = [];
  for (const [file, text] of sources) text.split(/\r?\n/).forEach((raw, i) => {
    const line = raw.replace(/;.*$/, '').trim();
    if (line) lines.push({ file, number: i + 1, line });
  });
  const fail = (entry, message) => { throw new Error(`${entry.file}:${entry.number}: ${message}`); };
  for (let pass = 0; pass < 2; ++pass) {
    let pc = null;
    for (const entry of lines) {
      const { line } = entry;
      if (line === 'lorom') continue;
      let m;
      if ((m = /^org\s+\$([0-9a-f]{6})$/i.exec(line))) { pc = parseInt(m[1], 16); continue; }
      if ((m = /^([A-Za-z_][\w]*):$/.exec(line))) {
        if (pass === 0) { if (labels.has(m[1])) fail(entry, 'duplicate label ' + m[1]); labels.set(m[1], pc); }
        continue;
      }
      if (pc === null) fail(entry, 'code before org');
      const emit = bytes => {
        for (const byte of bytes) {
          const offset = offsetOf(pc);
          if (offset >= size) fail(entry, 'outside image');
          if (pass === 1) {
            if (written[offset]) fail(entry, `overlapping output at ${pc.toString(16)}`);
            image[offset] = byte; written[offset] = 1;
          }
          pc = (pc & 0xff0000) | ((pc + 1) & 0xffff);
        }
      };
      if ((m = /^db\s+(.+)$/i.exec(line))) {
        emit(m[1].split(',').map(t => { const v = number(t.trim()); if (!(v >= 0 && v < 256)) fail(entry, 'bad byte'); return v; }));
        continue;
      }
      // Block moves: "mvn $src,$dst" (WDC order) is encoded opcode, destination bank, source bank.
      if ((m = /^(mvn|mvp)\s+\$([0-9a-f]{2}),\s*\$([0-9a-f]{2})$/i.exec(line))) {
        emit([m[1].toLowerCase() === 'mvn' ? 0x54 : 0x44, parseInt(m[3], 16), parseInt(m[2], 16)]);
        continue;
      }
      if (!(m = /^([a-z]{3})(?:\.([bwl]))?(?:\s+(.*))?$/i.exec(line))) fail(entry, 'cannot parse');
      const mnemonic = m[1].toUpperCase(), suffix = m[2]?.toLowerCase(), operand = (m[3] ?? '').trim();
      const branch = branchModes.get(mnemonic);
      if (branch) {
        const target = pass === 0 ? pc : (labels.get(operand) ?? number(operand));
        if (!Number.isFinite(target)) fail(entry, 'unknown label ' + operand);
        const length = branch === 'r' ? 2 : 3;
        const delta = ((target & 0xffff) - ((pc + length) & 0xffff) + 0x10000) & 0xffff;
        const signed = delta >= 0x8000 ? delta - 0x10000 : delta;
        if (pass === 1 && branch === 'r' && (signed < -128 || signed > 127)) fail(entry, 'branch out of range');
        emit(branch === 'r' ? [opcodes.get(`${mnemonic}|r`), signed & 255] : [opcodes.get(`${mnemonic}|rl`), delta & 255, delta >> 8]);
        continue;
      }
      const [mode, valueText] = classify(operand, suffix);
      let key = `${mnemonic}|${mode}`, width;
      if (mode === '#') {
        if (opcodes.has(`${mnemonic}|imm`)) key = `${mnemonic}|imm`;
        else if (opcodes.has(`${mnemonic}|8`)) key = `${mnemonic}|8`;
        width = suffix === 'w' ? 2 : 1;
      } else if (mnemonic === 'PEA') { key = 'PEA|16'; width = 2; }
      else width = { i: 0, A: 0, d: 1, dx: 1, dy: 1, dxi: 1, diy: 1, di: 1, dl: 1, dly: 1, sr: 1, siy: 1,
        a: 2, ax: 2, ay: 2, ai: 2, axi: 2, ali: 2, l: 3, lx: 3 }[mode];
      const opcode = opcodes.get(key);
      if (opcode === undefined || width === undefined) fail(entry, `no opcode for ${mnemonic} ${operand}`);
      // A label (optionally + a number) as an operand: patches written in this subset (snes-smooth/).
      let value = valueText === null ? 0 : number(valueText);
      // >label / <label: its high / low byte.
      if (!Number.isFinite(value) && valueText !== null && (m = /^([<>]?)([A-Za-z_]\w*)(?:\+(\$?[0-9a-f]+))?$/i.exec(valueText))) {
        value = pass === 0 ? 0 : (labels.get(m[2]) ?? NaN) + (m[3] ? number(m[3]) : 0);
        if (m[1] === '>') value = (value >> 8) & 255; else if (m[1] === '<') value &= 255;
      }
      if (!Number.isFinite(value)) fail(entry, 'bad operand ' + operand);
      emit([opcode, ...Array.from({ length: width }, (_, i) => (value >> (8 * i)) & 255)]);
    }
  }
  const missing = written.indexOf(0);
  return { image, written, labels, complete: missing < 0, firstMissing: missing };
}
