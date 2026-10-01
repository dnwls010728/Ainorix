-- 2D platformer hero. Needs Sprite + SpriteAnimation + CharacterBody (plane2D).
-- Feel: acceleration, coyote time, jump buffering, variable jump height.
-- params: { speed, jumpSpeed, accel, airAccel, coyote, buffer }
local Player = {}

local function approach(v, target, step)
  if v < target then return math.min(v + step, target) end
  return math.max(v - step, target)
end

function Player:onStart()
  local p = self.params
  self.speed = p.speed or 7
  self.jumpSpeed = p.jumpSpeed or 13.5
  self.accel = p.accel or 70
  self.airAccel = p.airAccel or 40
  self.coyoteTime = p.coyote or 0.1
  self.bufferTime = p.buffer or 0.12
  self.level = scene.find("Level")
  self.coyote, self.buffer = 0, 0
  self.facing = 1
  self.hurtTimer = 0
  self.lastVy = 0
  self.controls = true
end

function Player:jumpPressed()
  return input.pressed("Space") or input.pressed("W") or input.pressed("Up")
end

function Player:jumpHeld()
  return input.down("Space") or input.down("W") or input.down("Up")
end

function Player:onUpdate(dt)
  local v = self:velocity()
  local grounded = self:grounded()

  -- Bumped a block from below: the head stopped a rising jump.
  if self.lastVy > 1 and v.y <= 0.01 and not grounded then self:headBump() end
  self.lastVy = v.y

  local move = 0
  if self.controls and self.hurtTimer <= 0 then
    if input.down("Left") or input.down("A") then move = move - 1 end
    if input.down("Right") or input.down("D") then move = move + 1 end
  end
  local accel = grounded and self.accel or self.airAccel
  local vx = approach(v.x, move * self.speed, accel * dt)

  self.coyote = grounded and self.coyoteTime or math.max(0, self.coyote - dt)
  self.buffer = (self.controls and self:jumpPressed()) and self.bufferTime or math.max(0, self.buffer - dt)
  local vy = v.y
  if self.buffer > 0 and self.coyote > 0 then
    vy = self.jumpSpeed
    self.buffer, self.coyote = 0, 0
    audio.play("sounds/jump.wav", { volume = 0.5 })
  elseif vy > 0 and not self:jumpHeld() then
    vy = vy * 0.55  -- short hop when the button is released early
  end
  self:setVelocity(vx, vy, 0)

  if move ~= 0 then self.facing = move end
  self:animate(grounded, vx, vy, dt)
  self:checkHazards()
end

function Player:animate(grounded, vx, vy, dt)
  local clip
  if self.hurtTimer > 0 then
    self.hurtTimer = self.hurtTimer - dt
    clip = "hurt"
  elseif not grounded then
    clip = vy > 0 and "jump" or "fall"
  elseif math.abs(vx) > 0.5 then
    clip = "run"
  else
    clip = "idle"
  end
  self:set("Sprite", { flipX = self.facing < 0 })
  if self:get("SpriteAnimation").clip ~= clip then self:set("SpriteAnimation", { clip = clip }) end
end

-- Spikes below the feet, or falling out of the level.
function Player:checkHazards()
  local p = self:position()
  if p.y < -16 then return self:die() end
  if not self.level then return end
  local col, row = tilemap.cellAt(self.level, { x = p.x, y = p.y - 0.3, z = 0 })
  if tilemap.get(self.level, col, row) == "^" then self:die() end
end

-- "?" block above the head turns into a used block and pops a coin.
function Player:headBump()
  if not self.level then return end
  local p = self:position()
  for _, dx in ipairs({ 0, -0.25, 0.25 }) do
    local col, row = tilemap.cellAt(self.level, { x = p.x + dx, y = p.y + 0.75, z = 0 })
    local tile = tilemap.get(self.level, col, row)
    if tile == "?" then
      tilemap.set(self.level, col, row, "u")
      audio.play("sounds/coin.wav")
      scene.broadcast("onCoin", tilemap.cellCenter(self.level, col, row - 1))
      return
    elseif tile == "B" or tile == "u" then
      audio.play("sounds/bump.wav", { volume = 0.4 })
      return
    end
  end
end

-- Called by slimes: stomped from above (true) or touched from the side.
function Player:bounce()
  local v = self:velocity()
  self:setVelocity(v.x, self.jumpSpeed * 0.7, 0)
end

function Player:die()
  if not self.controls then return end
  self.controls = false
  self.hurtTimer = 0.6
  require("scripts.effects").spawn(self:position(), "hit")
  audio.play("sounds/hurt.wav", { volume = 0.6 })
  scene.broadcast("onPlayerDied")
end

-- Sent by game.lua after a death (or at the start of a life).
function Player:respawn(pos)
  self:setPosition(pos.x, pos.y, 0)
  self:setVelocity(0, 0, 0)
  self.controls = true
  self.hurtTimer = 0
end

function Player:freeze()
  self.controls = false
end

return Player
