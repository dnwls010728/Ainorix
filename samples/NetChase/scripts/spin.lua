-- Spins another entity on every peer. The crystal is replicated (NetSync), so a script attached to
-- it would run on the server only, and its rotation is not part of the snapshot.
-- params: { "target": "Target", "degreesPerSecond": 45 }
local Spin = {}

function Spin:onStart()
  self.target = scene.find(self.params.target or "Target") -- name of the entity to spin
  self.speed = self.params.degreesPerSecond or 45          -- around world +Y
end

function Spin:onUpdate(dt)
  if not self.target or not scene.exists(self.target) then return end
  local r = scene.get(self.target, "Transform").rotation
  scene.set(self.target, "Transform", { rotation = { r.x, (r.y + self.speed * dt) % 360, r.z } })
end

return Spin
