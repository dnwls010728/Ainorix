-- Things that fly. Player shots (bolt, shard, moth) are trigger colliders that damage the
-- enemies they pass through; the enemy bullet is seen by the player's Hurtbox sensor.
-- params { kind, vx, vy, dmg, pierce, life, weapon, explode, evolved }
local B = require("scripts.lib.balance")
local W = require("scripts.lib.world")

local Shot = {}

local U = B.U
local sqrt, cos, sin, atan = math.sqrt, math.cos, math.sin, math.atan

local function rgb(hex)
  return { tonumber(hex:sub(2, 3), 16) / 255, tonumber(hex:sub(4, 5), 16) / 255, tonumber(hex:sub(6, 7), 16) / 255 }
end

function Shot:onStart()
  local q = self.params
  self.kind = q.kind or "bolt"
  self.vx, self.vy = q.vx or 0, q.vy or 0
  self.dmg = q.dmg or 1
  self.pierce = q.pierce or 1
  self.life = q.life or (self.kind == "ebullet" and 5 or 1)
  self.weapon = q.weapon or ""
  self.explode = q.explode or 0 -- blast radius (evolved bolt)
  self.evolved = q.evolved or false
  self.hit = {} -- enemy ids already damaged
  self.hits = 0
  local p = self:position()
  self.x, self.y = p.x, p.y
  if self.kind == "bolt" and self.evolved then
    local glow = scene.child(self.id, "Glow")
    scene.set(glow, "Sprite", { color = rgb("#ffd24a") })
    scene.set(glow, "Transform", { scale = { x = 1.5, y = 1.5, z = 1 } })
  elseif self.kind == "moth" and self.evolved then
    scene.set(scene.child(self.id, "Glow"), "Sprite", { color = rgb("#bfe0ff") })
    scene.set(scene.child(self.id, "Body"), "Sprite", { color = rgb("#e8f4ff") })
  end
  if self.kind == "shard" then self:aim() end
end

function Shot:aim()
  self:set("Transform", { rotation = { x = 0, y = 0, z = math.deg(atan(self.vy, self.vx)) } })
end

function Shot:onTriggerEnter(other)
  if self.gone then return end
  if self.kind == "ebullet" then
    if other == W.hurtbox then
      self.gone = true
      W.player:damage(self.dmg)
      self:destroy()
    end
    return
  end
  local e = W.enemies[other]
  if not e or e.dead or self.hit[other] then return end
  self.hit[other] = true
  self.hits = self.hits + 1
  local ex, ey = e.x, e.y
  e:takeDamage(self.dmg, self.weapon, self.vx * 0.12, self.vy * 0.12)
  W.sfx("hit")
  if self.explode > 0 then
    for _, o in ipairs(W.enemiesIn(ex, ey, self.explode)) do
      if o ~= e then o:takeDamage(self.dmg * 0.6, self.weapon) end
    end
    W.ring(ex, ey, self.explode, "#ff9a3c", true)
  end
  if self.kind == "shard" and self.evolved then
    -- ricochet towards another enemy
    local n = W.nearest(ex, ey, 260 * U, self.hit)
    if n then
      local a = atan(n.y - ey, n.x - ex)
      self.vx, self.vy = cos(a) * 600 * U, sin(a) * 600 * U
      self:aim()
    end
  end
  self.pierce = self.pierce - 1
  if self.pierce <= 0 then
    self.gone = true
    self:destroy()
  end
end

function Shot:onUpdate(dt)
  if dt == 0 or self.gone then return end
  self.life = self.life - dt
  if self.life <= 0 then
    self.gone = true
    self:destroy()
    return
  end
  if self.kind == "moth" then
    -- home on the nearest enemy not hit yet; evolved moths come back to the keeper in between
    local t = W.nearest(self.x, self.y, 420 * U, self.hit)
    local tx, ty = W.player.x, W.player.y
    if t then tx, ty = t.x, t.y end
    if t or self.evolved then
      local dx, dy = tx - self.x, ty - self.y
      local d = sqrt(dx * dx + dy * dy)
      if d == 0 then d = 1 end
      self.vx = self.vx + dx / d * 1500 * U * dt
      self.vy = self.vy + dy / d * 1500 * U * dt
    end
    local sp = sqrt(self.vx * self.vx + self.vy * self.vy)
    local top = 330 * U
    if sp > top then self.vx, self.vy = self.vx / sp * top, self.vy / sp * top end
  end
  self.x, self.y = self.x + self.vx * dt, self.y + self.vy * dt
  self:setPosition(self.x, self.y, 0)
end

return Shot
