-- Short-lived world-space bursts survive the coin/enemy that spawned them.
local Effect = {}

function Effect.spawn(position, kind)
  local coin = kind == "coin"
  local id = scene.create(coin and "Coin Sparkles" or "Hit Sparks", {
    Transform = { position = { position.x, position.y, (position.z or 0) + 0.4 } },
    Tag = { tags = "effect" },
    ParticleEmitter = {
      rate = 0, loop = false, lifetime = 0.5, dimensions = 2, space = "world",
      speed = coin and 1.8 or 3, spread = 180, gravity = { 0, -5, 0 },
      startSize = coin and 0.12 or 0.16, endSize = 0.02,
      startColor = coin and { 1, 0.8, 0.1 } or { 1, 0.3, 0.15 },
      endColor = coin and { 1, 0.4, 0.05 } or { 0.6, 0.1, 0.05 },
      startOpacity = 1, endOpacity = 0, maxParticles = 16,
    },
    Script = { path = "scripts/effects.lua" },
  })
  particles.burst(id, coin and 12 or 16)
  return id
end

function Effect:onUpdate(dt)
  self.age = (self.age or 0) + dt
  if self.age >= 0.65 then self:destroy() end
end

return Effect
