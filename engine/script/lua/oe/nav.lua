-- Walks an entity along navigation mesh paths. The engine finds the path (nav.path); following it is
-- ordinary script work, kept here so every game does not write it again.
local M = {}
local Agent = {}
Agent.__index = Agent

local function number(value, fallback, name)
    if value == nil then return fallback end
    assert(type(value) == "number" and value == value and math.abs(value) < math.huge, name .. " must be a finite number")
    return value
end

local function flat(ax, az, bx, bz)
    local dx, dz = bx - ax, bz - az
    return math.sqrt(dx * dx + dz * dz), dx, dz
end

-- agent = M.agent(id, {speed, stoppingDistance, turnSpeed, faceOffset, repath})
--   speed             meters per second (default 3.5)
--   stoppingDistance  how close to the goal counts as arrived (default 0.1)
--   turnSpeed         degrees per second the entity turns toward its way; 0 keeps its rotation (default 720)
--   faceOffset        degrees added to the yaw: 180 for models that look along +Z (default 0)
--   repath            seconds between looking for the path again while moving; 0 = never (default 0.5)
function M.agent(id, options)
    assert(math.type(id) == "integer" and scene.exists(id), "agent needs an entity id")
    assert(options == nil or type(options) == "table", "agent options must be a table")
    options = options or {}
    local agent = setmetatable({}, Agent)
    agent.id = id
    agent.speed = number(options.speed, 3.5, "speed")
    agent.stoppingDistance = number(options.stoppingDistance, 0.1, "stoppingDistance")
    agent.turnSpeed = number(options.turnSpeed, 720, "turnSpeed")
    agent.faceOffset = number(options.faceOffset, 0, "faceOffset")
    agent.repath = number(options.repath, 0.5, "repath")
    agent.moving = false   -- on its way
    agent.partial = false  -- the goal cannot be reached: it walks as close as the mesh allows
    return agent
end

-- Looks for the path from where the entity stands. False (and no movement) when there is none.
function Agent:moveTo(target)
    assert(type(target) == "table", "moveTo needs a {x,y,z} point")
    local position = scene.get(self.id, "Transform").position
    local path, partial = nav.path(position, target)
    if not path then
        self:stop()
        return false
    end
    self.target = { x = target.x or target[1] or 0, y = target.y or target[2] or 0, z = target.z or target[3] or 0 }
    self.path, self.corner, self.partial, self.moving, self.sinceRepath = path, 2, partial, true, 0
    -- A Transform-driven entity keeps its height above the mesh (the origin of a capsule is its middle).
    self.lift = position.y - path[1].y
    return true
end

function Agent:stop()
    self.path, self.target, self.moving = nil, nil, false
    if scene.has(self.id, "CharacterBody") then
        local body = scene.get(self.id, "CharacterBody")
        scene.set(self.id, "CharacterBody", { velocity = { x = 0, y = body.velocity.y, z = 0 } })
    end
end

-- Meters left along the path; 0 when not moving.
function Agent:remaining()
    if not self.path then return 0 end
    local position = scene.get(self.id, "Transform").position
    local total, x, z = 0, position.x, position.z
    for i = self.corner, #self.path do
        local corner = self.path[i]
        total = total + flat(x, z, corner.x, corner.z)
        x, z = corner.x, corner.z
    end
    return total
end

-- Call every onUpdate. Returns true while the entity is on its way.
function Agent:update(dt)
    number(dt, 0, "dt")
    if not self.path then return false end
    if self.repath > 0 then
        self.sinceRepath = self.sinceRepath + dt
        if self.sinceRepath >= self.repath and not self:moveTo(self.target) then return false end
    end
    local transform = scene.get(self.id, "Transform")
    local position = transform.position
    local path = self.path
    local distance, dx, dz
    while true do
        local corner = path[self.corner]
        if not corner then
            self:stop()
            return false
        end
        distance, dx, dz = flat(position.x, position.z, corner.x, corner.z)
        -- Corners on the way are passed within a step; only the last one uses the stopping distance.
        local reach = self.corner == #path and self.stoppingDistance or math.max(0.05, self.speed * dt)
        if distance > reach then break end
        self.corner = self.corner + 1
    end
    local step = math.min(self.speed * dt, distance)
    local nx, nz = dx / distance, dz / distance
    if self.turnSpeed > 0 then
        local yaw = math.deg(math.atan(-nx, -nz)) + self.faceOffset
        local delta = (yaw - transform.rotation.y + 180) % 360 - 180
        local turn = self.turnSpeed * dt
        local turned = transform.rotation.y + math.max(-turn, math.min(turn, delta))
        transform.rotation.y = (turned + 180) % 360 - 180  -- kept in -180..180 however often it turns
    end
    if scene.has(self.id, "CharacterBody") then
        -- Physics moves it: walls, steps and gravity stay the engine's. Slow into the last corner.
        local speed = dt > 0 and step / dt or 0
        local body = scene.get(self.id, "CharacterBody")
        scene.set(self.id, "CharacterBody", { velocity = { x = nx * speed, y = body.velocity.y, z = nz * speed } })
        scene.set(self.id, "Transform", { rotation = transform.rotation })
    else
        position.x, position.z = position.x + nx * step, position.z + nz * step
        local ground = nav.nearest({ x = position.x, y = position.y - self.lift, z = position.z })
        if ground then position.y = ground.y + self.lift end
        scene.set(self.id, "Transform", { position = position, rotation = transform.rotation })
    end
    return true
end

return M
