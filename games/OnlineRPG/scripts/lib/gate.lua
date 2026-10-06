-- A game server's door: who may enter its world.
-- With a channel server (launch parameter `platform`, see lib/platform.lua) this server registers
-- there, reports its population every second, and asks about every player that connects: the
-- player's join name is an entry ticket, the channel server says whose it is, and only then
-- net.admit lets the avatar spawn (project.json: network.admission "script"). A refused or
-- unanswered ticket is kicked. While the link is down nobody new gets in, the world keeps running,
-- and the link is retried with a growing pause.
-- Without a channel server (a server started by hand, an editor preview) everybody is admitted
-- under the name they joined with.
local platform = require("scripts.lib.platform")
local Gate = {
  link = nil,
  registered = false,
  server = 0,       -- our id at the channel server
  retryAt = 0,      -- frame of the next connection attempt
  pause = 60,       -- frames until that attempt; doubles up to ten seconds
  pending = {},     -- player number -> frame the question was sent
  accounts = {},    -- player number -> account, for players that were admitted
  onAdmitted = nil, -- function(number, account, name)
}

local ADMIT_FRAMES = 300 -- a question unanswered for five seconds is a refusal

function Gate.enabled()
  return game.arg("platform") ~= nil
end

local function refuse(number, reason)
  Gate.pending[number] = nil
  pcall(net.kick, { player = number, reason = reason })
end

local function admit(number, account, name)
  Gate.pending[number] = nil
  Gate.accounts[number] = account
  if Gate.onAdmitted then Gate.onAdmitted(number, account, name) end
  pcall(net.admit, number)
end

-- A player reached the lobby.
function Gate.joined(number)
  if not Gate.enabled() then return admit(number, 0, net.name(number) or ("P" .. number)) end
  if not Gate.registered then return refuse(number, "server not ready") end
  local ticket = net.name(number) or ""
  if #ticket > 64 or not link.send(Gate.link, platform.ADMIT, string.pack("<I4s1", number, ticket)) then
    return refuse(number, "ticket refused")
  end
  Gate.pending[number] = time.frame()
end

function Gate.left(number)
  Gate.pending[number] = nil
  local account = Gate.accounts[number]
  Gate.accounts[number] = nil
  if account and account ~= 0 and Gate.registered then link.send(Gate.link, platform.LEFT, string.pack("<I4", account)) end
end

local function connect()
  local state = net.state()
  Gate.link = platform.open("platform", platform.GAME, game.arg("linkkey") or "")
  if not Gate.link then return end
  link.send(Gate.link, platform.GAME_REGISTER, string.pack("<Bs1s1I2I2",
    tonumber(game.arg("channel")) or 1, game.arg("map") or "town", game.arg("address") or "127.0.0.1",
    state.port, state.maxPlayers - 1))
end

local function dropped(reason)
  if Gate.registered then print("gate: lost the channel server (" .. tostring(reason) .. "); nobody can enter until it is back") end
  if Gate.link then link.close(Gate.link) end
  Gate.link, Gate.registered = nil, false
  for number in pairs(Gate.pending) do refuse(number, "server not ready") end
  Gate.retryAt = time.frame() + Gate.pause
  Gate.pause = math.min(Gate.pause * 2, 600)
end

-- Every frame on the server. `players`: avatars in the world.
function Gate.update(players)
  if not Gate.enabled() then return end
  local frame = time.frame()
  if not Gate.link then
    if frame < Gate.retryAt then return end
    connect()
    if not Gate.link then
      Gate.retryAt = frame + Gate.pause
      return
    end
  end
  local polled = link.poll(Gate.link)
  if not polled or polled.state == "closed" then return dropped(polled and polled.error or "link gone") end
  for _, message in ipairs(polled.messages) do
    if message.type == platform.GAME_REGISTERED then
      Gate.server = platform.unpack("<I2", message.data) or 0
      Gate.registered, Gate.pause = true, 60
      print("gate: registered at the channel server as game server " .. Gate.server)
    elseif message.type == platform.ADMIT_RESULT then
      local number, code, account, name = platform.unpack("<I4I2I4s1", message.data)
      if number and Gate.pending[number] then
        if code == 0 then admit(number, account, name) else refuse(number, "ticket refused") end
      end
    elseif message.type == platform.ERROR then
      print("gate: the channel server refused us, code " .. tostring(platform.unpack("<I2", message.data)))
    end
  end
  if Gate.registered and frame % 60 == 0 then link.send(Gate.link, platform.HEARTBEAT, string.pack("<I2", players)) end
  for number, since in pairs(Gate.pending) do
    if frame - since > ADMIT_FRAMES then refuse(number, "ticket refused") end
  end
end

return Gate
