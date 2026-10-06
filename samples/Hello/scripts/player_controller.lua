-- Simple player movement: W/A/S/D or arrow keys move on the XZ plane, Space jumps.
-- With a CharacterBody the physics engine moves the entity; without one it lands at y = half its height.
-- params: { "speed": 4, "jumpSpeed": 5, "gravity": 12 }
local controls = require("oe.input")
local vectors = require("oe.math")
local actions = {
  move = { kind = "vector", left = { "A", "Left" }, right = { "D", "Right" },
           up = { "W", "Up" }, down = { "S", "Down" } },
  jump = { kind = "button", keys = { "Space" } },
}
local Player = {}

function Player:onStart()
  self.speed = self.params.speed or 4
  self.jumpSpeed = self.params.jumpSpeed or 5
  self.gravity = self.params.gravity or 12
  self.vy = 0
  self.source = controls.localSource() -- this template is offline, not player-stream input
end

function Player:onUpdate(dt)
  local t = self:get("Transform")
  local pos = t.position

  local move = controls.read(self.source, actions, "move")
  local wish = vectors.normalize({ x = move.x, z = -move.y }) -- up on input maps to forward (-Z)
  local jump = controls.read(self.source, actions, "jump")

  -- With a CharacterBody, express intent as velocity and let physics move us
  -- (walls, slopes, gravity and landing are handled by the engine).
  if self:has("CharacterBody") then
    local vx, vz = wish.x * self.speed, wish.z * self.speed
    local vy = self:velocity().y
    if self:grounded() and (jump.pressed or jump.held) then vy = self.jumpSpeed end
    self:setVelocity(vx, vy, vz)
    return
  end

  if wish.x ~= 0 or wish.z ~= 0 then
    pos.x = pos.x + wish.x * self.speed * dt
    pos.z = pos.z + wish.z * self.speed * dt
  end

  local groundY = 0.5 * t.scale.y
  local grounded = pos.y <= groundY + 1e-4
  if grounded and (jump.pressed or jump.held) and self.vy <= 0 then
    self.vy = self.jumpSpeed
    grounded = false
  end
  if not grounded or self.vy > 0 then
    self.vy = self.vy - self.gravity * dt
    pos.y = pos.y + self.vy * dt
    if pos.y <= groundY then
      pos.y = groundY
      self.vy = 0
    end
  end

  self:set("Transform", { position = pos })
end

return Player
