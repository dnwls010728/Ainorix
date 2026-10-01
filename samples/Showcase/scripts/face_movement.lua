-- Turns this entity (a model under a moving parent) to face the direction the
-- parent's CharacterBody is walking. params: { "turnSpeed": 10, "yawOffset": 0 }
local Face = {}

function Face:onStart()
  self.body = scene.parent(self.id)
  self.yaw = self:get("Transform").rotation.y
  self:play("Survey")
end

function Face:onUpdate(dt)
  if not self.body then return end
  local v = scene.get(self.body, "CharacterBody")
  if not v then return end
  v = v.velocity
  local moving = v.x * v.x + v.z * v.z >= 0.01
  local clip = moving and (input.down("Shift") and "Run" or "Walk") or "Survey"
  if self:get("Animator").clip ~= clip then self:play(clip) end
  if not moving then return end
  -- Models face +Z; yaw 0 means looking down +Z.
  local target = math.deg(math.atan(v.x, v.z)) + (self.params.yawOffset or 0)
  local diff = (target - self.yaw + 540) % 360 - 180
  local k = math.min(1, (self.params.turnSpeed or 10) * dt)
  self.yaw = self.yaw + diff * k
  self:set("Transform", { rotation = { x = 0, y = self.yaw, z = 0 } })
end

return Face
