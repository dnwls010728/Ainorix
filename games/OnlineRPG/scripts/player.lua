-- A player's avatar: walks on the flat world from the declared network inputs, so the server and
-- the predicting owner compute the same position. It also writes what the avatar is doing and the
-- player's name into its replicated Tag ("walk_left,Guest0001"). Showing that (walk cycle, name
-- plate, speech bubble) is hud.lua's job on every peer: this script does not run for other
-- players' avatars on a client. The name is the server's to say (a client's join name is its entry
-- ticket): the predicting owner keeps whatever name the replicated Tag carries.
-- params: { "speed": 3.2 }
local world = require("scripts.lib.world")
local session = require("scripts.lib.session")
local Player = {}

function Player:onStart()
  self.speed = self.params.speed or 3.2 -- walking speed in world units per second
  local number = self:get("NetPlayer").player
  local spawn = world.spawns[(number - 1) % #world.spawns + 1]
  self:set("Transform", { position = { x = spawn[1], y = 0, z = spawn[2] } })
  self.facing = "down"
end

function Player:show(action)
  local current = self:get("Tag").tags
  local name
  if session.authority() then
    -- What the gate learned from the channel server, else the join name; the tag separates with commas.
    local number = self:get("NetPlayer").player
    name = (session.names[number] or net.name(number) or ("P" .. number)):gsub("[,%c]", "")
  else
    name = current:match(",(.*)$") or ""
  end
  local tags = action .. "_" .. self.facing .. "," .. name
  if current ~= tags then self:set("Tag", { tags = tags }) end
end

-- Where the player wants to go on the ground: stick plus WASD/arrows, x to the right, z toward the camera.
local function wish(keys)
  local mx, mz = keys.axis("LeftX"), -keys.axis("LeftY")
  if keys.down("A") or keys.down("Left") then mx = mx - 1 end
  if keys.down("D") or keys.down("Right") then mx = mx + 1 end
  if keys.down("W") or keys.down("Up") then mz = mz - 1 end
  if keys.down("S") or keys.down("Down") then mz = mz + 1 end
  return mx, mz
end

function Player:onUpdate(dt)
  if not session.offline() and not session.running() then return end
  local keys = session.input(self:get("NetPlayer").player)
  local mx, mz = wish(keys)
  local length = math.sqrt(mx * mx + mz * mz)
  if length < 0.01 then
    self:show("idle")
    return
  end
  if math.abs(mx) > math.abs(mz) then self.facing = mx < 0 and "left" or "right"
  else self.facing = mz < 0 and "up" or "down" end
  self:show("walk")
  local step = self.speed * (keys.down("Shift") and 1.7 or 1) * dt / math.max(1, length)
  local position = self:get("Transform").position
  local x, z = world.move(position.x, position.z, mx * step, mz * step, 0.3)
  self:set("Transform", { position = { x = x, y = 0, z = z } })
end

return Player
