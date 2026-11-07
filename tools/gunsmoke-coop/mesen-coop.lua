-- The two-player patch (tools/gunsmoke-coop) in Mesen 2: 2 PLAYERS chosen on the title (SELECT, START), two pads on
-- fixed patterns (player 1 as the oracles' "boss" pattern, player 2 the mirror image from frame 1010), the wanted
-- poster held, both kept from harm until GS_SHIELD; a line of state every 60 frames to GS_OUT and pictures (PPM) at the frames in GS_SHOTS.
--   GS_OUT=<file> GS_FRAMES=<n> GS_SHIELD=<n> GS_SHOTS=<f,f,..> Mesen.exe --testrunner <patched rom> tools/gunsmoke-coop/mesen-coop.lua
local out = io.open(os.getenv("GS_OUT") or "mesen-coop.txt", "w")
local frames = tonumber(os.getenv("GS_FRAMES") or "6000")
local shield = tonumber(os.getenv("GS_SHIELD") or "3000")
local shots = {}
for f in string.gmatch(os.getenv("GS_SHOTS") or "", "%d+") do shots[tonumber(f)] = true end
local shotDir = os.getenv("GS_SHOT_DIR") or "."
local frame = 0
local function r(a) return emu.read(a, emu.memType.nesMemory) end
local function w(a, v) emu.write(a, v, emu.memType.nesMemory) end

-- Pad 2 is played through $4017 here (Mesen's Lua input reaches pad 1 only): a shift register latched by the
-- strobe, bits A B Select Start Up Down Left Right from bit 0.
local pad2, shift2 = 0, 0
emu.addMemoryCallback(function(address, value) if value & 1 == 1 then shift2 = pad2 end end, emu.callbackType.write, 0x4016, 0x4016)
emu.addMemoryCallback(function(address, value)
  local bit = shift2 & 1
  shift2 = (shift2 >> 1) | 0x80
  return 0x40 | bit
end, emu.callbackType.read, 0x4017, 0x4017)

emu.addEventCallback(function()
  local f = frame
  local p1 = {}
  if f >= 80 and f < 82 then p1.select = true
  elseif f >= 90 and f < 94 then p1.start = true
  elseif f >= 830 then
    if (f >> 6) & 1 == 1 then p1.left = true else p1.right = true end
    if (f >> 7) & 1 == 1 then p1.up = true end
    if (f >> 2) & 1 == 1 then p1.a = true end
    if (f >> 3) & 3 == 1 then p1.b = true end
  end
  if r(0xAA) ~= 0 and (f & 31) == 0 then p1.select = true end
  emu.setInput(p1, 0)
  local p2 = 0
  if f >= 1010 then
    p2 = ((f >> 6) & 1 == 1 and 0x80 or 0x40) | ((f >> 8) & 1 == 1 and 0x10 or 0x20) | ((f >> 2) & 1 == 0 and 0x02 or 0) | ((f >> 3) & 3 == 2 and 0x01 or 0)
  end
  if r(0xAA) ~= 0 then p2 = ((f & 31) == 16) and 0x04 or 0 end -- in her windows SELECT alone (a shop closes on it)
  pad2 = p2
end, emu.eventType.inputPolled)

emu.addEventCallback(function()
  frame = frame + 1
  w(0x49, 1)
  if frame < shield then
    if r(0x7B) < 2 then w(0x7B, 2) end
    if r(0xD6) ~= 0 and r(0x060B) < 2 then w(0x060B, 2) end
  end
  if frame % 60 == 0 then
    out:write(string.format("%d stage %d mode %d act %d p2ram %d dying %d busy %d | ram %d,%d lives %d | saved %d,%d lives %d | score %d%d%d%d%d | lag-free %d\n",
      frame, r(0x41) + 1, r(0xD9) + 1, r(0xD6), r(0xDF), r(0x0642), r(0xD7), r(0x71), r(0x74), r(0x7A), r(0x0601), r(0x0604), r(0x060A),
      r(0x06F2) - 0x58, r(0x06F4) - 0x58, r(0x06F6) - 0x58, r(0x06F8) - 0x58, r(0x06FA) - 0x58, r(0xA0)))
  end
  if shots[frame] then
    local screen = emu.getScreenBuffer()
    local height = #screen // 256
    local f = io.open(string.format("%s/coop-%05d.ppm", shotDir, frame), "wb")
    local rgb = {}
    for i = 1, #screen do local p = screen[i]; rgb[#rgb + 1] = string.char((p >> 16) & 255, (p >> 8) & 255, p & 255) end
    f:write(string.format("P6 256 %d 255\n", height) .. table.concat(rgb))
    f:close()
  end
  if frame >= frames then out:close(); emu.stop(0) end
end, emu.eventType.endFrame)
