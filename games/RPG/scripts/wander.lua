-- Random wandering NPC: picks a point near its home, walks there over the terrain grid, pauses, repeats.
-- Same 4x3 sheet as the hero (rows: down, right, up; left = right mirrored).
-- params: { "radius": 4, "speed": 1.2, "pauseMin": 1, "pauseMax": 3 }
local Terrain = require("scripts.lib.terrain")

local Wander = {}

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

-- Small LCG so the walk is deterministic per entity.
function Wander:rand()
  self.seed = (self.seed * 1103515245 + 12345) % 2147483648
  return self.seed / 2147483648
end

function Wander:onStart()
  local p = self:position()
  self.x, self.y, self.z = p.x, p.y, p.z
  self.homeX, self.homeZ = p.x, p.z
  self.seed = (math.floor(p.x * 100) * 7919 + math.floor(p.z * 100) * 104729 + 1) % 2147483648
  self.facing = 0
  self.anim = 0
  self.moving = false
  self.wait = self:rand() * 2
  self.tx, self.tz = nil, nil
  self.stuck = 0
end

function Wander:pick()
  local r = self.params.radius or 4
  for _ = 1, 8 do
    local a, d = self:rand() * math.pi * 2, 1 + self:rand() * (r - 1)
    local tx, tz = self.homeX + math.cos(a) * d, self.homeZ + math.sin(a) * d
    if canStand(tx, tz, math.max(0, self.y)) then
      self.tx, self.tz, self.stuck = tx, tz, 0
      return
    end
  end
  self.wait = 0.5
end

function Wander:stop()
  self.tx, self.tz = nil, nil
  local lo, hi = self.params.pauseMin or 1, self.params.pauseMax or 3
  self.wait = lo + self:rand() * math.max(0, hi - lo)
end

function Wander:onUpdate(dt)
  if dt > 0 then
    if not self.tx then
      self.wait = self.wait - dt
      if self.wait <= 0 then self:pick() end
    end
    self.moving = false
    if self.tx then
      local dx, dz = self.tx - self.x, self.tz - self.z
      local dist = math.sqrt(dx * dx + dz * dz)
      if dist < 0.1 then
        self:stop()
      else
        local step = math.min(dist, (self.params.speed or 1.2) * dt)
        local wx, wz = dx / dist, dz / dist
        local gy = math.max(0, self.y)
        local nx, nz = self.x + wx * step, self.z + wz * step
        local moved = false
        -- spawned on an edge the grid calls blocked: walk free until back on standable ground
        local free = not canStand(self.x, self.z, gy)
        if free or canStand(nx, self.z, gy) then self.x = nx; moved = true end
        if free or canStand(self.x, nz, gy) then self.z = nz; moved = true end
        if moved then
          self.moving = true
          self.stuck = 0
          if math.abs(dx) > math.abs(dz) then self.facing = dx < 0 and 1 or 2 else self.facing = dz < 0 and 3 or 0 end
          self.anim = self.anim + dt * 5
        else
          self.stuck = self.stuck + dt
          if self.stuck > 0.3 then self:stop() end
        end
      end
    end
    if not self.moving then self.anim = 0 end
    local target = ground(self.x, self.z)
    if target >= 0 then self.y = self.y + (target - self.y) * math.min(1, dt * 16) end
    self:setPosition(self.x, self.y, self.z)
    local row = ({[0] = 0, [1] = 1, [2] = 1, [3] = 2})[self.facing]
    local col = self.moving and (math.floor(self.anim) % 4) or 0
    self:set("Sprite", {frame = row * 4 + col, flipX = self.facing == 1})
  end
end

return Wander
