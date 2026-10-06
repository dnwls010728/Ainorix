-- Session helpers shared by every script: who decides the rules here, and messages that work
-- the same offline and in a session.
--   offline    the player chose to play alone (connect.lua sets offlineMode): this process is its own server
--   running    a world is being simulated with a server
--   authority  this process decides the rules (offline, or the server of a running world)
-- Session.on / Session.send wrap net.on / net.rpc: offline there is no session to carry an RPC,
-- so the handler is called directly.
-- names: on the server, player number -> the name the channel server vouched for (lib/gate.lua).
local Session = { handlers = {}, offlineMode = false, names = {} }

function Session.offline()
  return Session.offlineMode and net.state().state == "idle"
end

function Session.running()
  local state = net.state()
  return state.sync ~= nil and state.sync.state == "running"
end

function Session.authority()
  return Session.offline() or (net.isServer() and Session.running())
end

-- The local player number (1 offline and before joining).
function Session.me()
  local number = net.localPlayer()
  return number == 0 and 1 or number
end

-- The entity a player controls, or nil.
function Session.avatar(number)
  for _, id in ipairs(scene.all("NetPlayer")) do
    if scene.get(id, "NetPlayer").player == number then return id end
  end
end

-- A player's keys and sticks ({ down, pressed, axis }): this device offline, else what the session
-- carries for that player.
function Session.input(number)
  if Session.offline() then return input end
  return input.player(number)
end

function Session.on(name, fn)
  Session.handlers[name] = fn
  net.on(name, fn)
end

-- target: "server", "all" or a player number.
function Session.send(target, name, ...)
  if Session.offline() then
    local fn = Session.handlers[name]
    if fn then fn(...) end
    return
  end
  net.rpc(target, name, ...)
end

-- Who sent the message being handled.
function Session.sender()
  if Session.offline() then return 1 end
  return net.sender()
end

return Session
