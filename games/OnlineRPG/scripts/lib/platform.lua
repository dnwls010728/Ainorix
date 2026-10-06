-- The game's side of its back end (games/OnlineRPG/server, docs/ONLINERPG_SERVERS.md): message
-- numbers of server/common/Protocol.h and the few helpers both the client (connect.lua) and a
-- game server (lib/gate.lua) need. Messages travel over the engine's `link`:
--   u32 length, u16 type, payload; fields are string.pack formats ("<s1" = u8 length + bytes).
-- Where the servers are: launch parameters (oe run ... --set login=host:port --set lobby=host:port;
-- a game server: --set platform=host:port), else the local defaults below.
local Platform = {
  VERSION = 1,
  CLIENT = 1, GAME = 2, -- roles in Hello

  HELLO = 1,         -- <I2 version, B role (a game server adds s1 link key)
  ERROR = 2,         -- <I2 code
  GUEST = 12,        -- (empty)
  LOGIN_RESULT = 13, -- <I4 account, s1 name, s1 session ticket, s1 name proof
  SESSION = 20,      -- <s1 session ticket, s1 name, s1 name proof
  CHANNELS = 21,     -- <B count, then per channel: B channel, I2 players, I2 capacity, B online
  ENTER = 22,        -- <B channel (0 = the emptiest that has room)
  ENTER_RESULT = 23, -- <s1 address, I2 port, s1 entry ticket, B channel, s1 map
  PING = 24,         -- (empty)
  GAME_REGISTER = 30,   -- <B channel, s1 map, s1 address, I2 port, I2 capacity
  GAME_REGISTERED = 31, -- <I2 server id
  HEARTBEAT = 32,       -- <I2 players
  ADMIT = 33,           -- <I4 player, s1 entry ticket
  ADMIT_RESULT = 34,    -- <I4 player, I2 code (0 = admitted), I4 account, s1 name
  LEFT = 35,            -- <I4 account

  ERR_BUSY = 3, ERR_RATE_LIMITED = 4, ERR_CHANNEL_FULL = 10, ERR_CHANNEL_OFFLINE = 11, ERR_TICKET = 12,

  defaults = {
    login = { "127.0.0.1", 7001 },    -- oe_login
    lobby = { "127.0.0.1", 7002 },    -- oe_channel, the port for players
    platform = { "127.0.0.1", 7003 }, -- oe_channel, the port for game servers
  },
}

-- "host:port" from the launch parameter `name`, else the default.
function Platform.endpoint(name)
  local address, port = table.unpack(Platform.defaults[name])
  local given = game.arg(name)
  if given then
    local a, p = given:match("^([%d%.]+):(%d+)$")
    if a then address, port = a, tonumber(p) end
  end
  return address, port
end

-- Opens a link to the server `name` and says Hello. Returns the link id, or nil.
function Platform.open(name, role, key)
  local address, port = Platform.endpoint(name)
  local id = link.open { address = address, port = port }
  if not id then return nil end
  local hello = string.pack("<I2B", Platform.VERSION, role)
  if key then hello = hello .. string.pack("<s1", key) end
  link.send(id, Platform.HELLO, hello)
  return id
end

-- string.unpack that returns nil for bytes that do not fit the format (never trust the wire).
function Platform.unpack(format, data)
  local values = table.pack(pcall(string.unpack, format, data))
  if not values[1] then return nil end
  return table.unpack(values, 2, values.n - 1) -- without the trailing "next position"
end

-- What the messages a client receives say. Each returns nil for bytes that are not that message.

function Platform.errorCode(data)
  return Platform.unpack("<I2", data)
end

-- { account =, name =, ticket =, proof = }
function Platform.loginResult(data)
  local account, name, ticket, proof = Platform.unpack("<I4s1s1s1", data)
  if not account then return nil end
  return { account = account, name = name, ticket = ticket, proof = proof }
end

-- { { channel =, players =, capacity =, online = }, ... }
function Platform.channels(data)
  local count = Platform.unpack("<B", data)
  if not count then return nil end
  local list, at = {}, 2
  for _ = 1, count do
    local channel, players, capacity, online = Platform.unpack("<BI2I2B", data:sub(at))
    if not channel then return nil end
    list[#list + 1] = { channel = channel, players = players, capacity = capacity, online = online == 1 }
    at = at + 6
  end
  return list
end

-- { address =, port =, ticket =, channel = }
function Platform.enterResult(data)
  local address, port, ticket, channel = Platform.unpack("<s1I2s1Bs1", data)
  if not address then return nil end
  return { address = address, port = port, ticket = ticket, channel = channel }
end

return Platform
