-- A campfire: stand in its trigger ring for 3 seconds to light it. A lit brazier is a
-- permanent light (Light2D), refills the oil, scatters XP and pushes critters back.
local B = require("scripts.lib.balance")
local W = require("scripts.lib.world")
local Pickup = require("scripts.run.pickup")

local Brazier = {}

local U = B.U
local TAU = math.pi * 2

function Brazier:onStart()
  self.lit, self.inside, self.progress = false, false, 0
  local p = self:position()
  self.x, self.y = p.x, p.y
  self.fill = scene.child(self.id, "Progress")
  self.t = 0
  W.braziers[#W.braziers + 1] = self
end

function Brazier:onTriggerEnter(other)
  if W.player and other == W.player.id then self.inside = true end
end

function Brazier:onTriggerExit(other)
  if W.player and other == W.player.id then self.inside = false end
end

function Brazier:onUpdate(dt)
  if dt == 0 then return end
  self.t = self.t + dt
  if self.lit then
    local fl = 1 + math.sin(self.t * 11 + self.x) * 0.12
    scene.set(self.fire, "Transform", { scale = { x = fl, y = fl, z = 1 } })
    self:set("Light2D", { radius = B.BRAZIER_LIGHT * 1.15 * (1 + math.sin(self.t * 7 + self.x) * 0.02) })
    return
  end
  local before = self.progress
  if self.inside then
    self.progress = self.progress + dt / B.BRAZIER_TIME
    if self.progress >= 1 then
      self:ignite()
      return
    end
  elseif self.progress > 0 then
    self.progress = math.max(0, self.progress - dt * 0.5)
  end
  if self.progress ~= before then
    local s = math.max(0.001, self.progress)
    scene.set(self.fill, "Transform", { scale = { x = s, y = s, z = 1 } })
  end
end

function Brazier:ignite()
  local g, p = W.game, W.player
  self.lit, self.progress = true, 1
  self.fire = scene.child(self.id, "Fire")
  scene.set(self.fire, "Sprite", { visible = true })
  scene.set(scene.child(self.id, "FireCore"), "Sprite", { visible = true })
  scene.set(scene.child(self.id, "Sparks"), "ParticleEmitter", { playing = true })
  for _, name in ipairs({ "Ring", "Progress", "Ember", "Beacon" }) do
    scene.set(scene.child(self.id, name), "Sprite", { visible = false })
  end
  self:set("Light2D", { radius = B.BRAZIER_LIGHT * 1.15, strength = 1 })
  W.lights[#W.lights + 1] = { x = self.x, y = self.y, r = B.BRAZIER_LIGHT }

  p.oil = p.maxOil
  g.glims = g.glims + 10 * g:glimMul()
  g.braziersLit = g.braziersLit + 1
  local n = 8 + math.floor(g.time / 60) * 2
  for i = 0, n - 1 do
    local a = (i / n) * TAU
    Pickup.drop("xp", self.x + math.cos(a) * 60 * U, self.y + math.sin(a) * 60 * U, 2 + math.floor(g.time / 120))
  end
  scene.broadcast("pull") -- every ember, Glim and oil drop flies to the keeper
  for _, e in ipairs(W.enemiesIn(self.x, self.y, B.BRAZIER_LIGHT)) do
    local dx, dy = e.x - self.x, e.y - self.y
    local d = math.sqrt(dx * dx + dy * dy)
    if d == 0 then d = 1 end
    e:takeDamage(e.boss and 0 or 30 + g.time * 0.15, "brazier", dx / d * 500 * U, dy / d * 500 * U)
  end
  W.ring(self.x, self.y, B.BRAZIER_LIGHT, g.stage.accent, true)
  W.burst(self.x, self.y, "#ffb347", 40, 260 * U)
  W.sfx("brazier")
  W.shake(6)
  W.toast({ en = "Campfire lit! " .. g.braziersLit .. "/" .. g.braziersTotal, ko = "모닥불을 밝혔어요! " .. g.braziersLit .. "/" .. g.braziersTotal })
  if g.braziersLit == g.braziersTotal then Pickup.drop("chest", self.x, self.y - 50 * U, 1, false) end
end

return Brazier
