-- Top-down hero (CharacterBody2D, topdown mode). WASD/arrows walk, Space/J shoots
-- a bolt the way the hero faces. Touching a slime hurts (short invulnerability, knockback).
-- params: { speed, accel }
local Player = {}

local function approach(v, target, step)
  if v < target then return math.min(v + step, target) end
  return math.max(v - step, target)
end

function Player:onStart()
  self.speed = self.params.speed or 5
  self.accel = self.params.accel or 45
  self.cooldown = 0
  self.hurtTimer = 0
  self.face = { x = 0, y = -1 }
  self.clip = ""
  self.controls = true
end

function Player:onUpdate(dt)
  local mx, my = 0, 0
  if self.controls then
    mx, my = input.axis("LeftX"), input.axis("LeftY")
    if mx == 0 and my == 0 then
      if input.down("A") or input.down("Left") or input.down("GamepadDPadLeft") then mx = mx - 1 end
      if input.down("D") or input.down("Right") or input.down("GamepadDPadRight") then mx = mx + 1 end
      if input.down("W") or input.down("Up") or input.down("GamepadDPadUp") then my = my + 1 end
      if input.down("S") or input.down("Down") or input.down("GamepadDPadDown") then my = my - 1 end
    end
  end
  local length = math.sqrt(mx * mx + my * my)
  if length > 1 then mx, my = mx / length, my / length end
  if mx ~= 0 or my ~= 0 then self.face = { x = mx, y = my } end

  local v = self:velocity()
  local step = self.accel * dt
  self:setVelocity(approach(v.x, mx * self.speed, step), approach(v.y, my * self.speed, step))

  self.cooldown = math.max(0, self.cooldown - dt)
  if self.controls and self.cooldown <= 0 and (input.pressed("Space") or input.pressed("J") or input.pressed("GamepadA")) then self:shoot() end

  self.hurtTimer = math.max(0, self.hurtTimer - dt)
  if self.hurtTimer <= 0 then
    for _, other in ipairs(self:contacts()) do
      local tag = scene.get(other, "Tag")
      if tag and string.find(tag.tags, "enemy", 1, true) then
        self:hurt(other)
        break
      end
    end
  end
  self:animate(mx, my)
end

function Player:shoot()
  self.cooldown = 0.28
  local p, f = self:position(), self.face
  local len = math.sqrt(f.x * f.x + f.y * f.y)
  local dx, dy = f.x / len, f.y / len
  local bolt = scene.instantiate("prefabs/bolt.prefab.json", { position = { x = p.x + dx * 0.55, y = p.y + dy * 0.55, z = 0.2 } })
  scene.set(bolt, "RigidBody2D", { velocity = { x = dx * 12, y = dy * 12, z = 0 } })
  audio.play("sounds/shoot.wav", { volume = 0.35 })
end

function Player:hurt(enemy)
  if not self.controls then return end
  self.hurtTimer = 1.0
  require("scripts.effects").spawn(self:position(), "hit")
  local p, e = self:position(), scene.get(enemy, "Transform").position
  local dx, dy = p.x - e.x, p.y - e.y
  local len = math.max(0.01, math.sqrt(dx * dx + dy * dy))
  self:setVelocity(dx / len * 9, dy / len * 9)
  audio.play("sounds/hurt.wav", { volume = 0.6 })
  scene.broadcast("onPlayerHurt")
end

function Player:animate(mx, my)
  local f = self.face
  local dir = math.abs(f.x) > math.abs(f.y) and "side" or (f.y > 0 and "up" or "down")
  local clip = dir .. ((mx ~= 0 or my ~= 0) and "_walk" or "_idle")
  if clip ~= self.clip then
    self.clip = clip
    self:set("SpriteAnimation", { clip = clip })
  end
  local blink = self.hurtTimer > 0 and math.floor(self.hurtTimer * 12) % 2 == 0
  self:set("Sprite", { flipX = dir == "side" and f.x < 0, opacity = blink and 0.35 or 1 })
end

-- From the game: stop moving (end screens).
function Player:freeze()
  self.controls = false
  self:setVelocity(0, 0)
end

return Player
