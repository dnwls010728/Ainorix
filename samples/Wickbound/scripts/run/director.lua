-- Spawn director: places the braziers, then feeds the arena — a steady trickle, an elite
-- every minute, swarm rings and the two bosses.
local D = require("scripts.lib.defs")
local B = require("scripts.lib.balance")
local W = require("scripts.lib.world")

local Director = {}

local U = B.U
local TAU = math.pi * 2
local HALF = B.WORLD / 2
local cos, sin, min, floor = math.cos, math.sin, math.min, math.floor

local function clamp(v, a, b)
  if v < a then return a end
  if v > b then return b end
  return v
end

function Director:onStart()
  self.spawnAcc = 0
  self.eliteT, self.swarmT = 60, 90
  self.nextBossTime, self.warned = D.MID_BOSS_TIME, false
  W.game.bossIndex = 0
  -- braziers on a jittered ring around the start
  local a0 = math.random() * TAU
  for i = 0, B.BRAZIER_COUNT - 1 do
    local a = a0 + (i / B.BRAZIER_COUNT) * TAU + (math.random() - 0.5) * 0.5
    local d
    if i % 2 == 0 then d = 430 + math.random() * 130 else d = 760 + math.random() * 180 end
    W.spawn("brazier", cos(a) * d * U, sin(a) * d * U)
  end
end

-- A point just outside the view, inside the arena.
function Director:spawnPos()
  local p = W.player
  local edge = HALF - 10 * U
  for _ = 1, 6 do
    local a = math.random() * TAU
    local d = B.SPAWN_DIST_MIN + math.random() * (B.SPAWN_DIST_MAX - B.SPAWN_DIST_MIN)
    local x, y = p.x + cos(a) * d, p.y + sin(a) * d
    if x > -edge and y > -edge and x < edge and y < edge then return x, y end
  end
  local a = math.atan(-p.y, -p.x) + (math.random() - 0.5) * 1.6
  return clamp(p.x + cos(a) * B.SPAWN_DIST_MIN, -edge, edge), clamp(p.y + sin(a) * B.SPAWN_DIST_MIN, -edge, edge)
end

local function spawn(kind, x, y, elite, chest)
  W.spawn("enemy_" .. kind, x, y, { kind = kind, elite = elite or false, chest = chest or false })
end

function Director:onUpdate(dt)
  if dt == 0 then return end
  local g, p = W.game, W.player
  if g.phase ~= "playing" then return end
  local minutes = g.time / 60
  local count = #W.enemyList
  local bossAlive = false
  for _, e in ipairs(W.enemyList) do
    if e.boss then bossAlive = true end
  end

  -- regular trickle
  local rate = B.spawnRate(minutes) * (p.dread and 1.5 or 1) * (bossAlive and 0.55 or 1)
  if g.endless then rate = rate * 1.3 end
  self.spawnAcc = self.spawnAcc + rate * dt
  local cap = B.spawnCap(minutes)
  while self.spawnAcc >= 1 do
    self.spawnAcc = self.spawnAcc - 1
    if count < cap then
      local x, y = self:spawnPos()
      spawn(B.weighted(B.spawnTable(minutes)), x, y)
      count = count + 1
    end
  end

  -- elites
  self.eliteT = self.eliteT - dt
  if self.eliteT <= 0 then
    self.eliteT = 60
    local x, y = self:spawnPos()
    local kinds = minutes < 4 and { "brute", "shade" } or { "brute", "stalker", "gloom" }
    spawn(kinds[math.random(#kinds)], x, y, true, floor(minutes + 0.5) % 2 == 0)
  end

  -- swarm ring
  self.swarmT = self.swarmT - dt
  if self.swarmT <= 0 then
    self.swarmT = 75
    local n = min(14 + floor(minutes * 3), 40)
    local a0 = math.random() * TAU
    local edge = HALF - 10 * U
    for i = 0, n - 1 do
      local a = a0 + (i / n) * TAU
      spawn("skitter", clamp(p.x + cos(a) * B.SPAWN_DIST_MIN, -edge, edge), clamp(p.y + sin(a) * B.SPAWN_DIST_MIN, -edge, edge))
    end
    W.toast({ en = "Here they come, from all around!", ko = "사방에서 우르르 몰려와요!" })
  end

  -- bosses: King Snooze at 5:00, the Moon Muncher at 10:00, then alternating every 5 minutes
  local kind = g.bossIndex % 2 == 0 and "king" or "eclipse"
  if not self.warned and g.time >= self.nextBossTime - 3 then
    self.warned = true
    if W.hud then W.hud:banner(B.ENEMIES[kind].name) end
    W.sfx("boss")
  end
  if g.time >= self.nextBossTime then
    g.bossIndex = g.bossIndex + 1
    self.nextBossTime = self.nextBossTime + (g.bossIndex == 1 and D.RUN_LENGTH - D.MID_BOSS_TIME or 300)
    self.warned = false
    local x, y = self:spawnPos()
    spawn(kind, x, y, false, true)
    W.shake(10)
  end
end

return Director
