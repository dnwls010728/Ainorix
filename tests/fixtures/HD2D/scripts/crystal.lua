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
  vfx.spawn("effects/crystal_sparks.vfx.json", {
    name = "Crystal Sparks", position = { self.base.x, y, self.base.z }, autoDestroy = true,
  })
  scene.send(scene.find("Game"), "onCrystal")
  self:destroy()
end

return Crystal
