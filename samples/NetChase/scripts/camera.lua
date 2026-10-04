-- Third-person camera: stays behind the local player and looks over its head.
-- Unreplicated, so every peer runs its own copy for its own player. It also owns mouse
-- look: the mouse is captured when you start moving (Escape frees it) and turns the
-- player. The turn is stored with input.setLook, which survives prediction replays and
-- reaches the server through the declared LookX network axis; player.lua reads it back.
-- params: { "distance": 5.5, "height": 2.6, "lookHeight": 1, "sensitivity": 0.2, "mouseLook": true }
local Camera = {}

local function avatar(number)
    for _, id in ipairs(scene.all("NetPlayer")) do
        if scene.get(id, "NetPlayer").player == number then return id end
    end
end

function Camera:onStart()
    self.captured = false
    input.setLook(0, 0)
end

-- Capture on the first move key or when a match starts. Offline the lobby buttons stay
-- clickable until then, and a capture we did not ask for (the platform re-locks on a
-- click after Escape) is released again so the buttons can be reached.
function Camera:capture(running)
    local locked = input.mouseLocked()
    if self.captured and not locked then
        self.captured = false
    elseif locked and not self.captured then
        if running then self.captured = true else input.lockMouse(false) end
    end
    local moved = input.pressed("W") or input.pressed("A") or input.pressed("S") or input.pressed("D")
    if not self.captured and (moved or (running and not self.wasRunning)) then
        self.captured = true
        input.lockMouse(true)
    end
    self.wasRunning = running
end

function Camera:onUpdate(dt)
    local number = net.localPlayer()
    local id = avatar(number == 0 and 1 or number)
    if not id then return end -- lobby or dedicated server: keep the overview
    local state = net.state()
    if self.params.mouseLook ~= false then self:capture(state.sync ~= nil and state.sync.state == "running") end
    local look = input.look()
    if input.mouseLocked() then
        -- look is -1..1 for -180..180 degrees; moving the mouse right turns right.
        local dx = input.mouseDelta()
        look = (look - dx * (self.params.sensitivity or 0.2) / 180 + 1) % 2 - 1
        input.setLook(look, 0)
    end
    local t = scene.get(id, "Transform")
    local distance = self.params.distance or 5.5
    local height = self.params.height or 2.6
    local lookHeight = self.params.lookHeight or 1
    local yaw = 180 + look * 180
    local rad = math.rad(yaw)
    -- The camera looks down its -Z, the player walks along its +Z: hence yaw + 180.
    self:set("Transform", {
        position = {x = t.position.x - math.sin(rad) * distance, y = t.position.y + height,
                    z = t.position.z - math.cos(rad) * distance},
        rotation = {x = -math.deg(math.atan(height - lookHeight, distance)), y = yaw + 180, z = 0}})
end
return Camera
