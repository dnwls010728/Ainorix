-- Shooting target. Needs Tag "target", a Collider and (when it moves) a
-- kinematic RigidBody so the physics shape follows it.
-- params: { move: "none" | "strafe" | "bob", distance: 2, speed: 1, phase: 0, health: 1 }
local Target = {}

function Target:onStart()
  local p = self.params
  self.move = p.move or "none"
  self.distance = p.distance or 2
  self.speed = p.speed or 1
  self.t = p.phase or 0
  self.health = p.health or 1
  self.base = self:position()
  self.color = self:get("MeshRenderer").color
  self.flash = 0
end

function Target:onUpdate(dt)
  self.t = self.t + dt * self.speed
  if self.move == "strafe" then
    self:setPosition(self.base.x + math.sin(self.t) * self.distance, self.base.y, self.base.z)
  elseif self.move == "bob" then
    self:setPosition(self.base.x, self.base.y + math.sin(self.t) * self.distance, self.base.z)
  end
  if self.flash > 0 then
    self.flash = self.flash - dt
    if self.flash <= 0 then self:set("MeshRenderer", { color = self.color }) end
  end
  self:rotate(0, 60 * dt, 0)
end

-- Called by the player's gun (scene.send(id, "onShot", dir)).
function Target:onShot(dir)
  self.health = self.health - 1
  if self.health > 0 then
    audio.play("sounds/hit.wav", { volume = 0.6, pitch = 1.4 })
    self:set("MeshRenderer", { color = { 1, 1, 1 } })
    self.flash = 0.08
    return
  end
  audio.play("sounds/hit.wav", { volume = 0.8 })
  scene.broadcast("onTargetDown", self.id)
  self:destroy()
end

return Target
