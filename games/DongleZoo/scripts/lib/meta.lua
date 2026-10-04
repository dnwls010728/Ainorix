-- Persistent progress: coins, items, themes, collection, best score, daily gift, daily
-- missions and settings, stored as one value in the engine's save slot.
local D = require("scripts.lib.defs")

local KEY = "donglezoo.save.v1"
local BIG = 1e12
local floor, min, max = math.floor, math.min, math.max

local M = {}

function M.defaultSave()
  return {
    version = 1,
    coins = 0,
    best = 0,
    items = { hammer = D.START_ITEMS, shake = D.START_ITEMS, wand = D.START_ITEMS },
    themes = { "meadow" },
    theme = "meadow",
    seen = { true, false, false, false, false, false, false, false, false, false, false }, -- collection, by tier
    stats = { games = 0, merges = 0, drops = 0, whales = 0 },
    settings = { music = true, sfx = true, locale = "" }, -- locale "" = not chosen yet
    daily = { lastClaim = "", streak = 0 },
    missions = { date = "", ids = {}, progress = {}, claimed = {} },
    noAds = false,
    tutorial = false, -- the first-game hint was shown
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
local function contains(list, v)
  for _, x in ipairs(list) do
    if x == v then return true end
  end
  return false
end
local function isDate(s)
  return type(s) == "string" and s:match("^%d%d%d%d%-%d%d%-%d%d$") ~= nil
end

-- Rebuilds a valid save from whatever was stored; anything malformed falls back to defaults.
function M.sanitize(raw)
  local d = M.defaultSave()
  if type(raw) ~= "table" then return d end
  d.coins = floor(num(raw.coins, 0, 0, BIG))
  d.best = floor(num(raw.best, 0, 0, BIG))
  if type(raw.items) == "table" then
    for _, it in ipairs(D.ITEMS) do d.items[it.id] = floor(num(raw.items[it.id], d.items[it.id], 0, 9999)) end
  end
  if type(raw.themes) == "table" then
    for _, id in ipairs(raw.themes) do
      if type(id) == "string" and D.theme(id).id == id and not contains(d.themes, id) then d.themes[#d.themes + 1] = id end
    end
  end
  if contains(d.themes, raw.theme) then d.theme = raw.theme end
  if type(raw.seen) == "table" then
    for i = 2, D.MAX_TIER do d.seen[i] = raw.seen[i] == true end
  end
  if type(raw.stats) == "table" then
    for k, v in pairs(d.stats) do d.stats[k] = floor(num(raw.stats[k], v, 0, BIG)) end
  end
  if type(raw.settings) == "table" then
    d.settings.music = bool(raw.settings.music, true)
    d.settings.sfx = bool(raw.settings.sfx, true)
    local l = raw.settings.locale
    if l == "en" or l == "ko" then d.settings.locale = l end
  end
  if type(raw.daily) == "table" then
    d.daily.lastClaim = isDate(raw.daily.lastClaim) and raw.daily.lastClaim or ""
    d.daily.streak = floor(num(raw.daily.streak, 0, 0, 100000))
  end
  local ms = raw.missions
  if type(ms) == "table" and isDate(ms.date) and type(ms.ids) == "table" then
    d.missions.date = ms.date
    for _, id in ipairs(ms.ids) do
      if type(id) == "string" and D.mission(id) and #d.missions.ids < 3 and not contains(d.missions.ids, id) then
        d.missions.ids[#d.missions.ids + 1] = id
        d.missions.progress[id] = floor(num(type(ms.progress) == "table" and ms.progress[id], 0, 0, BIG))
        d.missions.claimed[id] = type(ms.claimed) == "table" and ms.claimed[id] == true
      end
    end
  end
  d.noAds = bool(raw.noAds, false)
  d.tutorial = bool(raw.tutorial, false)
  return d
end

function M.load()
  M.data = M.sanitize(save.get(KEY))
  M.rollMissions()
  return M.data
end

function M.save()
  save.set(KEY, M.data)
  save.flush()
end

-- ---------------------------------------------------------------- coins, items, themes

function M.addCoins(n)
  M.data.coins = M.data.coins + max(0, floor(n))
end

function M.spend(n)
  if M.data.coins < n then return false end
  M.data.coins = M.data.coins - n
  return true
end

function M.buyItem(id)
  local it = D.item(id)
  if not it or not M.spend(it.price) then return false end
  M.data.items[id] = M.data.items[id] + 1
  M.save()
  return true
end

function M.grantItem(id, n)
  M.data.items[id] = M.data.items[id] + (n or 1)
  M.save()
end

function M.useItem(id)
  if M.data.items[id] <= 0 then return false end
  M.data.items[id] = M.data.items[id] - 1
  M.bump("items", 1)
  M.save()
  return true
end

function M.hasTheme(id)
  return contains(M.data.themes, id)
end

function M.buyTheme(id)
  local th = D.theme(id)
  if th.id ~= id or M.hasTheme(id) or not M.spend(th.price) then return false end
  M.data.themes[#M.data.themes + 1] = id
  M.data.theme = id
  M.save()
  return true
end

function M.selectTheme(id)
  if not M.hasTheme(id) then return false end
  M.data.theme = id
  M.save()
  return true
end

function M.purchaseApplied(productId)
  local p = D.product(productId)
  if not p then return end
  if productId == "no_ads" then M.data.noAds = true end
  if p.coins then M.addCoins(p.coins) end
  M.save()
end

-- Marks an animal as met; returns true the first time.
function M.see(tier)
  if M.data.seen[tier] then return false end
  M.data.seen[tier] = true
  M.addCoins(D.NEW_ANIMAL_COINS)
  return true
end

-- ---------------------------------------------------------------- daily gift

local MONTH_DAYS = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 }

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

-- Streak days already collected, counting only an unbroken streak.
function M.dailyStreak()
  local d = M.data.daily
  if d.lastClaim == time.date() or d.lastClaim == dayBefore(time.date()) then return d.streak end
  return 0
end

function M.dailyAvailable()
  return M.data.daily.lastClaim ~= time.date()
end

-- Day 1..7 of the gift that is (or was today) on offer.
function M.dailyDay()
  local s = M.dailyStreak()
  if not M.dailyAvailable() then s = s - 1 end
  return s % #D.DAILY_REWARDS + 1
end

function M.claimDaily(doubled)
  if not M.dailyAvailable() then return 0 end
  local amount = D.DAILY_REWARDS[M.dailyDay()] * (doubled and 2 or 1)
  local streak = M.dailyStreak() + 1
  M.addCoins(amount)
  M.data.daily = { lastClaim = time.date(), streak = streak }
  M.save()
  return amount
end

-- ---------------------------------------------------------------- daily missions

local function hash(s)
  local h = 5381
  for i = 1, #s do h = (h * 33 + s:byte(i)) % 2147483647 end
  return h
end

-- Picks today's three missions (the same for everyone on that day) when the date changed.
function M.rollMissions()
  local ms, today = M.data.missions, time.date()
  if ms.date == today and #ms.ids == 3 then return end
  local pool = {}
  for i, m in ipairs(D.MISSIONS) do pool[i] = m.id end
  local h = hash(today)
  local ids = {}
  for _ = 1, 3 do
    h = (h * 1103515245 + 12345) % 2147483648
    ids[#ids + 1] = table.remove(pool, h % #pool + 1)
  end
  M.data.missions = { date = today, ids = ids, progress = {}, claimed = {} }
  for _, id in ipairs(ids) do
    M.data.missions.progress[id] = 0
    M.data.missions.claimed[id] = false
  end
end

-- Adds to a counted stat, or raises a best-of stat; missions watching it follow.
function M.bump(stat, amount)
  M.rollMissions()
  local ms = M.data.missions
  for _, id in ipairs(ms.ids) do
    local def = D.mission(id)
    if def.stat == stat then
      if def.best then
        ms.progress[id] = max(ms.progress[id], amount)
      else
        ms.progress[id] = ms.progress[id] + amount
      end
    end
  end
end

function M.missionDone(id)
  return M.data.missions.progress[id] >= D.mission(id).goal
end

function M.claimMission(id)
  local ms = M.data.missions
  if ms.claimed[id] ~= false or not M.missionDone(id) then return 0 end
  ms.claimed[id] = true
  M.addCoins(D.mission(id).reward)
  M.save()
  return D.mission(id).reward
end

-- Missions finished but not collected yet (the red dot on the menu).
function M.missionsReady()
  local n = 0
  for _, id in ipairs(M.data.missions.ids) do
    if M.missionDone(id) and not M.data.missions.claimed[id] then n = n + 1 end
  end
  return n
end

-- ---------------------------------------------------------------- end of a game

-- Applies a finished game; returns the coins earned and whether it is a new best.
function M.applyGame(score)
  local coins = floor(score * D.COINS_PER_SCORE)
  local newBest = score > M.data.best
  if newBest then M.data.best = score end
  M.data.stats.games = M.data.stats.games + 1
  M.bump("games", 1)
  M.bump("score", score)
  M.addCoins(coins)
  M.save()
  return coins, newBest
end

return M
