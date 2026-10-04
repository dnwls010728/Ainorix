-- Third-person movement: W/S run along the facing, A/D strafe, Space jumps. The facing is
-- the look direction camera.lua stores from the mouse (the LookX network axis).
-- Only declared network inputs are read, so the server and the predicting owner agree.
-- params: { "speed": 5, "jumpSpeed": 6, "gravity": 16 }
local Player = {}
local colors = {{0.22, 0.65, 1}, {1, 0.35, 0.3}, {0.3, 0.9, 0.5}, {0.8, 0.45, 1}}
-- Keep in sync with the Pillar entities in scenes/main.scene.json (x, z, blocking radius).
local pillars = {{-5, -5, 1.25}, {5, -5, 1.25}, {-5, 5, 1.25}, {5, 5, 1.25}, {0, 0, 1.25}}
local ground, limit = 0.6, 11.5

function Player:onStart()
    self.speed = self.params.speed or 5
    self.jumpSpeed = self.params.jumpSpeed or 6
    self.gravity = self.params.gravity or 16
    self.vy = 0
    local number = self:get("NetPlayer").player
    -- Everybody starts on the south side looking at the arena centre (yaw 180 faces -Z).
    self:set("Transform", {position = {x = (number - 1) * 2 - 3, y = ground, z = 9},
                           rotation = {x = 0, y = 180, z = 0}})
    local c = colors[(number - 1) % #colors + 1]
    self:set("MeshRenderer", {color = {r = c[1], g = c[2], b = c[3]}})
end

function Player:onUpdate(dt)
    local state = net.state()
    if state.state ~= "idle" and (not state.sync or state.sync.state ~= "running") then return end
    local number = self:get("NetPlayer").player
    local p = state.state == "idle" and input or input.player(number)
    local t = self:get("Transform")
    local forward = (p.down("W") and 1 or 0) - (p.down("S") and 1 or 0)
    local right = (p.down("D") and 1 or 0) - (p.down("A") and 1 or 0)
    local length = math.sqrt(forward * forward + right * right)
    if length > 1 then forward, right = forward / length, right / length end
    -- Look is -1..1 for -180..180 degrees around the starting direction.
    local yaw = (180 + p.look() * 180) % 360
    local sin, cos = math.sin(math.rad(yaw)), math.cos(math.rad(yaw))
    local x = t.position.x + (sin * forward - cos * right) * self.speed * dt
    local z = t.position.z + (cos * forward + sin * right) * self.speed * dt
    for _, pillar in ipairs(pillars) do
        local dx, dz = x - pillar[1], z - pillar[2]
        local d = math.sqrt(dx * dx + dz * dz)
        if d < pillar[3] and d > 1e-4 then
            x, z = pillar[1] + dx / d * pillar[3], pillar[2] + dz / d * pillar[3]
        end
    end
    local y = t.position.y
    if y <= ground and p.down("Space") then self.vy = self.jumpSpeed end
    if y > ground or self.vy > 0 then
        self.vy = self.vy - self.gravity * dt
        y = y + self.vy * dt
        if y <= ground then y, self.vy = ground, 0 end
    end
    self:set("Transform", {
        position = {x = math.max(-limit, math.min(limit, x)), y = y, z = math.max(-limit, math.min(limit, z))},
        rotation = {x = 0, y = yaw, z = 0}})
end
return Player
