-- Simple player movement: W/A/S/D or arrow keys move on the XZ plane, Space jumps.
-- With a CharacterBody the physics engine moves the entity; without one it lands at y = half its height.
-- params: { "speed": 4, "jumpSpeed": 5, "gravity": 12 }
local Player = {}

function Player:onStart()
  self.speed = self.params.speed or 4
  self.jumpSpeed = self.params.jumpSpeed or 5
  self.gravity = self.params.gravity or 12
  self.vy = 0
end

function Player:onUpdate(dt)
  local t = self:get("Transform")
  local pos = t.position

  local mx, mz = 0, 0
  if input.down("W") or input.down("Up") then mz = mz - 1 end
  if input.down("S") or input.down("Down") then mz = mz + 1 end
  if input.down("A") or input.down("Left") then mx = mx - 1 end
  if input.down("D") or input.down("Right") then mx = mx + 1 end
  local len = math.sqrt(mx * mx + mz * mz)

  -- With a CharacterBody, express intent as velocity and let physics move us
  -- (walls, slopes, gravity and landing are handled by the engine).
  if self:has("CharacterBody") then
    local vx, vz = 0, 0
    if len > 0 then
      vx = mx / len * self.speed
      vz = mz / len * self.speed
    end
    local vy = self:velocity().y
    if self:grounded() and (input.pressed("Space") or input.down("Space")) then vy = self.jumpSpeed end
    self:setVelocity(vx, vy, vz)
    return
  end

  if len > 0 then
    pos.x = pos.x + mx / len * self.speed * dt
    pos.z = pos.z + mz / len * self.speed * dt
  end

  local groundY = 0.5 * t.scale.y
  local grounded = pos.y <= groundY + 1e-4
  if grounded and (input.pressed("Space") or input.down("Space")) and self.vy <= 0 then
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
