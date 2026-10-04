-- A crystal shard: hovers, and is collected when the hero walks into it.
-- params: { "phase": 0 }
local Crystal = {}

function Crystal:onStart()
  self.phase = self.params.phase or 0 -- offset of the hover motion, so shards do not move in step
  self.base = self:position()
  self.hero = scene.find("Hero")
end

function Crystal:onUpdate(dt)
  local y = self.base.y + math.sin(time.now() * 2 + self.phase) * 0.12
  self:setPosition(self.base.x, y, self.base.z)
  local hero = scene.get(self.hero, "Transform").position
  local dx, dz = hero.x - self.base.x, hero.z - self.base.z
  if dx * dx + dz * dz > 0.7 * 0.7 then return end

  audio.play("sounds/crystal.wav", { volume = 0.7 })
  -- Sparks that outlive the shard.
  local sparks = scene.create("Crystal Sparks", {
    Transform = { position = { self.base.x, y, self.base.z } },
    ParticleEmitter = { rate = 0, burst = 18, loop = false, lifetime = 0.7, lifetimeVariation = 0.4, speed = 2.4,
                        speedVariation = 0.5, spread = 180, drag = 3, gravity = { 0, -1.5, 0 }, startSize = 0.16,
                        endSize = 0.02, startColor = { 0.6, 1.6, 1.8 }, endColor = { 0.3, 0.6, 1.4 },
                        space = "world", blend = "add", maxParticles = 18 }
  })
  timer.after(1.2, function() scene.destroy(sparks) end)
  scene.send(scene.find("Game"), "onCrystal")
  self:destroy()
end

return Crystal
