-- Things on the ground: XP embers, oil, hearts, Glims, chests. A kinematic body the
-- player's sensors see: entering the Magnet pulls it in, touching the Hurtbox collects it.
-- params { kind = xp | oil | heal | glim | chest, value, boss }
local B = require("scripts.lib.balance")
local W = require("scripts.lib.world")

local Pickup = {}

local U = B.U
local sqrt, min = math.sqrt, math.min

local function rgb(hex)
  return { tonumber(hex:sub(2, 3), 16) / 255, tonumber(hex:sub(4, 5), 16) / 255, tonumber(hex:sub(6, 7), 16) / 255 }
end

-- Spawns a pickup; XP merges into an existing ember once there are very many.
function Pickup.drop(kind, x, y, value, boss)
  if kind == "xp" and W.pickups > 450 then
    for _, gem in pairs(W.xpGems) do
      gem.value = gem.value + value
      return
    end
  end
  W.spawn("pickup_" .. kind, x, y, { kind = kind, value = value, boss = boss or false })
end

function Pickup:onStart()
  self.kind = self.params.kind or "xp"
  self.value = self.params.value or 1
  self.boss = self.params.boss or false
  local a = math.random() * math.pi * 2
  self.vx, self.vy = math.cos(a) * 60 * U, math.sin(a) * 60 * U
  self.age = 0
  self.pulled = false
  local p = self:position()
  self.x, self.y = p.x, p.y
  W.pickups = W.pickups + 1
  if self.kind == "xp" then
    W.xpGems[self.id] = self
    local color = self.value >= 20 and "#ff7ad9" or self.value >= 5 and "#7fd6ff" or "#ffc857"
    local scale = self.value >= 20 and 1.55 or self.value >= 5 and 1.33 or 1
    self:set("Transform", { scale = { x = scale, y = scale, z = 1 } })
    scene.set(scene.child(self.id, "Body"), "Sprite", { color = rgb(color) })
    scene.set(scene.child(self.id, "Glow"), "Sprite", { color = rgb(color) })
  end
end

function Pickup:onDestroy()
  W.pickups = W.pickups - 1
  W.xpGems[self.id] = nil
end

-- Braziers call this on everything (scene.broadcast): embers, Glims and oil fly to the keeper.
function Pickup:pull()
  if self.kind == "xp" or self.kind == "glim" or self.kind == "oil" then self.pulled = true end
end

function Pickup:onTriggerEnter(other)
  if other == W.magnet then
    self.pulled = true
  elseif other == W.hurtbox then
    self:collect()
  end
end

function Pickup:collect()
  if self.collected then return end
  self.collected = true
  local g, p = W.game, W.player
  local kind = self.kind
  if kind == "xp" then
    g:gainXp(self.value)
    W.sfx("pickup")
  elseif kind == "oil" then
    p.oil = min(p.maxOil, p.oil + self.value)
    W.sfx("oil")
  elseif kind == "heal" then
    p.hp = min(p.maxHp, p.hp + self.value)
    W.sfx("oil")
  elseif kind == "glim" then
    g.glims = g.glims + self.value * g:glimMul()
    W.sfx("pickup")
  elseif kind == "chest" then
    g:openChest(self.boss)
  end
  self:destroy()
end

function Pickup:onUpdate(dt)
  if dt == 0 or self.collected then return end
  self.age = self.age + dt
  if self.pulled and self.age > 0.15 then
    local p = W.player
    local dx, dy = p.x - self.x, p.y - self.y
    local d = sqrt(dx * dx + dy * dy)
    if d < 0.01 then d = 0.01 end
    local step = min(d, (420 + self.age * 500) * U * dt)
    self.x, self.y = self.x + dx / d * step, self.y + dy / d * step
    self:setPosition(self.x, self.y, 0)
  elseif self.vx ~= 0 then
    self.x, self.y = self.x + self.vx * dt, self.y + self.vy * dt
    self.vx, self.vy = self.vx * 0.9, self.vy * 0.9
    if self.vx * self.vx + self.vy * self.vy < 1e-4 then self.vx, self.vy = 0, 0 end
    self:setPosition(self.x, self.y, 0)
  end
end

return Pickup
