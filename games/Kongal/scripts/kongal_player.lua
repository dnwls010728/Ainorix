-- Kongal: walks on the XZ plane relative to the camera (W/A/S/D or arrows, left stick), Shift runs,
-- Q/E orbit the camera. Moves through the CharacterBody on this entity; the child "Kongal" holds the
-- model (faces +Z), turns toward the walking direction and blends Idle <-> Walk.
-- params: { "walk": 1.4, "run": 2.6, "turnSpeed": 10, "orbitSpeed": 90, "camDistance": 3.4, "camHeight": 2.0 }
local Player = {}

local function shortestAngle(from, to)
  return (to - from + 180) % 360 - 180
end

function Player:onStart()
  self.walk = self.params.walk or 1.4
  self.run = self.params.run or 2.6
  self.turnSpeed = self.params.turnSpeed or 10      -- how fast the model turns toward the walk direction
  self.orbitSpeed = self.params.orbitSpeed or 90    -- degrees per second for Q/E
  self.camDistance = self.params.camDistance or 3.4
  self.camHeight = self.params.camHeight or 2.0
  self.model = scene.child(self.id, "Kongal")
  self.camera = scene.find("Main Camera")
  self.camYaw = 0
  self.facing = 0
  self.clip = nil
  self:placeCamera()
  self:setClip("Idle", 1)
end

function Player:placeCamera()
  if not self.camera then return end
  local r = math.rad(self.camYaw)
  scene.set(self.camera, "CameraFollow", {
    offset = {math.sin(r) * self.camDistance, self.camHeight, math.cos(r) * self.camDistance}})
end

function Player:setClip(name, speed)
  if not self.model then return end
  if self.clip ~= name then
    animation.play(self.model, name, {blend = 0.2, loop = true, restart = false, speed = speed})
    self.clip = name
  else
    scene.set(self.model, "Animator", {speed = speed})
  end
end

function Player:onUpdate(dt)
  local key = function(a, b) return input.down(a) or input.down(b) end
  local orbit = (input.down("E") and 1 or 0) - (input.down("Q") and 1 or 0)
  if orbit ~= 0 then
    self.camYaw = self.camYaw + orbit * self.orbitSpeed * dt
    self:placeCamera()
  end

  local f = (key("W", "Up") and 1 or 0) - (key("S", "Down") and 1 or 0)
  local s = (key("D", "Right") and 1 or 0) - (key("A", "Left") and 1 or 0)
  local ax, ay = input.axis("LeftX"), input.axis("LeftY")
  if math.abs(ax) + math.abs(ay) > 0 then f, s = ay, ax end

  local vy = self:velocity().y
  local len = math.sqrt(f * f + s * s)
  if len < 0.01 then
    self:setVelocity(0, vy, 0)
    self:setClip("Idle", 1)
    return
  end

  -- camera-relative direction: forward points from the camera to the player
  local r = math.rad(self.camYaw)
  local fx, fz = -math.sin(r), -math.cos(r)
  local rx, rz = math.cos(r), -math.sin(r)
  local k = math.min(1, len) / len
  local dx, dz = (fx * f + rx * s) * k, (fz * f + rz * s) * k
  local running = input.down("Shift") or input.down("GamepadX")
  local speed = running and self.run or self.walk
  self:setVelocity(dx * speed, vy, dz * speed)

  local target = math.deg(math.atan(dx, dz))
  self.facing = self.facing + shortestAngle(self.facing, target) * math.min(1, dt * self.turnSpeed)
  if self.model then scene.set(self.model, "Transform", {rotation = {y = self.facing}}) end
  -- the clip's stride is tiny, so cadence follows speed by eye: ~1.5x at walk, ~2.9x at run
  self:setClip("Walk", math.max(0.6, speed * math.min(1, len) / 0.9))
end

return Player
