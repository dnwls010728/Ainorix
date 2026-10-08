-- Time of day: keyframes of light, sky and camera-effect values, blended with a smoothstep.
-- t: 0 dawn, 0.25 midday, 0.5 golden hour, 0.68 dusk, 0.85 night, 1 dawn again.
local Atmosphere = {}

local function hex(s)
  return {tonumber(s:sub(2, 3), 16) / 255, tonumber(s:sub(4, 5), 16) / 255, tonumber(s:sub(6, 7), 16) / 255}
end

local KEYS = {
  {t = 0, sunColor = "#ffb48a", sunI = 2.5, sunDir = {-28, 8, 10}, hemiSky = "#b4a0d8", hemiGround = "#6a5060", hemiI = 0.85,
   haze = "#f0c0b0", zenith = "#5e6cb0", exposure = 1.2, shadowTint = {0.88, 0.9, 1.14}, highTint = {1.12, 1.0, 0.88},
   sat = 1.14, vignette = 0.5, bloom = 0.6, lamp = 0.55, stars = 0.12, shaft = 1.0, cloudLit = "#ffd8c0", cloudShade = "#a89cc0",
   fireflies = 0.2},
  {t = 0.25, sunColor = "#fff4e0", sunI = 3.7, sunDir = {-14, 27, 22}, hemiSky = "#9cc8ff", hemiGround = "#8a7a58", hemiI = 1.2,
   haze = "#cde4f2", zenith = "#3f82dc", exposure = 1.05, shadowTint = {0.92, 0.97, 1.08}, highTint = {1.04, 1.02, 0.96},
   sat = 1.14, vignette = 0.42, bloom = 0.42, lamp = 0, stars = 0, shaft = 0.55, cloudLit = "#ffffff", cloudShade = "#a8b8d8",
   fireflies = 0},
  {t = 0.5, sunColor = "#ffd49a", sunI = 3.4, sunDir = {-24, 20, 12}, hemiSky = "#a8c8ff", hemiGround = "#8a6a48", hemiI = 1.05,
   haze = "#f0dcc0", zenith = "#5f90d6", exposure = 1.15, shadowTint = {0.86, 0.92, 1.14}, highTint = {1.1, 1.02, 0.84},
   sat = 1.18, vignette = 0.5, bloom = 0.55, lamp = 0.3, stars = 0, shaft = 1.0, cloudLit = "#fff0d8", cloudShade = "#b0a0b8",
   fireflies = 0.1},
  {t = 0.68, sunColor = "#ff8a60", sunI = 2.1, sunDir = {-30, 6, 6}, hemiSky = "#8a78c8", hemiGround = "#5a4058", hemiI = 0.9,
   haze = "#d8a0a8", zenith = "#34347e", exposure = 1.25, shadowTint = {0.8, 0.86, 1.2}, highTint = {1.16, 0.98, 0.84},
   sat = 1.2, vignette = 0.56, bloom = 0.68, lamp = 0.9, stars = 0.25, shaft = 0.9, cloudLit = "#ffb090", cloudShade = "#6a5488",
   fireflies = 0.55},
  {t = 0.85, sunColor = "#8aa8ff", sunI = 1.15, sunDir = {22, 26, -10}, hemiSky = "#3c4c90", hemiGround = "#1c2240", hemiI = 0.5,
   haze = "#1e2648", zenith = "#070b22", exposure = 1.32, shadowTint = {0.8, 0.92, 1.3}, highTint = {0.96, 1.02, 1.16},
   sat = 1.08, vignette = 0.62, bloom = 0.78, lamp = 1, stars = 1, shaft = 0.25, cloudLit = "#6878b0", cloudShade = "#252c58",
   fireflies = 1},
}

local COLORS = {"sunColor", "hemiSky", "hemiGround", "haze", "zenith", "cloudLit", "cloudShade"}
local TRIPLES = {"shadowTint", "highTint"}
local NUMBERS = {"sunI", "hemiI", "exposure", "sat", "vignette", "bloom", "lamp", "stars", "shaft", "fireflies"}

for _, k in ipairs(KEYS) do
  for _, name in ipairs(COLORS) do k[name] = hex(k[name]) end
  local d = k.sunDir
  local len = math.sqrt(d[1] * d[1] + d[2] * d[2] + d[3] * d[3])
  k.sunDir = {d[1] / len, d[2] / len, d[3] / len}
end
do
  local first = {}
  for name, value in pairs(KEYS[1]) do first[name] = value end
  first.t = 1
  KEYS[#KEYS + 1] = first
end

local function mix3(a, b, u)
  return {a[1] + (b[1] - a[1]) * u, a[2] + (b[2] - a[2]) * u, a[3] + (b[3] - a[3]) * u}
end

-- Values at time of day `tod` (any number; wraps every 1).
function Atmosphere.sample(tod)
  local t = tod % 1
  local i = 1
  while i < #KEYS - 1 and t >= KEYS[i + 1].t do i = i + 1 end
  local a, b = KEYS[i], KEYS[i + 1]
  local u = math.min(1, math.max(0, (t - a.t) / (b.t - a.t)))
  u = u * u * (3 - 2 * u)
  local out = {}
  for _, name in ipairs(COLORS) do out[name] = mix3(a[name], b[name], u) end
  for _, name in ipairs(TRIPLES) do out[name] = mix3(a[name], b[name], u) end
  for _, name in ipairs(NUMBERS) do out[name] = a[name] + (b[name] - a[name]) * u end
  local d = mix3(a.sunDir, b.sunDir, u)
  local len = math.sqrt(d[1] * d[1] + d[2] * d[2] + d[3] * d[3])
  out.sunDir = {d[1] / len, d[2] / len, d[3] / len}
  return out
end

function Atmosphere.label(tod)
  local x = tod % 1
  if x < 0.12 or x >= 0.94 then return "새벽" end
  if x < 0.4 then return "낮" end
  if x < 0.6 then return "오후" end
  if x < 0.76 then return "노을" end
  return "밤"
end

return Atmosphere
