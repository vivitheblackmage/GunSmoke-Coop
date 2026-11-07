import fs from 'node:fs';
import { VARIANTS, readVariant, variantFile } from './variants.mjs';
import { NesMachine } from '../nes/machine.mjs';
// Tests the co-op patch (tools/gunsmoke-coop/build.mjs) in tools/nes/machine.mjs:
//   alone: the patched ROM with nobody on pad 2 against the original, the same pads (the oracles' "game" pattern with
//          the player shielded and the poster held): RAM equal at every NMI apart from the patch's own RAM and the
//          stack page, the same lag frames;
//   title: 1 PLAYER and 2 PLAYERS on the title screen, the cursor (the rifle) moved by SELECT;
//   two players: 2 PLAYERS chosen, both play patterns, friendly fire off and on: both enter at the start, move and fire,
//          three lives each (apart), a player hit while the other plays dies where they stand (never the original's
//          fade while the other plays), comes back blinking, is out with no lives left; the last one dies the
//          original's way; the game over when both are out; a rider always has the horse object; the lives in the
//          corners; player 2's sprites in her palette; a boss aims at her, a shop's upgrades go to both; no crash (the
//          CPU stays in ROM), and two runs end equal.
// Usage: node tools/gunsmoke-coop/test.mjs [rom] [alone frames] [two-player frames]
//   rom: a supported ROM (tools/gunsmoke-coop/variants.mjs; default the USA one), patched by build.mjs before
const { variant, rom: original } = readVariant(process.argv[2] || variantFile(VARIANTS[0]));
console.log(`${variant.name}`);
const aloneFrames = Number(process.argv[3] ?? 30000), twoFrames = Number(process.argv[4] ?? 40000);
const patchedFile = fs.readFileSync(`out/${variant.out}`);
const patchedPrg = (options) => {
  const prg = Buffer.from(patchedFile.subarray(16, 16 + original.prg.length));
  const at = Number(/config_offset = (0x[0-9a-f]+)/.exec(fs.readFileSync('tools/gunsmoke-coop/patch_data.hpp', 'utf8'))[1]);
  if (options) options.forEach((v, i) => { prg[at + i] = v; });
  return prg;
};
const symbols = new Map(fs.readFileSync('out/coop.sym', 'utf8').trim().split('\n').map(l => { const [a, n] = l.split(' '); return [n, parseInt(a, 16)]; }));
const boss = f => f >= 60 && f < 66 ? 0x08 : f < 830 ? 0 :
  ((f >> 6) & 1 ? 0x40 : 0x80) | ((f >> 7) & 1 ? 0x10 : 0) | ((f >> 2) & 1 ? 0x01 : 0) | (((f >> 3) & 3) === 1 ? 0x02 : 0);
const game = (f, m) => {
  // SELECT in a window held two frames (a press of one frame can fall between two pad reads, as in Krut.Rök's course)
  const select = m.ram[0xaa] && (f & 31) < 2 ? 0x04 : 0;
  if (m.ram[0x4b] !== 1) return boss(f) | select;
  // The aim: the boss (slot $0E) or, before it comes, an enemy left in slots $10-$16 (the boss waits for them; a
  // window shooter by a wall stopped the course for good with straight shots only, 2026-11-01). Straight up (A+B)
  // from below it, else from where a diagonal shot (B up-left, A up-right, 45 degrees) reaches it.
  let bx = m.ram[0x5ee], by = m.ram[0x5ce];
  if (!(m.ram[0x40e] & 0x80)) for (let k = 0x10; k <= 0x16; k++) if (m.ram[0x400 + k] & 0x80) { bx = m.ram[0x5e0 + k]; by = m.ram[0x5c0 + k]; break; }
  const y = m.ram[0x71], x = m.ram[0x74], dy = Math.max(0, y - by);
  let aim = bx, fire = 0x03;
  if (x > bx + 12 && bx + dy < 0xf0) { aim = bx + dy; fire = 0x02; } else if (x + 12 < bx && bx - dy > 0x10) { aim = bx - dy; fire = 0x01; }
  return select | (y < 0xb0 ? 0x20 : 0) | (x + 4 < aim ? 0x80 : x > aim + 4 ? 0x40 : 0) | ((f & 7) < 2 ? fire : 0);
};
const machine = prg => {
  const m = new NesMachine({ ...original, prg });
  const step = m.cpu.step.bind(m.cpu);
  m.cpu.step = () => { // the CPU must stay in the ROM's code (a wild jump into RAM or a BRK means a broken patch)
    if (m.cpu.pc < 0x8000) throw Error(`PC $${m.cpu.pc.toString(16)} outside the ROM at frame ${m.frame}`);
    step();
  };
  // An object drawn ($C978) with bank 4 mapped draws other pictures (its frames may be in the mapped bank): never.
  if (prg !== original.prg) m.hooks.set(0xc978, mm => { if (mm.cart.bank === 4) objectsInBank4++; });
  return m;
};
let objectsInBank4 = 0;
// Not compared: the stack page, the patch's RAM, the scheduler's tasks ($10-$1F) and its $20/$21/$23/$24 (when a task
// is caught there depends on the cycles).
// $26: the low byte of a VRAM script's address (the title's cursor script; the NMI clears only $27).
const skip = a => (a >= 0x100 && a < 0x200) || (a >= 0xd6 && a <= 0xdf) || (a >= 0x600 && a < 0x6a0) || (a >= 0x10 && a <= 0x24) || a === 0x26;
let failed = false;

// --- alone
// Which ready task runs first in a frame follows the cycles (in the original too), so a patch that
// takes time cannot stay equal to the cycle: RAM is compared exactly (but the random numbers $AC-$AF) as long as it
// stays equal, and over the whole run the game must go the same way: the same stages and rounds, no crash.
{
  const a = machine(original.prg), b = machine(patchedPrg());
  let first = -1, lagA = 0, lagB = 0, compared = 0;
  const snapA = [], snapB = [], course = [[], []];
  // Compared where the play loop starts a frame ($E91D) and where task 0 reads the pad ($C067): points in the game's
  // own order (an NMI comes a few cycles later in the patched ROM and can catch the scheduler elsewhere).
  const hook = (m, snaps, inc, k) => {
    const nmi = m.cpu.nmi.bind(m.cpu);
    m.cpu.nmi = () => { const pc = m.cpu.pc; if (!((pc >= 0xc003 && pc <= 0xc01a) || (pc >= 0xc01d && pc <= 0xc027) || (pc >= 0xc098 && pc <= 0xc09f))) inc(); nmi(); };
    for (const at of [0xe91d, 0xc067]) m.hooks.set(at, mm => { if (first < 0) snaps.push(Uint8Array.from(mm.ram)); });
    m.hooks.set(0xe757, mm => course[k].push(`stage ${mm.ram[0x41] + 1} round ${mm.ram[0x4e]}`)); // $E757: a stage loads
  };
  hook(a, snapA, () => lagA++, 0); hook(b, snapB, () => lagB++, 1);
  a.reset(); b.reset();
  for (let f = 1; f <= aloneFrames; f++) {
    for (const m of [a, b]) { m.pads[0] = game(f, m); m.pads[1] = 0; if (m.ram[0x7b] < 2) m.ram[0x7b] = 2; m.ram[0x49] = 1; m.runFrame(); }
    while (first < 0 && snapA.length && snapB.length) {
      const x = snapA.shift(), y = snapB.shift();
      for (let i = 0; i < 0x800; i++) if (!skip(i) && (i < 0xac || i > 0xaf) && x[i] !== y[i]) { first = compared; console.log(`alone: equal for ${compared} points to frame ${a.frame} (RAM $${i.toString(16)} ${x[i]} / ${y[i]}: the task order)`); break; }
      if (first < 0) compared++;
    }
  }
  const same = course[0].join() === course[1].join();
  console.log(`alone: ${aloneFrames} frames; the original ${course[0].join(', ')}; the patched ROM ${same ? 'the same' : course[1].join(', ')}; lag NMIs ${lagA} / ${lagB}`);
  if (!same) failed = true;
}

// --- the attract mode: nobody pressing anything, the same sprites as the original through the story (its figures'
// frames are in bank 1) until the demonstration's stage loads ($E75D, without the poster; from there its tasks run in another order, as above)
{
  const a = machine(original.prg), b = machine(patchedPrg());
  a.reset(); b.reset();
  let first = -1, demo = -1;
  for (const m of [a, b]) m.hooks.set(0xe75d, mm => { if (demo < 0) demo = mm.frame; });
  for (let f = 1; f <= 5000 && first < 0 && demo < 0; f++) {
    a.runFrame(); b.runFrame();
    for (let i = 0; i < 256; i++) if (a.oam[i] !== b.oam[i]) { first = f; break; }
  }
  const ok = first < 0 && demo > 1500;
  console.log(`attract: sprites equal to the original for ${demo} frames, to the demonstration's stage${first > 0 ? ' (SPRITES APART at ' + first + ')' : ''}${ok ? '' : '  FAIL'}`);
  if (!ok) failed = true;
}

// --- the title
{
  const m = machine(patchedPrg());
  m.reset();
  const text = (address, n) => [...Array(n).keys()].map(k => m.ppuRead(address + k)).map(c => c >= 0xd0 && c <= 0xd9 ? String(c - 0xd0) :
    c >= 0xda && c <= 0xf3 ? String.fromCharCode(65 + c - 0xda) : c === 0xfc ? '>' : ' ').join('');
  for (let f = 1; f <= 120; f++) { m.pads[0] = 0; m.runFrame(); }
  const before = [text(0x21c9, 11), text(0x2209, 11)];
  for (let f = 1; f <= 10; f++) { m.pads[0] = f <= 2 ? 0x04 : 0; m.runFrame(); }
  const after = [text(0x21c9, 11), text(0x2209, 11)];
  const [l1, l2] = variant.lines;
  const ok = before[0] === '> ' + l1.padEnd(9) && before[1] === '  ' + l2 && after[0] === '  ' + l1.padEnd(9) && after[1] === '> ' + l2 &&
    text(0x21eb, 10).trim() === '';
  console.log(`title: "${before.join('" "')}", after SELECT "${after.join('" "')}"${ok ? '' : '  FAIL'}`);
  if (!ok) failed = true;
}

// --- the horse: player 2 gets the horse as its item does ($76 = 2: $FBD9 brings it to slot 1), then player 1 is hit
// while she rides (he dies where he stands and comes back): she keeps riding and the horse object stays all along.
{
  const m = machine(patchedPrg());
  m.reset();
  let mounted = -1, hit = -1, died = -1, back = -1, riding = 0, lost = 0, after = 0;
  m.hooks.set(symbols.get('die_saved') & 0xffff, mm => { if (mm.cart.bank === 4 && hit > 0 && died < 0) died = mm.frame; });
  const p2rides = () => (m.ram[0xdf] ? m.ram[0x77] : m.ram[0x607]) !== 0;
  for (let f = 1; f <= 6000; f++) {
    m.pads[0] = f < 100 ? (f >= 80 && f < 82 ? 0x04 : f >= 90 && f < 94 ? 0x08 : 0) : 0;
    m.pads[1] = 0;
    m.ram[0x49] = 1;
    if (m.ram[0xd6] && !m.ram[0xd7]) {
      if (mounted < 0 && f > 1500) { m.ram[0x606] = 2; mounted = f; }
      if (mounted > 0 && hit < 0 && f > mounted + 300 && m.ram[0x607] === 3 && !m.ram[0xdf]) { m.ram[0x7b] = 0; m.ram[0x76] = 1; hit = f; }
    }
    if (hit < 0 && m.ram[0x7b] < 2 && !m.ram[0xdf]) m.ram[0x7b] = 2; // player 1 kept from harm until the planned hit
    if (m.ram[0x60b] < 2 && m.ram[0xd6] && hit < 0) m.ram[0x60b] = 2;
    m.runFrame();
    if (died > 0 && back < 0 && m.ram[0xd6] && m.ram[0x642] === 0) back = f;
    if (mounted > 0 && p2rides()) { riding++; if (!(m.ram[0x401] & 0x80)) lost++; if (back > 0) after++; }
  }
  const ok = mounted > 0 && hit > 0 && died > 0 && back > died + 60 && riding > 300 && after > 100 && lost === 0;
  console.log(`horse: player 2 given the horse at ${mounted}, player 1 hit at ${hit}, died where he stood at ${died}, back at ${back}; she rode ${riding} frames (${after} after his return), the horse object missing ${lost}${ok ? '' : '  FAIL'}`);
  if (!ok) failed = true;
}

// --- a life from the other player: the RAM's player dies where he stands with his last life (out; the players
// exchange places), types A B B A START on his own pad: the other (3 lives) gives one, he comes back at line $BC; his
// START does not pause. Then he is out again while the other is on her last life: the code does nothing.
{
  const m = machine(patchedPrg());
  m.reset();
  const ev = { out: -1, typed: -1, back: -1, line: -1, livesAfter: '', pauses: 0, out2: -1, refused: '' };
  m.hooks.set(0xf2ec, () => { ev.pauses++; });
  const outNow = () => !m.ram[0xd6] && !m.ram[0x642] && m.ram[0x60a] === 0;
  const code = [0x01, 0x02, 0x02, 0x01, 0x08]; // A B B A START (the machine's pad bits)
  let typing = -1, pad = 0, phase = 0, shield = true;
  for (let f = 1; f <= 5000; f++) {
    m.pads[0] = f < 100 ? (f >= 80 && f < 82 ? 0x04 : f >= 90 && f < 94 ? 0x08 : 0) : 0;
    m.pads[1] = 0;
    m.ram[0x49] = 1;
    if (shield && m.ram[0x7b] < 2) m.ram[0x7b] = 2;
    if (phase < 4 && phase !== 1 && m.ram[0x60b] < 2) m.ram[0x60b] = 2; // the saved one too, until his planned hit
    const inPlay = m.ram[0xd6] && !m.ram[0xd7];
    // the RAM's player (player 1 or 2 by $DF) hit with his last life
    if (phase === 0 && f > 1500 && inPlay) { pad = m.ram[0xdf] ? 1 : 0; m.ram[0x7a] = 1; m.ram[0x7b] = 0; m.ram[0x76] = 1; phase = 1; }
    if (phase === 1 && outNow()) { ev.out = f; typing = f + 30; phase = 2; }
    // the saved one (him again) hit with his one life while the other is down to her last
    if (phase === 3 && f > ev.back + 200 && inPlay) { pad = m.ram[0xdf] ? 0 : 1; m.ram[0x7a] = 1; m.ram[0x60a] = 1; m.ram[0x60b] = 0; m.ram[0x606] = 1; phase = 4; }
    if (phase === 4 && outNow()) { ev.out2 = f; typing = f + 30; phase = 5; }
    if (typing > 0 && f >= typing) { const k = (f - typing) / 6 | 0; if (k < code.length) m.pads[pad] = (f - typing) % 6 < 3 ? code[k] : 0; else { typing = -1; if (phase === 2) ev.typed = f; } }
    if (phase === 5 && typing < 0 && f > ev.out2 + 120) { ev.refused = `${m.ram[0x60a]}/${m.ram[0x7a]} in play ${m.ram[0xd6]}`; break; }
    m.runFrame();
    if (phase === 2 && ev.typed > 0 && m.ram[0xd6] && ev.back < 0) { ev.back = f; ev.livesAfter = `${m.ram[0x60a]}/${m.ram[0x7a]}`; }
    if (ev.back > 0 && ev.line < 0 && m.ram[0x601] === 0xbc) ev.line = f;
    if (phase === 2 && ev.back > 0 && f > ev.back + 5) phase = 3;
    shield = phase !== 1 || outNow(); // the RAM's player kept from harm but for the planned hit
  }
  const ok = ev.out > 0 && ev.back > 0 && ev.livesAfter === '1/2' && ev.line > 0 && ev.pauses === 0 && ev.refused === '0/1 in play 0';
  console.log(`a life from the other: out at ${ev.out}, the code typed by ${ev.typed}, back at ${ev.back} (lives ${ev.livesAfter}, line $BC at ${ev.line}), ` +
    `pauses ${ev.pauses}; out again with her last life: ${ev.refused}${ok ? '' : '  FAIL'}`);
  if (!ok) failed = true;
}

// --- two players
const sym = name => symbols.get(name) & 0xffff;
const two = (options, label, frames) => {
  const m = machine(patchedPrg(options));
  m.reset();
  const pal = options[1] ?? 2;
  const s = { entered: -1, moves: 0, shots: 0, stage: 0, lastX: -1, bossAims: 0, shares: 0, inPlace: [0, 0], original: [0, 0],
    fadeWhileOther: 0, comeBacks: 0, livesApart: 0, actWithoutLives: 0, badLives: 0, riding: 0, horseLost: 0, hud: 0, coloured: 0,
    exchanges: 0, gameOver: -1, gameOverLives: '', herDeathFrames: 0, blueWhileHer: 0, stallBreaks: 0 };
  let waiting = 0;
  // RAM: $D6 the other in play, $D7 busy, $DA her block swapped in for a pass, $DF player 2 in RAM, $D9 the mode,
  // $0642 dying.
  const p2here = mm => (mm.ram[0xda] ^ mm.ram[0xdf]) & 1;
  for (const [name, key] of [['rs_swap', 'bossAims'], ['share', 'shares'], ['exchange', 'exchanges']])
    m.hooks.set(sym(name), mm => { if (mm.cart.bank === 4) s[key]++; });
  // A death where the player stands (die_saved: the dying one is the saved player then; player 1 or 2 by P2RAM).
  m.hooks.set(sym('die_saved'), mm => { if (mm.cart.bank === 4) s.inPlace[mm.ram[0xdf] ? 0 : 1]++; });
  m.hooks.set(sym('hl_d'), mm => { if (mm.cart.bank === 4 && mm.ram[0x642] === 1 && mm.ram[0x60a]) s.comeBacks++; });
  let herDeath = false;
  m.hooks.set(0xf31c, mm => { // the original's death of the RAM's player: only when the other is not in play
    s.original[p2here(mm)]++;
    if (mm.ram[0xd6]) s.fadeWhileOther++;
  });
  m.hooks.set(0xf339, mm => { herDeath = p2here(mm) === 1; });
  m.hooks.set(0xf3f1, () => { herDeath = false; });
  m.hooks.set(0xf3f4, mm => { herDeath = false; if (s.gameOver < 0) { s.gameOver = mm.frame; s.gameOverLives = `${mm.ram[0x7a]}/${mm.ram[0x60a]}`; } });
  for (let f = 1; f <= frames && s.gameOver < 0; f++) {
    // the title: SELECT (2 PLAYERS), then START; then the patterns
    m.pads[0] = f < 100 ? (f >= 80 && f < 82 ? 0x04 : f >= 90 && f < 94 ? 0x08 : 0) : game(f, m);
    m.pads[1] = f < 1010 ? 0 : ((f >> 6) & 1 ? 0x80 : 0x40) | ((f >> 8) & 1 ? 0x10 : 0x20) | ((f >> 2) & 1 ? 0 : 0x02) | (((f >> 3) & 3) === 2 ? 0x01 : 0);
    if (m.ram[0xaa]) m.pads[1] = (f & 31) >= 16 && (f & 31) < 18 ? 0x04 : 0; // in a window SELECT alone, two frames (a shop closes on it)
    if (f < frames / 3) { if (m.ram[0x7b] < 2) m.ram[0x7b] = 2; if (m.ram[0x60b] < 2 && m.ram[0xd6]) m.ram[0x60b] = 2; }
    m.ram[0x49] = 1;
    // The test's stall breaker: a boss fight waiting 1500 frames for enemies the patterns cannot hit (no boss in slot
    // $0E yet) gets them removed; counted (it happens with the original's code alone too, from the same place).
    waiting = m.ram[0x4b] === 1 && !(m.ram[0x40e] & 0x80) ? waiting + 1 : 0;
    if (waiting > 1500) { for (let k = 0x10; k <= 0x16; k++) m.ram[0x400 + k] = 0; waiting = 0; s.stallBreaks++; }
    m.runFrame();
    const act = m.ram[0xd6], busy = m.ram[0xd7], p2ram = m.ram[0xdf];
    if (m.ram[0xd9] !== 1) { if (f > 200) s.modeLost = s.modeLost ?? f; continue; }
    if (act && s.entered < 0) s.entered = f;
    if (m.ram[0x41] > s.stage) s.stage = m.ram[0x41];
    const x2 = p2here(m) ? m.ram[0x74] : m.ram[0x604];
    if (s.entered >= 0 && x2 !== s.lastX) { s.moves++; s.lastX = x2; }
    if ((p2here(m) ? m.ram[0x84] : m.ram[0x614]) && s.entered >= 0) s.shots++;
    const lives = [m.ram[0x7a], m.ram[0x60a]];
    if (lives.some(v => v > 9)) s.badLives++;
    if (lives[0] !== lives[1]) s.livesApart++;
    if (act && lives[1] === 0) s.actWithoutLives++;
    // A rider has the horse (object 1); the horse lost while still ridden was the bug.
    for (const [ride, inPlay] of [[m.ram[0x77], !busy], [m.ram[0x607], act && !busy]]) if (inPlay && ride) {
      s.riding++;
      if (!(m.ram[0x401] & 0x80)) s.horseLost++;
    }
    if (!busy && s.entered >= 0) {
      // the lives: the hats (tile 0) at X $10 and $D4 on line $D0
      let n = 0; for (let o = 0; o < 256; o += 4) if (m.oam[o] === 0xd0 && m.oam[o + 1] === 0 && (m.oam[o + 3] === 0x10 || m.oam[o + 3] === 0xd4)) n++;
      if (n === 2) s.hud++;
      // player 2's first sprite in her palette: hers at $0200 after her pass, at $0218 when she is the RAM's player and
      // the other passed, or at $0200 when she plays alone
      const at = act ? (p2ram ? 0x218 : 0x200) : (p2ram ? 0x200 : -1);
      if (at >= 0 && (m.ram[at + 2] & 3) === pal) s.coloured++;
    }
    if (herDeath) {
      s.herDeathFrames++;
      for (let o = 4; o < 256; o += 4) if (m.oam[o] < 0xef && m.oam[o + 1] < 0x36 && (m.oam[o + 2] & 3) === 0) { s.blueWhileHer++; break; }
    }
  }
  s.score = [0, 2, 4, 6, 8].map(i => m.ram[0x6f2 + i] - 0x58).join('');
  s.ram = Buffer.from(m.ram).toString('hex');
  console.log(`two players ${label}: entered at ${s.entered}, player 2 moved ${s.moves}, her shot pause ${s.shots} frames, stage ${s.stage + 1}, ` +
    `deaths in place ${s.inPlace.join(' / ')}, the original's ${s.original.join(' / ')} (${s.fadeWhileOther} with the other in play), ` +
    `back ${s.comeBacks}, exchanges ${s.exchanges}, lives apart ${s.livesApart} frames, in play without lives ${s.actWithoutLives}, ` +
    `riding ${s.riding} (horse lost ${s.horseLost}), lives shown ${s.hud}, her palette ${s.coloured}, her death ${s.herDeathFrames} (${s.blueWhileHer} in palette 0), ` +
    `bosses aimed at her ${s.bossAims}, shops shared ${s.shares}, score ${s.score}, game over at ${s.gameOver} (lives ${s.gameOverLives}), stall breaks ${s.stallBreaks}` +
    `${s.modeLost ? ', MODE LOST at ' + s.modeLost : ''}${s.badLives ? ', LIVES BROKEN' : ''}`);
  return s;
};
for (const [options, label] of [[[0, 2, 0, 0x16, 0x36, 0x0f], 'default'], [[1, 2, 0, 0x16, 0x36, 0x0f], 'friendly fire'],
  [[0, 1, 1, 0x16, 0x38, 0x0f], 'own colours (palette 1)']]) {
  const r1 = two(options, label, twoFrames), r2 = two(options, label + ' (again)', twoFrames);
  const ok = r1.entered > 0 && r1.entered < 1000 && r1.moves > 100 && r1.shots > 100 && r1.ram === r2.ram &&
    r1.inPlace[0] + r1.inPlace[1] > 0 && r1.comeBacks > 0 && r1.exchanges > 0 && r1.fadeWhileOther === 0 && r1.livesApart > 0 &&
    r1.actWithoutLives === 0 && !r1.badLives && !r1.modeLost && r1.horseLost === 0 && r1.hud > 1000 && r1.coloured > 1000 &&
    r1.blueWhileHer === 0 && r1.gameOver > 0 && r1.gameOverLives === '0/0';
  if (!ok) { failed = true; console.log('  FAIL'); }
}
console.log(`objects drawn with bank 4 mapped: ${objectsInBank4}${objectsInBank4 ? '  FAIL' : ''}`);
if (objectsInBank4) failed = true;
console.log(failed ? 'FAIL' : 'PASS');
process.exitCode = failed ? 1 : 0;
