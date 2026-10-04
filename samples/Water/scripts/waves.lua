-- The sea's wave table, shared by the water shader graph (through ocean.lua) and by float.lua.
-- Four sine waves: height = sum(amp * sin(dirX * x + dirZ * z + speed * t)). Keep the table in
-- sync with WAVES in tools/make_scene.py, which writes the graph's default uniforms.
local Waves = {}

local WAVES = { -- direction (degrees in the XZ plane), wavelength, amplitude at sea state 1
  { 20, 7.0, 0.16 }, { -35, 4.3, 0.09 }, { 75, 2.3, 0.04 }, { -110, 1.4, 0.02 },
}
local SPEED = 0.6 -- of the deep-water speed sqrt(g * k)

Waves.dirX, Waves.dirZ, Waves.speed, Waves.amp = {}, {}, {}, {}
for i, wave in ipairs(WAVES) do
  local k = 2 * math.pi / wave[2]
  Waves.dirX[i] = k * math.cos(math.rad(wave[1]))
  Waves.dirZ[i] = k * math.sin(math.rad(wave[1]))
  Waves.speed[i] = -SPEED * math.sqrt(9.8 * k) -- negative: the wave travels along its direction
  Waves.amp[i] = wave[3]
end

Waves.scale = 1 -- sea state: multiplies every amplitude (ocean.lua eases it between presets)

-- Graph uniform `amp` for the current sea state.
function Waves.amplitudes()
  local out = {}
  for i = 1, 4 do out[i] = Waves.amp[i] * Waves.scale end
  return out
end

-- Surface height and slopes (d height / dx, d height / dz) at a world position and time.
function Waves.sample(x, z, t)
  local height, slopeX, slopeZ = 0, 0, 0
  for i = 1, 4 do
    local phase = Waves.dirX[i] * x + Waves.dirZ[i] * z + Waves.speed[i] * t
    local amp = Waves.amp[i] * Waves.scale
    height = height + amp * math.sin(phase)
    local c = amp * math.cos(phase)
    slopeX = slopeX + c * Waves.dirX[i]
    slopeZ = slopeZ + c * Waves.dirZ[i]
  end
  return height, slopeX, slopeZ
end

return Waves
