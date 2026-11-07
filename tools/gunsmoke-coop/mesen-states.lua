-- The two-player patch's debug states (docs/HACK.md, "Debug states"): Mesen 2 save states at each
-- stage's start with 2 PLAYERS, so a stage can be tried without playing the ones before. Run in Mesen's test runner
-- (tools/gunsmoke-coop/make-states.mjs, or the patcher's MESEN DEBUG STATES); needs no file access (Mesen's default
-- keeps scripts from I/O): each state is printed as a line "GSSTATE <slot> <stage> <hex>" and the caller writes Mesen's
-- slot files (<ROM name>_<slot>.mss), so F1-F7 (Mesen's keys for the slots) load them.
--   slots 1-6: stages 1-6 as the game enters them (the poster, then play); both players, three lives each
--   slot 7:    stage 1's boss (the stage's map at its end), both players in play
-- How: the title's 2 PLAYERS chosen (SELECT, START) once; that moment kept (a state); for each slot the state back,
-- and at the stage load ($E757, which reads the stage from $41) $41 set to the slot's stage, as if the stages before
-- were done. Nothing else is changed: no upgrades, round 1, the score 0.
local function r(a) return emu.read(a, emu.memType.nesMemory) end
local function w(a, v) emu.write(a, v, emu.memType.nesMemory) end

local frame, title, slot, stage, loaded, played = 0, nil, 0, 0, false, 0
local pending = nil -- an action for the next exec callback (states are made and loaded only there)
local function hex(data) return (data:gsub(".", function(c) return string.format("%02x", c:byte()) end)) end
local function save(s)
  print(string.format("GSSTATE %d %d %s", s, r(0x41) + 1, hex(emu.createSavestate())))
end
local function nextSlot()
  slot = slot + 1
  if slot > 7 then print("GSDONE"); emu.stop(0); return end
  stage = slot <= 6 and slot - 1 or 0
  loaded, played = false, 0
  pending = function() emu.loadSavestate(title) end
end

-- The pads: SELECT then START on the title (pad 1), nothing after.
emu.addEventCallback(function()
  local p = {}
  if title == nil then
    if frame >= 80 and frame < 82 then p.select = true elseif frame >= 90 and frame < 94 then p.start = true end
  end
  emu.setInput(p, 0)
end, emu.eventType.inputPolled)

emu.addEventCallback(function()
  frame = frame + 1
  w(0x49, 1) -- the poster held (as the tests): the boss comes when the stage's map ends
  if loaded and r(0xD6) ~= 0 and r(0xD7) == 0 then played = played + 1 end
  if frame > 60000 then print("GSFAIL slot " .. slot); emu.stop(1) end
end, emu.eventType.endFrame)

-- The title's START taken ($B0B2 after the hook): keep the moment once.
emu.addMemoryCallback(function()
  if title == nil and r(0xD9) == 1 then title = emu.createSavestate(); nextSlot() end
end, emu.callbackType.exec, 0xB0B2)

-- A stage loads: the slot's stage instead.
emu.addMemoryCallback(function()
  if slot >= 1 and not loaded then w(0x41, stage); loaded = true end
end, emu.callbackType.exec, 0xE757)

-- The play loop's start: a state when both are in play (slot 7: at the boss, after the map's end).
emu.addMemoryCallback(function()
  if pending then local p = pending; pending = nil; p(); return end
  if not loaded or r(0xD6) == 0 or r(0xD7) ~= 0 then return end
  if slot <= 6 and played >= 2 then save(slot); nextSlot()
  elseif slot == 7 then
    if r(0x4B) == 1 and played >= 2 then save(slot); nextSlot()
    elseif r(0x4B) ~= 1 then w(0x7B, 2); w(0x060B, 2) end -- kept from harm on the way (blinking)
  end
end, emu.callbackType.exec, 0xE91D)

-- Before the title is kept: the scheduler's idle loop for a pending load.
emu.addMemoryCallback(function()
  if pending then local p = pending; pending = nil; p() end
end, emu.callbackType.exec, 0xC003)
