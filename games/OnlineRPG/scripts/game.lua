-- Rules, decided only where session.authority() is true (a game server, a listen host, or this
-- process offline): items on the ground, picking them up (E), bags, dropping from a bag, and chat.
-- Clients ask with messages ("chat", "drop") and are told the results ("say", "inv", "notice");
-- every request is checked here, nothing a client claims is trusted. Who a player is and whether
-- it may enter is the gate's business (lib/gate.lua asks the channel server).
local session = require("scripts.lib.session")
local items = require("scripts.lib.items")
local world = require("scripts.lib.world")
local space = require("scripts.lib.space")
local gate = require("scripts.lib.gate")
local Game = {}

local CHAT_LENGTH = 80   -- characters
local CHAT_INTERVAL = 30 -- frames between two messages of one player

function Game:onStart()
  self.bags = {}     -- player number -> array of { key =, count = }
  self.ground = {}   -- item entity id -> key
  self.lastChat = {} -- player number -> frame of the last message
  self.names = {}    -- player number -> name, for players whose avatar is in the world
  self.spawned = false
  gate.onAdmitted = function(number, _, name) session.names[number] = name end
  session.on("chat", function(text) self:chat(session.sender(), text) end)
  session.on("drop", function(slot) self:drop(session.sender(), slot) end)
end

-- A player reached the lobby of a running world: the gate decides whether it gets in.
function Game:onPlayerJoined(number)
  if not session.authority() or number == net.localPlayer() then return end
  gate.joined(number)
end

function Game:onPlayerLeft(number)
  if not session.authority() then return end
  gate.left(number)
  session.names[number] = nil
  if self.names[number] then session.send("all", "notice", self.names[number] .. " left.") end
  self.bags[number] = nil
  self.lastChat[number] = nil
  self.names[number] = nil
end

function Game:place(key, x, z)
  local id = scene.instantiate(items.prefab(key), { position = { x = x, y = 0, z = z } })
  self.ground[id] = key
  return id
end

function Game:groundCount()
  local count = 0
  for _ in pairs(self.ground) do count = count + 1 end
  return count
end

-- Adds one item to a bag; false when it does not fit.
function Game:give(number, key)
  local bag = self.bags[number] or {}
  self.bags[number] = bag
  for _, slot in ipairs(bag) do
    if slot.key == key and slot.count < items.stack then
      slot.count = slot.count + 1
      return true
    end
  end
  if #bag >= items.slots then return false end
  bag[#bag + 1] = { key = key, count = 1 }
  return true
end

function Game:sendBag(number)
  session.send(number, "inv", self.bags[number] or {})
  game.set("bag" .. number, self.bags[number] or {})
end

-- The ground item nearest to a position and within reach of it, or nil.
function Game:nearestItem(at)
  local best, bestDistance
  for _, id in ipairs(scene.withTag("item")) do -- id order: ties go to the lowest id
    if self.ground[id] then
      local d = space.distance2(space.at(id), at)
      if d <= items.reach ^ 2 and (not best or d < bestDistance) then best, bestDistance = id, d end
    end
  end
  return best
end

-- E: the nearest item within reach of a player's avatar goes into the bag.
function Game:pickup(number, avatar)
  local item = self:nearestItem(space.at(avatar))
  if not item then return end
  local key = self.ground[item]
  if not self:give(number, key) then
    session.send(number, "notice", "Your bag is full.")
    return
  end
  self.ground[item] = nil
  scene.destroy(item)
  self:sendBag(number)
  session.send(number, "notice", "Picked up " .. items.names[key] .. ".")
end

-- "drop" request: one item of a bag slot goes back on the ground in front of the avatar.
function Game:drop(number, slot)
  if not session.authority() then return end
  local bag = self.bags[number]
  if not bag or math.type(slot) ~= "integer" or not bag[slot] then return end
  local avatar = session.avatar(number)
  if not avatar then return end
  if self:groundCount() >= items.ground then
    session.send(number, "notice", "There is no room on the ground.")
    return
  end
  local entry = bag[slot]
  entry.count = entry.count - 1
  if entry.count <= 0 then table.remove(bag, slot) end
  local at = space.at(avatar)
  local x, z = world.move(at.x, at.z, 0, 0.7, 0.2)
  self:place(entry.key, x, z)
  self:sendBag(number)
end

-- "chat" request: checked, then told to everybody as "say" with the speaker's name.
function Game:chat(number, text)
  if not session.authority() or type(text) ~= "string" or not self.names[number] then return end
  text = text:gsub("%c", " "):gsub("^%s+", ""):gsub("%s+$", "")
  local length = utf8.len(text)
  if not length or length == 0 or length > CHAT_LENGTH then return end
  local frame = time.frame()
  if self.lastChat[number] and frame - self.lastChat[number] < CHAT_INTERVAL then return end
  self.lastChat[number] = frame
  session.send("all", "say", number, self.names[number], text)
end

-- Once, on the first frame with authority: the world's items, and offline the only avatar.
function Game:populate()
  self.spawned = true
  for _, item in ipairs(world.items) do self:place(item[1], item[2], item[3]) end
  if session.offline() then -- network.playerPrefab only spawns in a session
    local id = scene.instantiate("prefabs/player.prefab.json", { name = "Player 1" })
    scene.set(id, "NetPlayer", { player = 1 })
    scene.set(id, "NetSync", { owner = 1 })
  end
end

-- An avatar just entered the world: its player gets a name, and everybody hears about it.
function Game:welcome(number)
  self.names[number] = session.names[number] or net.name(number) or ("P" .. number)
  session.send("all", "notice", self.names[number] .. " joined.")
end

function Game:onUpdate()
  if not session.authority() then return end
  if not self.spawned then self:populate() end
  for _, id in ipairs(scene.all("NetPlayer")) do
    local number = scene.get(id, "NetPlayer").player
    if not self.names[number] then self:welcome(number) end
    if session.input(number).pressed("E") then self:pickup(number, id) end
  end
  game.set("ground", self:groundCount())
  if not session.offline() then gate.update(#scene.all("NetPlayer")) end
end

return Game
