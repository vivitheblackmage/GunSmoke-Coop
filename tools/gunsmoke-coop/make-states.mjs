import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawnSync } from 'node:child_process';
// The two-player patch's debug states for Mesen 2 (tools/gunsmoke-coop/mesen-states.lua): slots 1-6 = stages 1-6 with
// 2 PLAYERS, slot 7 = stage 1's boss; F1-F7 load them in Mesen (its default keys). Mesen names a slot file after the ROM
// file (<name>_<slot>.mss in its SaveStates folder), so the states belong to that patched ROM file; patch again with
// other options and make them again. Slot files already there are kept as <file>.bak (one generation).
// The script prints the states (Mesen keeps scripts from files by default); this writes them.
// Usage: node tools/gunsmoke-coop/make-states.mjs <patched rom> [Mesen.exe] [SaveStates folder]
//   Mesen.exe: default Mesen.exe in the working directory
//   the folder: default Mesen's own (SaveStates next to Mesen.exe when it is portable, else Documents/Mesen2/SaveStates)
const [romArg, mesenArg, dirArg] = process.argv.slice(2);
if (!romArg) { console.log('Usage: node tools/gunsmoke-coop/make-states.mjs <patched rom> [Mesen.exe] [SaveStates folder]'); process.exit(1); }
const rom = path.resolve(romArg), mesen = path.resolve(mesenArg ?? 'Mesen.exe');
const portable = fs.existsSync(path.join(path.dirname(mesen), 'settings.json'));
const dir = path.resolve(dirArg ?? (portable ? path.join(path.dirname(mesen), 'SaveStates') : path.join(os.homedir(), 'Documents', 'Mesen2', 'SaveStates')));
const name = path.basename(rom, path.extname(rom));
const run = spawnSync(mesen, ['--testrunner', rom, path.resolve('tools/gunsmoke-coop/mesen-states.lua')],
  { encoding: 'latin1', timeout: 600_000, maxBuffer: 64 << 20 });
const states = [...(run.stdout ?? '').matchAll(/^GSSTATE (\d+) (\d+) ([0-9a-f]+)\s*$/gm)].map(m => ({ slot: +m[1], stage: +m[2], data: Buffer.from(m[3], 'hex') }));
if (states.length !== 7 || !/^GSDONE/m.test(run.stdout ?? '')) {
  console.log(`Mesen made ${states.length} of 7 states (exit ${run.status})${run.error ? ': ' + run.error.message : ''}`);
  process.exit(1);
}
fs.mkdirSync(dir, { recursive: true });
for (const s of states) {
  const file = path.join(dir, `${name}_${s.slot}.mss`);
  if (fs.existsSync(file)) fs.renameSync(file, file + '.bak');
  fs.writeFileSync(file, s.data);
  console.log(`F${s.slot}: stage ${s.stage}${s.slot === 7 ? ', its boss' : ''} (${s.data.length} bytes)`);
}
console.log(`7 states for "${name}" in ${dir}`);
