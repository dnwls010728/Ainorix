-- One weapon of the keeper's build: a child entity of the player that fires on its own
-- cooldown. params { id = weapon id from defs.WEAPONS }. Level and evolution live in the
-- Game script's build (World.game.weapons).
local B = require("scripts.lib.balance")
local W = require("scripts.lib.world")

local Weapon = {}

local U = B.U
local TAU = math.pi * 2
local cos, sin, atan, max, abs, floor = math.cos, math.sin, math.atan, math.max, math.abs, math.floor

function Weapon:onStart()
  self.wid = self.params.id or "ember_bolt"
  self.entry = W.game:findWeapon(self.wid)
  self.cd = 0.4
  self.active, self.angle, self.tick = 0, 0, 0
  self.orbs = {} -- flame ring: child entities circling the keeper
end

local function shot(kind, x, y, params)
  params.kind = kind
  return W.spawn("shot_" .. kind, x, y, params)
end

-- How many enemies a beam from (x, y) along angle a touches; with dmg it also hurts them.
local function lineHits(x, y, a, len, width, dmg, weapon)
  local cx, cy = cos(a), sin(a)
  local n = 0
  local list = W.enemyList
  local hits = {}
  for i = 1, #list do
    local e = list[i]
    if not e.dead then
      local dx, dy = e.x - x, e.y - y
      local along = dx * cx + dy * cy
      if along >= 0 and along <= len and abs(dx * cy - dy * cx) < width / 2 + e.r then
        n = n + 1
        hits[n] = e
      end
    end
  end
  if dmg then
    for i = 1, n do hits[i]:takeDamage(dmg, weapon) end
  end
  return n
end

function Weapon:setOrbs(count, rings)
  local want = count * rings
  while #self.orbs > want do scene.destroy(table.remove(self.orbs)) end
  while #self.orbs < want do
    self.orbs[#self.orbs + 1] = scene.instantiate("prefabs/orb.prefab.json", { parent = self.id })
  end
end

function Weapon:onUpdate(dt)
  if dt == 0 then return end
  local g, p = W.game, W.player
  local w = self.entry
  local id = self.wid
  local s = B.wstat(id, w.level, w.evolved)
  local dmg = s.dmg * g.dmgMul
  local x, y = p.x, p.y
  if w.evolved and not self.wasEvolved then
    self.wasEvolved = true
    self.cd, self.active = 0, 0
  end
  self.cd = self.cd - dt

  if id == "ember_bolt" then
    if self.cd > 0 then return end
    local used, n = {}, 0
    for i = 0, s.count - 1 do
      local t = W.nearest(x, y, 520 * U, used)
      if not t and n > 0 then t = W.nearest(x, y, 520 * U) end
      if not t then break end
      used[t.id] = true
      n = n + 1
      local a = atan(t.y - y, t.x - x)
      shot("bolt", x, y, { vx = cos(a) * 520 * U, vy = sin(a) * 520 * U, dmg = dmg, pierce = s.pierce, life = 1.4,
        weapon = id, explode = s.area * U * g.areaMul, evolved = w.evolved })
    end
    if n > 0 then
      self.cd = s.cd * g.cdMul
      W.sfx("shoot")
    else
      self.cd = 0.1
    end

  elseif id == "flame_ring" then
    local on = true
    if not w.evolved then
      if self.active > 0 then
        self.active = self.active - dt
      elseif self.cd <= 0 then
        self.active = s.dur * g.areaMul
        self.cd = s.dur * g.areaMul + s.cd * g.cdMul
      end
      on = self.active > 0
    end
    if not on then
      self:setOrbs(0, 0)
      return
    end
    local rings = w.evolved and 2 or 1
    self:setOrbs(s.count, rings)
    self.angle = self.angle + dt * 3.2
    self.tick = self.tick - dt
    local hurt = self.tick <= 0
    if hurt then self.tick = 0.25 end
    local k = 0
    for ring = 0, rings - 1 do
      local rad = (s.area + ring * 70) * U * g.areaMul
      for i = 0, s.count - 1 do
        local a = self.angle * (ring > 0 and -0.8 or 1) + (i / s.count) * TAU
        local ox, oy = cos(a) * rad, sin(a) * rad
        k = k + 1
        scene.set(self.orbs[k], "Transform", { position = { x = ox, y = oy, z = 0 } })
        if hurt then W.areaDamage(x + ox, y + oy, 19 * U, dmg, id, 60 * U) end
      end
    end

  elseif id == "spark_chain" then
    if self.cd > 0 then return end
    local cur = W.nearest(x, y, max(260 * U, p.lightR))
    if not cur then
      self.cd = 0.15
      return
    end
    self.cd = s.cd * g.cdMul
    local hit, n, d2 = {}, 0, dmg
    local px, py = x, y
    while cur and n < s.count do
      hit[cur.id] = true
      local cx, cy = cur.x, cur.y
      W.beam(px, py, cx, cy, 5 * U, "#9fe6ff", 0.16)
      cur:takeDamage(d2, id)
      d2 = d2 * (w.evolved and 0.98 or 0.92)
      n = n + 1
      px, py = cx, cy
      cur = W.nearest(cx, cy, s.area * U * g.areaMul, hit)
    end
    W.sfx("zap")

  elseif id == "oil_flask" then
    if self.cd > 0 then return end
    local any = false
    local list = W.enemyList
    for _ = 1, s.count do
      if #list == 0 then break end
      -- aim at a random enemy reasonably close
      local t = nil
      for _ = 1, 6 do
        local c = list[math.random(#list)]
        local dx, dy = c.x - x, c.y - y
        if dx * dx + dy * dy < (380 * U) ^ 2 then
          t = c
          break
        end
      end
      t = t or W.nearest(x, y, 420 * U)
      if not t then break end
      W.spawn("flask", x, y, { tx = t.x, ty = t.y, dmg = dmg, area = s.area * U * g.areaMul, dur = s.dur * g.areaMul, evolved = w.evolved })
      any = true
    end
    self.cd = any and s.cd * g.cdMul or 0.2

  elseif id == "moth_swarm" then
    if self.cd > 0 then return end
    if #W.enemyList == 0 then
      self.cd = 0.2
      return
    end
    self.cd = s.cd * g.cdMul
    for _ = 1, s.count do
      local a = math.random() * TAU
      shot("moth", x, y, { vx = cos(a) * 200 * U, vy = sin(a) * 200 * U, dmg = dmg, pierce = s.pierce, life = s.dur * g.areaMul,
        weapon = id, evolved = w.evolved })
    end

  elseif id == "beacon_pulse" then
    if self.cd > 0 then return end
    self.cd = s.cd * g.cdMul
    local r = p.lightR * (s.area / 100) * g.areaMul
    W.areaDamage(x, y, r, dmg, id, 260 * U, s.dur)
    W.ring(x, y, r, "#ffe27a", w.evolved)
    W.sfx("pulse")

  elseif id == "glass_shards" then
    if self.cd > 0 then return end
    self.cd = s.cd * g.cdMul
    local base = atan(p.fy, p.fx)
    for i = 0, s.count - 1 do
      local a
      if w.evolved then a = (i / s.count) * TAU else a = base + (i - (s.count - 1) / 2) * 0.14 end
      shot("shard", x, y, { vx = cos(a) * 600 * U, vy = sin(a) * 600 * U, dmg = dmg, pierce = s.pierce, life = s.dur * g.areaMul,
        weapon = id, evolved = w.evolved })
    end
    W.sfx("shoot")

  elseif id == "sun_lance" then
    if self.cd > 0 then return end
    local len = 540 * U * g.areaMul
    local width = s.area * U
    local list = W.enemyList
    -- choose the direction that pierces the most enemies among a few candidates
    local bestA, bestN = 0, 0
    for _ = 1, 8 do
      if #list == 0 then break end
      local c = list[math.random(#list)]
      local dx, dy = c.x - x, c.y - y
      if dx * dx + dy * dy <= len * len then
        local a = atan(dy, dx)
        local n = lineHits(x, y, a, len, width)
        if n > bestN then bestN, bestA = n, a end
      end
    end
    if bestN == 0 then
      local t = W.nearest(x, y, len)
      if not t then
        self.cd = 0.2
        return
      end
      bestA = atan(t.y - y, t.x - x)
    end
    self.cd = s.cd * g.cdMul
    for i = 0, s.count - 1 do
      local a = bestA + (i / s.count) * TAU
      lineHits(x, y, a, len, width, dmg, id)
      W.beam(x, y, x + cos(a) * len, y + sin(a) * len, width, "#fff2b0", 0.22)
    end
    W.sfx("zap")
    W.shake(3)
  end
end

return Weapon
