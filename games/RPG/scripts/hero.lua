-- The hero: walks over the terrain grid relative to the camera, steps up/down one block at most,
-- animates the 4x3 sheet (rows: down, right, up; left = right mirrored) and kicks up dust.
-- Shares its state with scripts/game.lua through game.set("hero", ...); reads the camera yaw from it.
-- params: { "walk": 3.6, "run": 6.2 }
local Terrain = require("scripts.lib.terrain")

local Hero = {}

local RADIUS = 0.26

local function ground(x, z)
  return Terrain.height(math.floor(x + 0.5), math.floor(z + 0.5))
end

local function canStand(x, z, fromY)
  for _, c in ipairs({{-1, -1}, {1, -1}, {-1, 1}, {1, 1}}) do
    local h = ground(x + c[1] * RADIUS, z + c[2] * RADIUS)
    if h < 0 or math.abs(h - fromY) > 1 then return false end
  end
  return true
end

function Hero:onStart()
  local p = self:position()
  self.x, self.y, self.z = p.x, p.y, p.z
  self.vx, self.vz = 0, 0
  self.anim = 0
  self.facing = 0 -- 0 down, 1 left, 2 right, 3 up
  self.dustTimer = 0
  self.dust = scene.find("Dust")
  self:publish(false, false)
end

function Hero:publish(moving, running)
  game.set("hero", {x = self.x, y = self.y, z = self.z, vx = self.vx, vz = self.vz, moving = moving, running = running})
end

function Hero:onUpdate(dt)
  local locked = game.get("locked") == true
  local yaw = game.get("camYaw") or 0
  local key = function(a, b) return input.down(a) or input.down(b) end
  local f, s = 0, 0
  if not locked then
    f = (key("W", "Up") and 1 or 0) - (key("S", "Down") and 1 or 0)
    s = (key("D", "Right") and 1 or 0) - (key("A", "Left") and 1 or 0)
    local ax, ay = input.axis("LeftX"), input.axis("LeftY")
    if math.abs(ax) + math.abs(ay) > 0 then f, s = ay, ax end
  end
  local moving = f ~= 0 or s ~= 0
  local running = false
  local vx, vz = 0, 0
  if moving and dt > 0 then
    local len = math.sqrt(f * f + s * s)
    local sx, sf = s / len, f / len
    local c, sn = math.cos(yaw), math.sin(yaw)
    local wx = sx * c - sf * sn
    local wz = -sx * sn - sf * c
    running = input.down("Shift") or input.down("GamepadX")
    local speed = (running and (self.params.run or 6.2) or (self.params.walk or 3.6)) * math.min(1, len)
    local gy = math.max(0, self.y)
    local nx, nz = self.x + wx * speed * dt, self.z + wz * speed * dt
    if canStand(nx, self.z, gy) then self.x = nx; vx = wx * speed end
    if canStand(self.x, nz, gy) then self.z = nz; vz = wz * speed end
    if math.abs(sx) > math.abs(sf) then self.facing = sx < 0 and 1 or 2 else self.facing = sf > 0 and 3 or 0 end
    self.anim = self.anim + dt * (running and 11 or 7.5)
  else
    self.anim = 0
  end
  local k = math.min(1, dt * 6)
  self.vx = self.vx + (vx - self.vx) * k
  self.vz = self.vz + (vz - self.vz) * k
  local target = ground(self.x, self.z)
  if target >= 0 then self.y = self.y + (target - self.y) * math.min(1, dt * 16) end
  self:setPosition(self.x, self.y, self.z)

  local row = ({[0] = 0, [1] = 1, [2] = 1, [3] = 2})[self.facing]
  local col = moving and (math.floor(self.anim) % 4) or 0
  self:set("Sprite", {frame = row * 4 + col, flipX = self.facing == 1})

  if moving and not locked and self.dust then
    self.dustTimer = self.dustTimer - dt
    if self.dustTimer <= 0 then
      self.dustTimer = running and 0.07 or 0.17
      scene.set(self.dust, "Transform", {position = {self.x, self.y + 0.08, self.z}})
      vfx.burst(self.dust, 1)
    end
  end
  self:publish(moving, running)
end

return Hero
