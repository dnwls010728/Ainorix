-- Balance tables: world/player constants, enemy stats, spawn curves, weapon stats.
-- Distances and speeds are world units (meters for the physics): the design numbers of
-- the original, which were pixels, times U.
local B = {}

local U = 1 / 40
B.U = U

B.WORLD = 2400 * U -- the square arena, centred on the origin
B.BASE_HP = 100
B.BASE_SPEED = 165 * U
B.BASE_PICKUP = 70 * U
B.BASE_LIGHT = 235 * U
B.BASE_OIL = 100
B.OIL_DRAIN = 1.5 -- per second
B.DARK_SPEED = 1.35
B.DARK_DAMAGE = 1.25
B.BRAZIER_COUNT = 6
B.BRAZIER_RADIUS = 90 * U -- stand inside to ignite
B.BRAZIER_LIGHT = 250 * U
B.BRAZIER_TIME = 3
B.SPAWN_DIST_MIN = 560 * U
B.SPAWN_DIST_MAX = 680 * U

local floor, min, max = math.floor, math.min, math.max

function B.xpForLevel(level)
  return floor(3 + 3.4 * level + 0.55 * level * level)
end

-- ---------------------------------------------------------------- enemies

local function E(hp, speed, dmg, r, xp, eye, extra)
  local e = { hp = hp, speed = speed * U, dmg = dmg, r = r * U, xp = xp, eye = eye }
  for k, v in pairs(extra or {}) do e[k] = v end
  return e
end

B.ENEMIES = {
  shade = E(8, 60, 8, 13, 1, "#ffffff"),
  skitter = E(4, 104, 5, 9, 1, "#c9fbff"),
  brute = E(62, 42, 16, 22, 6, "#ffc4ec"),
  wisp = E(16, 56, 8, 12, 3, "#ffffff"),
  gloom = E(32, 50, 10, 17, 3, "#fff0a8"),
  gloomlet = E(9, 82, 6, 10, 1, "#fff0a8"),
  stalker = E(22, 68, 12, 13, 3, "#ffb0c0"),
  leech = E(18, 76, 4, 12, 2, "#f0ffb0"),
  king = E(4200, 58, 24, 44, 120, "#ffe08a", { boss = true, name = { en = "King Snooze", ko = "졸음 대왕" } }),
  eclipse = E(13000, 48, 30, 58, 300, "#ffb060", { boss = true, name = { en = "Moon Muncher", ko = "달 먹보" } }),
}

function B.enemyHpScale(minutes)
  return 1 + 0.12 * minutes + 0.075 * minutes * minutes
end
function B.enemyDmgScale(minutes)
  return 1 + 0.09 * minutes
end
function B.spawnRate(minutes)
  return 1.4 + 0.72 * minutes
end
function B.spawnCap(minutes)
  return min(50 + 32 * minutes, 360)
end

local TABLES = {
  { { "shade", 1 } },
  { { "shade", 3 }, { "skitter", 2 } },
  { { "shade", 3 }, { "skitter", 2 }, { "brute", 0.5 } },
  { { "shade", 3 }, { "skitter", 2 }, { "brute", 0.7 }, { "wisp", 0.8 } },
  { { "shade", 2 }, { "skitter", 2 }, { "brute", 1 }, { "wisp", 1 }, { "gloom", 1 } },
  { { "shade", 2 }, { "skitter", 2 }, { "brute", 1 }, { "wisp", 1 }, { "gloom", 1 }, { "stalker", 1.2 } },
  { { "shade", 2 }, { "skitter", 2 }, { "brute", 1.2 }, { "wisp", 1 }, { "gloom", 1.2 }, { "stalker", 1.2 }, { "leech", 1 } },
  { { "shade", 1 }, { "skitter", 2 }, { "brute", 1.5 }, { "wisp", 1.2 }, { "gloom", 1.5 }, { "stalker", 1.5 }, { "leech", 1.2 } },
}

function B.spawnTable(m)
  return TABLES[min(8, max(1, floor(m) + 1))]
end

-- Picks a value from { {value, weight}, ... }.
function B.weighted(items)
  local total = 0
  for i = 1, #items do total = total + items[i][2] end
  local r = math.random() * total
  for i = 1, #items do
    r = r - items[i][2]
    if r <= 0 then return items[i][1] end
  end
  return items[#items][1]
end

-- ---------------------------------------------------------------- weapons

local MOTHS = { 2, 3, 3, 4, 5 }

-- `area` stays in design units (the weapon scripts convert what is a distance).
local function W(dmg, cd, count, area, pierce, dur)
  return { dmg = dmg, cd = cd, count = count, area = area, pierce = pierce, dur = dur }
end

function B.wstat(id, level, evolved)
  local l = max(0, level - 1)
  if id == "ember_bolt" then
    if evolved then return W(30, 0.5, 3, 60, 99, 0) end
    return W(11 + 3 * l, 0.85 - 0.05 * l, l >= 3 and 2 or 1, 0, l >= 2 and 2 or 1, 0)
  elseif id == "flame_ring" then
    if evolved then return W(30, 0, 6, 84, 0, 999) end
    return W(10 + 4 * l, 2, 2 + floor(l / 2), 78 + 5 * l, 0, 5 + 0.5 * l)
  elseif id == "spark_chain" then
    if evolved then return W(38, 1.0, 30, 220, 0, 0) end
    return W(14 + 5 * l, 1.8 - 0.12 * l, 3 + l, 170, 0, 0)
  elseif id == "oil_flask" then
    if evolved then return W(14, 2.4, 3, 130, 0, 5) end
    return W(5 + 2 * l, 3.2 - 0.2 * l, l >= 3 and 2 or 1, 60 + 8 * l, 0, 3.5)
  elseif id == "moth_swarm" then
    if evolved then return W(22, 1.6, 7, 0, 4, 5) end
    return W(8 + 3 * l, 2.2 - 0.12 * l, MOTHS[min(l, 4) + 1], 0, 1, 3)
  elseif id == "beacon_pulse" then
    if evolved then return W(40, 2.4, 1, 100, 0, 1) end
    return W(12 + 5 * l, 4 - 0.3 * l, 1, 70 + 4 * l, 0, 0)
  elseif id == "glass_shards" then
    if evolved then return W(28, 0.8, 16, 0, 6, 1.2) end
    return W(10 + 3.5 * l, 1.2 - 0.07 * l, 3 + l, 0, 3, 0.7)
  elseif id == "sun_lance" then
    if evolved then return W(70, 2, 3, 60, 0, 0) end
    return W(22 + 9 * l, 3 - 0.2 * l, 1, 22 + 3 * l, 0, 0)
  end
  return W(0, 1, 0, 0, 0, 0)
end

local STAT_ORDER = { "dmg", "cd", "count", "area", "pierce", "dur" }
local STAT_LABEL = {
  dmg = { en = "Damage", ko = "피해" },
  cd = { en = "Cooldown", ko = "쿨다운" },
  count = { en = "Count", ko = "개수" },
  area = { en = "Area", ko = "범위" },
  pierce = { en = "Pierce", ko = "관통" },
  dur = { en = "Duration", ko = "지속" },
}

local function num(d)
  if d == floor(d) then return string.format("%d", d) end
  local s = string.format("%.2f", d)
  return (s:gsub("0+$", ""))
end

-- Human readable difference between two weapon levels, e.g. "+3 Damage · -0.05s Cooldown".
function B.describeUpgrade(id, level)
  local a = B.wstat(id, level - 1, false)
  local b = B.wstat(id, level, false)
  local en, ko = {}, {}
  for _, k in ipairs(STAT_ORDER) do
    local d = floor((b[k] - a[k]) * 100 + 0.5) / 100
    if d ~= 0 then
      local timed = k == "cd" or k == "dur"
      local sign = d > 0 and "+" or ""
      en[#en + 1] = sign .. num(d) .. (timed and "s" or "") .. " " .. STAT_LABEL[k].en
      ko[#ko + 1] = STAT_LABEL[k].ko .. " " .. sign .. num(d) .. (timed and "초" or "")
    end
  end
  return { en = table.concat(en, " · "), ko = table.concat(ko, " · ") }
end

return B
