import fs from 'node:fs';
import { assemble } from '../asm65816.mjs';
import { VARIANTS, PAL_HEADER, readVariant, variantFile } from './variants.mjs';
// Builds the Gun.Smoke two-player patch (tools/gunsmoke-coop/coop.asm, docs/HACK.md) for each supported
// ROM (tools/gunsmoke-coop/variants.mjs: the USA ROM and the Swedish Krut.Rök); for each one in the working directory:
//   out/<variant out>       the patched ROM with the default options (never in git)
//   out/<id>-coop.ips       the patch (our bytes only)
// and for all of them (assembled without a ROM):
//   tools/gunsmoke-coop/patch_data.hpp           the patcher's data (in git: our assembled bytes only)
//   out/coop.sym            the labels (the same in every variant)
// GS_FRIENDLY_FIRE (0/1) sets the option of the ROMs written here (player 2's colours: the defaults).
// Usage: node tools/gunsmoke-coop/build.mjs
const size = 0x20000;
// RAM: zero page $D6-$DF (cleared at power-on, never used by the original), $0600-$069F (never written by the original,
// not cleared at power-on: everything there is set before it is read; $0600-$0618 the saved player $70-$88). The VRAM
// scripts: the menu's at $0650 on the title (38 bytes), then the cursor's ($0678) and the chosen line's blink ($0650,
// $0660) in its place.
const ram = { ACT: 0xd6, BUSY: 0xd7, BSWAP: 0xd8, MODE2P: 0xd9, INRAM: 0xda, PASS: 0xdb, TA: 0xdc, TY: 0xdd, TX: 0xde, P2RAM: 0xdf,
  SAVE: 0x600, SAVEF5: 0x629, SAVEF7: 0x62a, BTSWAP: 0x62b, OWNED: 0x634, WINO: 0x635, TSWAP: 0x636,
  BEFORE: 0x637, TMP: 0x638, TMP2: 0x639, PBOT: 0x63a, PBANK: 0x63b, TMP3: 0x63c, TMP4: 0x63d, TMP5: 0x63e, LOOPX: 0x63f,
  SHOPW: 0x640, DYING: 0x642, RESPAWN: 0x643, CURSOR: 0x644, DFIG: 0x645, INGAME: 0x646, CODEI: 0x647, GIFT: 0x648,
  MENU: 0x650, BLINKT: 0x650, BLINKB: 0x660, CURS: 0x678 };
// The hooks by the trampoline's return address (the jsr's address + 2) and their handlers, the most frequent first.
const hooks = [['$F258', 'h_target'], ['rsm_t', 'h_rsm'], ['$C069', 'h_pad'], ['stubf_a', 'h_fa'], ['stubf_b', 'h_fb'],
  ['$C947', 'h_c945'], ['$F280', 'h_eend'], ['$F288', 'h_hit'], ['$E91F', 'h_loop'], ['$E86B', 'h_place'],
  ['$F3B8', 'h_lives'], ['$E72F', 'h_init'], ['$F2DE', 'h_window'], ['$C502', 'h_pal'], ['$F35B', 'h_death'], ['$C4C6', 'h_death'], ['$F3E1', 'h_lives_screen'],
  ['$B0B0', 'h_title'], ['$B051', 'h_menu']];
// By the return address's low byte only (only the trampoline's own calls arrive; the build checks they differ).
const dispatch = hooks.map(([at, handler], k) => {
  const lo = at.startsWith('$') ? `$${at.slice(3)}` : `${at}+$02`; // stubf_*, rsm_t in $C1xx
  return [`cmp.b #${lo}`, `bne d${k}`, `jmp.w ${handler}`, `d${k}:`].join('\n');
}).join('\n');
// {NAME} and {NAME}+$NN (summed here: the assembler adds numbers to labels only); {<NAME} / {>NAME} its low / high byte.
const fill = text => text.split('\n').map(line => line.replace(/;.*$/, '')).join('\n').replace(/\{([<>]?)(\w+)\}(?:\+\$([0-9A-F]+))?/gi, (m, part, name, plus) => {
  if (!(name in ram)) throw Error('Unknown placeholder ' + m);
  let value = ram[name] + (plus ? parseInt(plus, 16) : 0);
  if (part === '<') value &= 0xff; else if (part === '>') value >>= 8;
  return '$' + value.toString(16).toUpperCase().padStart(value > 0xff ? 4 : 2, '0');
});
// The swap's single bytes unrolled ($88, $F5, $F7; $70-$87 is a loop of two bytes a turn in coop.asm: the room).
const swapBlock = [[0x88, '{SAVE}+$18'], [0xf5, '{SAVEF5}'], [0xf7, '{SAVEF7}']]
  .map(([zp, saved]) => [`lda.b $${zp.toString(16).toUpperCase()}`, `ldy.w ${saved}`, `sta.w ${saved}`, `sty.b $${zp.toString(16).toUpperCase()}`].join('\n'))
  .join('\n');
// The title's font (the background pattern table: 0-9 = $D0-$D9, A-Z = $DA-$F3, space $F4; the rifle $FC).
const font = text => [...text].map(c => c === ' ' ? 0xf4 : c >= '0' && c <= '9' ? 0xd0 + +c : c >= 'A' && c <= 'Z' ? 0xda + c.charCodeAt(0) - 65 : (() => { throw Error('No tile for ' + c); })());
const db = bytes => 'db ' + bytes.map(b => '$' + b.toString(16).toUpperCase().padStart(2, '0')).join(',');
// The variant's title entry (PUSH START's place: $21C9, the cursor, a space, line 1, padded to the entry's length) and
// the two lines' RAM entries (14 bytes each: $21C9 / $2209, 11 characters, the cursor's place first).
const variantSource = v => {
  const n = v.title.length - 3, line1 = [0xfc, 0xf4, ...font(v.lines[0].trimEnd())];
  if (line1.length > n) throw Error(`${v.id}: line 1 does not fit the title entry`);
  while (line1.length < n) line1.push(0xf4);
  const title = [`org $01${v.title.at.toString(16).toUpperCase()}`, db([0x21, 0xc9, n, ...line1]),
    ...(v.edits ?? []).flatMap(e => [`org $01${e.at.toString(16).toUpperCase()}`, db(font(e.text))])].join('\n');
  const entry = (hi, lo, text) => { const t = font(text); if (t.length !== 9) throw Error(`${v.id}: a line is 9 characters`); return db([hi, lo, 0x0b, 0xf4, 0xf4, ...t]); };
  return fill(fs.readFileSync('tools/gunsmoke-coop/coop.asm', 'utf8').split('{DISPATCH}').join(dispatch).split('{SWAPBLOCK}').join(swapBlock)
    .split('{TITLE}').join(title).split('{LINES}').join(entry(0x21, 0xc9, v.lines[0]) + '\n' + entry(0x22, 0x09, v.lines[1])));
};
const offsetOf = address => (address >> 16) * 0x4000 + (address & 0x3fff);
// Bank 1: the title's hooks ($B04F, $B0AE), the script bytes ($B05A, $B0BF, $B0C3, $B0CC, $B0D0) and the variant's
// title entry. Bank 4: its two unused places ($9000-$93FF and $B800-$BFFF: pages of $00 and $FF fill, never read by
// the original in 100000 frames of play, attract and random play). The fixed bank: the moved code ($C15A-$C208) and the
// hooks. Nothing else (the assembler would wrap past their ends).
const bank1Of = v => new Set([0xb04f, 0xb050, 0xb051, 0xb052, 0xb053, 0xb05a, 0xb0ae, 0xb0af, 0xb0b0, 0xb0b1, 0xb0bf, 0xb0c3, 0xb0cc, 0xb0d0,
  ...[...Array(v.title.length).keys()].map(k => v.title.at + k),
  ...(v.edits ?? []).flatMap(e => [...e.text].map((_, k) => e.at + k))].map(a => 0x4000 + a - 0x8000));
const fillPlace = i => (i >= 4 * 0x4000 + 0x1000 && i < 4 * 0x4000 + 0x1400) || (i >= 4 * 0x4000 + 0x3800 && i < 5 * 0x4000);
// What the patch relies on in a ROM besides the bytes it writes: the fixed bank as the USA ROM's but for the player's
// movement tables, the texts of the game over and the shops and the vectors' neighbours ($DF91-$E118, $F522-$F5AF,
// $F83C-$F9FF, $FFF0-$FFF1: a translation's), the hooks' bank 1 code ($B03E-$B0E0 but the PUSH START scripts'
// addresses) and bank 4's unused places as fill.
const mayDiffer = i => { const a = 0xc000 + (i - 7 * 0x4000); return (a >= 0xdf91 && a <= 0xe118) || (a >= 0xf522 && a <= 0xf5af) || (a >= 0xf83c && a <= 0xf9ff) || a === 0xfff0 || a === 0xfff1; };

const built = new Map();
for (const v of VARIANTS) {
  const { image, written, labels } = assemble([['coop.asm', variantSource(v)]], size, offsetOf);
  for (const name of ['stubf_a', 'stubf_b', 'rsm_t']) if ((labels.get(name) & 0xff00) !== 0xc100) throw Error(name + ' left $C1xx');
  { // the return addresses' low bytes must differ (the dispatcher compares only those)
    const lows = hooks.map(([at]) => at.startsWith('$') ? parseInt(at.slice(3), 16) : (labels.get(at) + 2) & 0xff);
    if (new Set(lows).size !== lows.length) throw Error('Two hooks share a return address low byte: ' + lows.map(x => x.toString(16)).join(' '));
  }
  const fixedEnd = [...Array(0x300).keys()].filter(i => written[7 * 0x4000 + i]).pop() + 0xc000;
  if (fixedEnd > 0xc208) throw Error('The trampoline code passes $C208: $' + fixedEnd.toString(16));
  const bank1 = bank1Of(v);
  for (let i = 0; i < 7 * 0x4000; i++) if (written[i] && !bank1.has(i) && !fillPlace(i)) throw Error(`${v.id}: written outside its places: PRG $${i.toString(16)}`);
  if (labels.get('end_b800') > 0x04c000 || labels.get('end_b800') < 0x04b800) throw Error('The code passes $BFFF');
  if (labels.get('end_9000') > 0x049400) throw Error('The code at $9000 passes $93FF');
  built.set(v.id, { v, image, written, labels, fixedEnd });
}
// One set of labels for all (the variants differ in data only): the options and the tests' symbols in the same places.
const labelText = b => [...b.labels].map(([n, a]) => a.toString(16) + ' ' + n).join('\n');
const first = built.get(VARIANTS[0].id);
for (const b of built.values()) if (labelText(b) !== labelText(first)) throw Error(`${b.v.id}: its labels differ from ${first.v.id}'s`);
const configOffset = 4 * 0x4000 + (first.labels.get('config') & 0x3fff);
fs.mkdirSync('out', { recursive: true });
fs.writeFileSync('out/coop.sym', labelText(first) + '\n');

// The ROMs present: checked and written.
const usaFile = variantFile(VARIANTS[0]), usa = usaFile ? readVariant(usaFile).rom.prg : null;
for (const b of built.values()) {
  const path = variantFile(b.v);
  if (!path) { console.log(`${b.v.id}: no ${b.v.file} here (its patch data built, not checked against the ROM)`); continue; }
  const { file, rom } = readVariant(path), prg = rom.prg;
  if (rom.sha256 !== b.v.sha256) throw Error(`${path} is not ${b.v.name}`);
  for (let i = 0; i < size; i++) if (b.written[i] && fillPlace(i) && prg[i] !== 0 && prg[i] !== 0xff) throw Error(`${b.v.id}: bank 4's place is not fill at PRG $${i.toString(16)}`);
  if (usa && b.v.id !== 'usa') {
    const bank1 = bank1Of(b.v), pointers = new Set([0xb0bf, 0xb0c3, 0xb0cc, 0xb0d0].map(a => 0x4000 + a - 0x8000));
    for (let i = 7 * 0x4000; i < size; i++) if (prg[i] !== usa[i] && !mayDiffer(i)) throw Error(`${b.v.id}: the fixed bank differs from the USA ROM's at $${(0xc000 + i - 7 * 0x4000).toString(16)}`);
    for (let i = 0x4000 + 0x303e; i <= 0x4000 + 0x30e0; i++) if (prg[i] !== usa[i] && !pointers.has(i)) throw Error(`${b.v.id}: the title's code differs at $1:${(0x8000 + i - 0x4000).toString(16)}`);
    for (let i = 0; i < size; i++) if (b.written[i] && !bank1.has(i) && prg[i] !== usa[i]) throw Error(`${b.v.id}: a hooked byte differs from the USA ROM's at PRG $${i.toString(16)}`);
  }
  const out = Buffer.from(file);
  if (b.v.pal) PAL_HEADER.copy(out, 0); // timing PAL for emulators and the MiSTer (variants.mjs)
  for (let i = 0; i < size; i++) if (b.written[i]) out[16 + i] = b.image[i];
  out[16 + configOffset] = Number(process.env.GS_FRIENDLY_FIRE ?? 0) ? 1 : 0;
  // IPS: every byte that differs from the original (with the header: offsets in the .nes file).
  const records = [];
  for (let i = 0; i < out.length;) {
    if (out[i] === file[i]) { i++; continue; }
    let j = i; while (j < out.length && j - i < 0xffff && out[j] !== file[j]) j++;
    records.push([i, out.subarray(i, j)]); i = j;
  }
  const parts = [Buffer.from('PATCH')];
  for (const [at, data] of records) { const h = Buffer.alloc(5); h.writeUIntBE(at, 0, 3); h.writeUInt16BE(data.length, 3); parts.push(h, data); }
  parts.push(Buffer.from('EOF'));
  fs.writeFileSync(`out/${b.v.out}`, out);
  fs.writeFileSync(`out/${b.v.id}-coop.ips`, Buffer.concat(parts));
  let bankEnd = 0; for (let i = 4 * 0x4000 + 0x3800; i < 5 * 0x4000; i++) if (b.written[i]) bankEnd = 0x8000 + (i & 0x3fff);
  console.log(`${b.v.id}: ${records.length} IPS records; fixed bank to $${b.fixedEnd.toString(16)}, bank 4 $B800-$${bankEnd.toString(16)} and ` +
    `$9000-$${(b.labels.get('end_9000') & 0xffff).toString(16)}; options at PRG $${configOffset.toString(16)}: friendly fire ${out[16 + configOffset]}`);
}

// The patcher's data: each variant's assembled bytes as runs of the written mask (PRG offsets; no ROM content).
const hex = x => '0x' + x.toString(16).padStart(2, '0');
const lines = ['// Generated by tools/gunsmoke-coop/build.mjs from coop.asm: our own assembled bytes only (no ROM data).', '#pragma once',
  '#include <cstddef>', '#include <cstdint>', '#include <iterator>', 'namespace coop {', 'struct Run { std::uint32_t offset; std::uint32_t size; const std::uint8_t* bytes; };',
  '// A supported ROM (tools/gunsmoke-coop/variants.mjs): its name for the page, the SHA-256 of the ROM without its header, its runs.',
  '// pal: the patched file gets the NES 2.0 header pal_header (timing PAL; tools/gunsmoke-coop/variants.mjs PAL_HEADER).',
  'struct Variant { const char* name; const char* sha256; const Run* runs; std::size_t count; bool pal; };',
  `inline constexpr unsigned char pal_header[16] = {${[...PAL_HEADER].map(x => '0x' + x.toString(16).padStart(2, '0')).join(', ')}};`,
  `// The options' bytes (PRG offset, the same in every variant): friendly fire, player 2's palette (1-3), own colours (0/1), colour 1-3.`,
  `constexpr std::uint32_t config_offset = 0x${configOffset.toString(16)};`];
let total = [];
for (const b of built.values()) {
  const runs = [];
  for (let i = 0; i < size;) { if (!b.written[i]) { i++; continue; } let j = i; while (j < size && b.written[j]) j++; runs.push([i, b.image.subarray(i, j)]); i = j; }
  runs.forEach(([, bytes], k) => { lines.push(`inline const std::uint8_t ${b.v.id}${k}[] = {`); for (let i = 0; i < bytes.length; i += 20) lines.push('    ' + [...bytes.subarray(i, i + 20)].map(hex).join(', ') + ','); lines.push('};'); });
  lines.push(`inline const Run ${b.v.id}_runs[] = {`, ...runs.map(([at, bytes], k) => `    {0x${at.toString(16)}, ${bytes.length}, ${b.v.id}${k}},`), '};');
  total.push(`${b.v.id} ${runs.length} runs, ${runs.reduce((s, [, x]) => s + x.length, 0)} bytes`);
}
lines.push('inline const Variant variants[] = {', ...[...built.values()].map(b => `    {"${b.v.name}", "${b.v.sha256}", ${b.v.id}_runs, std::size(${b.v.id}_runs), ${b.v.pal ? 'true' : 'false'}},`), '};');
// The debug states' Mesen script (tools/gunsmoke-coop/mesen-states.lua, our own) for the patcher's MESEN DEBUG STATES.
const lua = fs.readFileSync('tools/gunsmoke-coop/mesen-states.lua', 'utf8').split('\r\n').join('\n');
if (lua.includes(')GSLUA')) throw Error('The Lua script contains the raw string delimiter');
lines.push('// tools/gunsmoke-coop/mesen-states.lua', `inline constexpr const char* mesen_states_lua = R"GSLUA(${lua})GSLUA";`, '}', '');
fs.writeFileSync('tools/gunsmoke-coop/patch_data.hpp', lines.join('\n'));
console.log(`Patcher data: ${total.join('; ')}.`);
