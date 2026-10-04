-- The traveller: walks on the ground plane with a CharacterBody and picks the walk
-- cycle of its child sprite from the direction of travel (the side row is mirrored for left).
-- params: { "speed": 3.2 }
local Hero = {}

function Hero:onStart()
  self.speed = self.params.speed or 3.2 -- walking speed in meters per second
  self.sprite = scene.child(self.id, "HeroSprite")
  self.facing = "down"
  self.clip = "idle_down"
end

function Hero:onUpdate(dt)
  local mx, mz = input.axis("LeftX"), -input.axis("LeftY")
  if input.down("A") or input.down("Left") then mx = mx - 1 end
  if input.down("D") or input.down("Right") then mx = mx + 1 end
  if input.down("W") or input.down("Up") then mz = mz - 1 end
  if input.down("S") or input.down("Down") then mz = mz + 1 end
  if game.get("dialog") then mx, mz = 0, 0 end

  local len = math.sqrt(mx * mx + mz * mz)
  local vx, vz = 0, 0
  if len > 0.01 then
    local speed = self.speed * ((input.down("Shift") or input.down("GamepadX")) and 1.7 or 1)
    local scale = speed / math.max(1, len)
    vx, vz = mx * scale, mz * scale
    if math.abs(mx) > math.abs(mz) then
      self.facing = mx < 0 and "left" or "right"
    else
      self.facing = mz < 0 and "up" or "down"
    end
  end
  self:setVelocity(vx, self:velocity().y, vz)

  local row = (self.facing == "left" or self.facing == "right") and "side" or self.facing
  local clip = (len > 0.01 and "walk_" or "idle_") .. row
  if clip ~= self.clip then
    self.clip = clip
    scene.set(self.sprite, "SpriteAnimation", { clip = clip })
  end
  scene.set(self.sprite, "Sprite", { flipX = self.facing == "left" })
  scene.set(self.sprite, "SpriteAnimation", { speed = input.down("Shift") and 1.5 or 1 })
end

return Hero
