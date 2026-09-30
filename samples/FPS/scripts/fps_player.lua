-- First-person player: mouse look, WASD movement, jumping and a hitscan pistol.
-- Lives on the "Player" entity (CharacterBody). Expects children:
--   Head (Camera)          pitch + yaw are applied here
--   Head/Gun, Head/MuzzleFlash
-- params: { speed, sprint, jumpSpeed, sensitivity, magazine, reloadTime, fireDelay }
local Player = {}

local function clamp(v, lo, hi) return math.max(lo, math.min(hi, v)) end

function Player:onStart()
  local p = self.params
  self.speed = p.speed or 5.5
  self.sprint = p.sprint or 8.5
  self.jumpSpeed = p.jumpSpeed or 5.5
  self.sensitivity = p.sensitivity or 0.12   -- degrees per pixel of mouse motion
  self.magazine = p.magazine or 12
  self.reloadTime = p.reloadTime or 1.1
  self.fireDelay = p.fireDelay or 0.14

  self.head = scene.find("Head")
  self.gun = scene.find("Gun")
  self.flash = scene.find("MuzzleFlash")
  self.ammoText = scene.find("AmmoText")
  self.gunRest = scene.get(self.gun, "Transform").position
  local r = scene.get(self.head, "Transform").rotation
  self.pitch, self.yaw = r.x, r.y
  self.eye = scene.get(self.head, "Transform").position.y

  self.ammo = self.magazine
  self.reloading = 0
  self.cooldown = 0
  self.kick = 0
  self.shots, self.hits = 0, 0
  self.enabled = true
  input.lockMouse()
  self:updateAmmo()
end

-- Horizontal forward/right vectors for the current yaw (cameras look down -Z).
function Player:basis()
  local y = math.rad(self.yaw)
  return -math.sin(y), -math.cos(y), math.cos(y), -math.sin(y)
end

-- Unit vector the camera looks along.
function Player:aim()
  local y, p = math.rad(self.yaw), math.rad(self.pitch)
  return { x = -math.sin(y) * math.cos(p), y = math.sin(p), z = -math.cos(y) * math.cos(p) }
end

function Player:onUpdate(dt)
  if not self.enabled then
    self:setVelocity(0, self:velocity().y, 0)
    return
  end

  -- Look: mouse (while captured) or Left/Right arrows.
  local dx, dy = input.mouseDelta()
  self.yaw = self.yaw - dx * self.sensitivity
  self.pitch = clamp(self.pitch - dy * self.sensitivity, -85, 85)
  if input.down("Left") then self.yaw = self.yaw + 120 * dt end
  if input.down("Right") then self.yaw = self.yaw - 120 * dt end
  self.yaw = (self.yaw + 540) % 360 - 180
  scene.set(self.head, "Transform", { rotation = { x = self.pitch, y = self.yaw, z = 0 } })

  -- Move relative to where we look.
  local fx, fz, rx, rz = self:basis()
  local f, s = 0, 0
  if input.down("W") or input.down("Up") then f = f + 1 end
  if input.down("S") or input.down("Down") then f = f - 1 end
  if input.down("D") then s = s + 1 end
  if input.down("A") then s = s - 1 end
  local vx, vz = fx * f + rx * s, fz * f + rz * s
  local len = math.sqrt(vx * vx + vz * vz)
  local speed = input.down("Shift") and self.sprint or self.speed
  if len > 0 then vx, vz = vx / len * speed, vz / len * speed end
  local vy = self:velocity().y
  if self:grounded() and input.pressed("Space") then vy = self.jumpSpeed end
  self:setVelocity(vx, vy, vz)

  -- Weapon.
  self.cooldown = math.max(0, self.cooldown - dt)
  if self.reloading > 0 then
    self.reloading = self.reloading - dt
    if self.reloading <= 0 then
      self.reloading = 0
      self.ammo = self.magazine
      self:updateAmmo()
    end
  elseif input.pressed("R") and self.ammo < self.magazine then
    self:reload()
  elseif input.pressed("MouseLeft") and self.cooldown <= 0 then
    if self.ammo > 0 then self:fire() else
      audio.play("sounds/empty.wav", { volume = 0.5, pitch = 0.7 })
      self:reload()
    end
  end
  self:animateGun(dt)
end

function Player:reload()
  self.reloading = self.reloadTime
  audio.play("sounds/reload.wav", { volume = 0.6, pitch = 0.8 })
  self:updateAmmo()
end

function Player:fire()
  self.ammo = self.ammo - 1
  self.cooldown = self.fireDelay
  self.kick = 1
  self.shots = self.shots + 1
  game.set("shots", self.shots)
  audio.play("sounds/shot.wav", { volume = 0.45, pitch = 1.7 })
  scene.set(self.flash, "MeshRenderer", { visible = true })
  timer.after(0.05, function() scene.set(self.flash, "MeshRenderer", { visible = false }) end)
  self:updateAmmo()

  local pos = self:position()
  local dir = self:aim()
  self.pitch = clamp(self.pitch + 0.8, -85, 85)  -- recoil kicks the view after the shot
  local origin = { x = pos.x + dir.x * 0.6, y = pos.y + self.eye + dir.y * 0.6, z = pos.z + dir.z * 0.6 }
  local hit = physics.raycast(origin, dir, 150)
  if hit and hit.entity == self.id then  -- started inside our own capsule (looking down)
    origin = { x = hit.point.x + dir.x * 0.05, y = hit.point.y + dir.y * 0.05, z = hit.point.z + dir.z * 0.05 }
    hit = physics.raycast(origin, dir, 150)
  end
  if not hit then return end

  self:spark(hit.point)
  local tag = scene.get(hit.entity, "Tag")
  if tag and string.find(tag.tags, "target", 1, true) then
    self.hits = self.hits + 1
    game.set("hits", self.hits)
    scene.send(hit.entity, "onShot", dir)
  elseif scene.has(hit.entity, "RigidBody") then
    physics.addImpulse(hit.entity, { x = dir.x * 4, y = dir.y * 4 + 1, z = dir.z * 4 })
  end
end

-- Short-lived impact marker.
function Player:spark(p)
  local id = scene.create("Spark", {
    Transform = { position = { p.x, p.y, p.z }, scale = { 0.12, 0.12, 0.12 } },
    MeshRenderer = { mesh = "sphere", color = { 1, 0.85, 0.35 }, unlit = true, castShadows = false },
  })
  timer.after(0.12, function() if scene.exists(id) then scene.destroy(id) end end)
end

function Player:animateGun(dt)
  self.kick = math.max(0, self.kick - dt * 9)
  local dip = 0
  if self.reloading > 0 then
    local t = 1 - self.reloading / self.reloadTime       -- 0 -> 1
    dip = math.sin(t * math.pi) * 0.18
  end
  local r = self.gunRest
  scene.set(self.gun, "Transform", {
    position = { x = r.x, y = r.y - dip, z = r.z + self.kick * 0.07 },
    rotation = { x = self.kick * 8 - dip * 120, y = 0, z = 0 },
  })
end

function Player:updateAmmo()
  local text = self.reloading > 0 and "RELOADING..." or string.format("AMMO  %d / %d", self.ammo, self.magazine)
  scene.set(self.ammoText, "UIText", { text = text, color = self.ammo == 0 and { 1, 0.4, 0.35 } or { 1, 1, 1 } })
end

-- Sent by the game manager when all targets are down.
function Player:onRoundOver()
  self.enabled = false
  input.lockMouse(false)
end

return Player
