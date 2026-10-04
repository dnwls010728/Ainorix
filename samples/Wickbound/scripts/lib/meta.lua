-- Persistent progress: Glims, meta upgrades, unlocks, achievements, lifetime stats and
-- settings, stored as one value in the engine's save slot (save.get / save.set / save.flush).
local D = require("scripts.lib.defs")

local KEY = "wickbound.save.v1"
local BIG = 1e12
local floor, min, max = math.floor, math.min, math.max

local M = {}

local function contains(list, v)
  for _, x in ipairs(list) do
    if x == v then return true end
  end
  return false
end

function M.defaultSave()
  return {
    version = 1,
    glims = 0,
    metaLevels = {},
    unlockedKeepers = { "ada" },
    selectedKeeper = "ada",
    unlockedStages = { "moor" },
    selectedStage = "moor",
    achievements = {},
    stats = { runs = 0, wins = 0, kills = 0, bossKills = 0, bestTime = 0, braziersLit = 0, glimsEarned = 0, maxLevel = 0, evolutions = 0 },
    settings = { music = 0.6, sfx = 0.8, shake = true, damageNumbers = true, locale = "en" },
    daily = { lastClaim = "", streak = 0 }, -- lastClaim = YYYY-MM-DD or ""
    noAds = false,
  }
end

local function num(v, def, lo, hi)
  if type(v) == "number" and v == v and v ~= math.huge and v ~= -math.huge then return min(hi, max(lo, v)) end
  return def
end
local function bool(v, def)
  if type(v) == "boolean" then return v end
  return def
end

-- Rebuilds a valid save from whatever was stored; anything malformed falls back to defaults.
function M.sanitize(raw)
  local d = M.defaultSave()
  if type(raw) ~= "table" then return d end
  d.glims = floor(num(raw.glims, 0, 0, BIG))

  if type(raw.metaLevels) == "table" then
    for _, m in ipairs(D.META) do
      local lv = raw.metaLevels[m.id]
      if type(lv) == "number" and lv == lv then
        local c = floor(min(m.max, max(0, lv)))
        if c > 0 then d.metaLevels[m.id] = c end
      end
    end
  end

  if type(raw.unlockedKeepers) == "table" then
    for _, id in ipairs(raw.unlockedKeepers) do
      if type(id) == "string" and D.keeperById(id).id == id and not contains(d.unlockedKeepers, id) then
        d.unlockedKeepers[#d.unlockedKeepers + 1] = id
      end
    end
  end
  if contains(d.unlockedKeepers, raw.selectedKeeper) then d.selectedKeeper = raw.selectedKeeper end

  if type(raw.unlockedStages) == "table" then
    for _, id in ipairs(raw.unlockedStages) do
      if type(id) == "string" and D.stageById(id).id == id and not contains(d.unlockedStages, id) then
        d.unlockedStages[#d.unlockedStages + 1] = id
      end
    end
  end
  if contains(d.unlockedStages, raw.selectedStage) then d.selectedStage = raw.selectedStage end

  if type(raw.achievements) == "table" then
    for _, a in ipairs(D.ACHIEVEMENTS) do
      if raw.achievements[a.id] == true then d.achievements[a.id] = true end
    end
  end

  if type(raw.stats) == "table" then
    for k, v in pairs(d.stats) do d.stats[k] = num(raw.stats[k], v, 0, BIG) end
  end

  if type(raw.settings) == "table" then
    local s = raw.settings
    d.settings.music = num(s.music, d.settings.music, 0, 1)
    d.settings.sfx = num(s.sfx, d.settings.sfx, 0, 1)
    d.settings.shake = bool(s.shake, d.settings.shake)
    d.settings.damageNumbers = bool(s.damageNumbers, d.settings.damageNumbers)
    if s.locale == "en" or s.locale == "ko" then d.settings.locale = s.locale end
  end
  if type(raw.daily) == "table" then
    local lc = raw.daily.lastClaim
    d.daily.lastClaim = type(lc) == "string" and lc:match("^%d%d%d%d%-%d%d%-%d%d$") or ""
    d.daily.streak = floor(num(raw.daily.streak, 0, 0, 100000))
  end
  d.noAds = bool(raw.noAds, false)
  return d
end

function M.load()
  M.data = M.sanitize(save.get(KEY))
  return M.data
end

function M.save()
  save.set(KEY, M.data)
  save.flush()
end

function M.reset()
  local fresh = M.defaultSave()
  fresh.settings = M.data.settings
  fresh.noAds = M.data.noAds
  M.data = fresh
  M.save()
end

local function level(id)
  return M.data.metaLevels[id] or 0
end
M.level = level

-- Multipliers and bonuses handed to a run. All multipliers are 1-based.
function M.runModifiers()
  local e = D.META_EFFECT
  return {
    hpMul = 1 + e.vitality * level("vitality"),
    damageMul = 1 + e.might * level("might"),
    speedMul = 1 + e.swiftness * level("swiftness"),
    lightMul = 1 + e.radiance * level("radiance"),
    oilDrainMul = 1 - e.thrift * level("thrift"),
    glimMul = 1 + e.greed * level("greed"),
    xpMul = 1 + e.wisdom * level("wisdom"),
    revives = level("second_wind") * e.second_wind,
    rerolls = 1 + level("reroll") * e.reroll,
  }
end

-- Cost of the next level, or nil when maxed.
function M.metaCost(id)
  local def = D.metaById(id)
  if not def then return nil end
  local lv = level(id)
  if lv >= def.max then return nil end
  return floor(def.baseCost * def.costMul ^ lv + 0.5)
end

function M.buyMeta(id)
  local cost = M.metaCost(id)
  if not cost or M.data.glims < cost then return false end
  M.data.glims = M.data.glims - cost
  M.data.metaLevels[id] = level(id) + 1
  M.save()
  return true
end

function M.unlockKeeper(id)
  local k = D.keeperById(id)
  if k.id ~= id or contains(M.data.unlockedKeepers, id) or M.data.glims < k.cost then return false end
  M.data.glims = M.data.glims - k.cost
  M.data.unlockedKeepers[#M.data.unlockedKeepers + 1] = id
  M.save()
  return true
end

function M.keeperUnlocked(id) return contains(M.data.unlockedKeepers, id) end
function M.stageUnlocked(id) return contains(M.data.unlockedStages, id) end

function M.selectKeeper(id)
  if not M.keeperUnlocked(id) then return end
  M.data.selectedKeeper = id
  M.save()
end

function M.selectStage(id)
  if not M.stageUnlocked(id) then return end
  M.data.selectedStage = id
  M.save()
end

-- Applies a finished run: Glims, stats, stage unlocks, achievements. Returns newly earned achievement ids.
function M.applyRun(result)
  local data = M.data
  data.glims = data.glims + max(0, result.glimsEarned)
  local s = data.stats
  s.runs = s.runs + 1
  if result.victory then s.wins = s.wins + 1 end
  s.kills = s.kills + result.kills
  s.bossKills = s.bossKills + result.bossesKilled
  s.bestTime = max(s.bestTime, result.time)
  s.braziersLit = s.braziersLit + result.braziersLit
  s.glimsEarned = s.glimsEarned + result.glimsEarned
  s.maxLevel = max(s.maxLevel, result.level)
  s.evolutions = s.evolutions + result.evolutions
  if result.victory then
    for _, st in ipairs(D.STAGES) do
      if st.requires == result.stageId and not contains(data.unlockedStages, st.id) then
        data.unlockedStages[#data.unlockedStages + 1] = st.id
      end
    end
  end
  local earned = {}
  for _, a in ipairs(D.ACHIEVEMENTS) do
    if not data.achievements[a.id] and a.check(s, result, data) then
      data.achievements[a.id] = true
      data.glims = data.glims + a.reward
      earned[#earned + 1] = a.id
    end
  end
  M.save()
  return earned
end

function M.grantGlims(amount)
  M.data.glims = M.data.glims + max(0, amount)
  M.save()
end

-- ---------------------------------------------------------------- daily gift

local MONTH_DAYS = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 }

-- The day before a YYYY-MM-DD date.
local function dayBefore(date)
  local y, m, d = date:match("^(%d+)-(%d+)-(%d+)$")
  y, m, d = tonumber(y), tonumber(m), tonumber(d) - 1
  if d < 1 then
    m = m - 1
    if m < 1 then y, m = y - 1, 12 end
    d = MONTH_DAYS[m]
    if m == 2 and y % 4 == 0 and (y % 100 ~= 0 or y % 400 == 0) then d = 29 end
  end
  return string.format("%04d-%02d-%02d", y, m, d)
end

local function streakIfClaimed()
  return M.data.daily.lastClaim == dayBefore(time.date()) and M.data.daily.streak or 0
end

function M.dailyAvailable()
  return M.data.daily.lastClaim ~= time.date()
end

function M.dailyAmount()
  return D.DAILY_REWARDS[streakIfClaimed() % #D.DAILY_REWARDS + 1]
end

-- Claims today's gift; returns the Glims granted (0 if already claimed).
function M.claimDaily(doubled)
  if not M.dailyAvailable() then return 0 end
  local amount = M.dailyAmount() * (doubled and 2 or 1)
  local streak = streakIfClaimed() + 1
  M.data.glims = M.data.glims + amount
  M.data.daily = { lastClaim = time.date(), streak = streak }
  M.save()
  return amount
end

function M.purchaseApplied(productId)
  local data = M.data
  if productId == "no_ads" then
    data.noAds = true
  elseif productId == "starter" then
    data.glims = data.glims + 2000
    if not contains(data.unlockedKeepers, "bram") then data.unlockedKeepers[#data.unlockedKeepers + 1] = "bram" end
  elseif productId == "glims_s" then
    data.glims = data.glims + 1000
  elseif productId == "glims_l" then
    data.glims = data.glims + 7000
  else
    return
  end
  M.save()
end

return M
