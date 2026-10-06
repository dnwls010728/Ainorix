-- The way into the game, and the notice when the way back is cut (docs/ONLINERPG_SERVERS.md).
-- The script is always in one of these phases (see Phase below, one block each):
--   title     the Login and Offline buttons
--   offline   the player chose to play alone
--   login     a link to the login server: Guest -> our account, name and a session ticket
--   lobby     a link to the channel server: the ticket -> the channel list -> Enter -> where the
--             game server is and an entry ticket for it
--   entering  the engine session with that game server; the ticket is our join name, the server
--             asks the channel server about it and lets us into its running world
--   online    a world is running
--   popup     bad news in a modal popup; its OK button goes back to the title or closes the game
-- Entering a world restarts every script (the scene starts over from the server's state), so
-- "online" is simply: a world is running. If that stops without the player leaving, the popup
-- says so and OK closes the game (game.quit). Failing to get in shows the same popup, but OK only
-- returns to the title.
local session = require("scripts.lib.session")
local platform = require("scripts.lib.platform")
local text = require("scripts.lib.text")
local widgets = require("scripts.lib.widgets")
local phases = require("scripts.lib.phases")
local Connect = {}

local WAIT_FRAMES = 600 -- give up on a server after ten seconds
local PING_FRAMES = 300 -- keep the channel server's link alive while waiting there

-- ---------------------------------------------------------------- phases

local Phase = { title = {}, offline = {}, login = {}, lobby = {}, entering = {}, online = {}, popup = {} }

function Phase.title.enter(self)
  self.status:text("")
  self.popup:hide()
end

function Phase.title.update(self)
  if session.running() then self.phase:go("online") end -- started by a tool (editor preview)
end

function Phase.offline.enter(self)
  session.offlineMode = true
  self.title:hide()
end

Phase.offline.update = Phase.title.update

function Phase.login.enter(self)
  if not self:dial("login") then return end
  link.send(self.link, platform.GUEST)
  self.status:text(text.get("connecting"))
end

function Phase.login.update(self)
  self:listen()
end

-- login: what the login server said (platform.loginResult). The ticket proves who we are.
function Phase.lobby.enter(self, login)
  if not self:dial("lobby") then return end
  link.send(self.link, platform.SESSION, string.pack("<s1s1s1", login.ticket, login.name, login.proof))
  self.asked = false -- for a channel
  game.set("account", { id = login.account, name = login.name })
  self.status:text(text.get("lobby"))
end

function Phase.lobby.update(self)
  self:listen()
  if not self.phase:is("lobby") then return end
  if self.phase:frames() % PING_FRAMES == PING_FRAMES - 1 then link.send(self.link, platform.PING) end
end

-- world: the game server's address and our ticket for it (platform.enterResult).
function Phase.entering.enter(self, world)
  self:hangUp()
  if not pcall(net.join, { address = world.address, port = world.port, name = world.ticket }) then return self:fail("failed") end
  self.ready = false
  self.status:text(text.get("entering", world.channel))
end

function Phase.entering.update(self)
  local state = net.state().state
  if session.running() then
    self.phase:go("online")
  elseif state == "error" or state == "offline" or self.phase:frames() > WAIT_FRAMES then
    pcall(net.leave)
    self:fail("failed")
  elseif state == "lobby" and not self.ready then
    self.ready = pcall(net.ready, { ready = true })
  end
end

function Phase.online.enter(self)
  self.title:hide()
end

function Phase.online.update(self)
  if not session.running() and not net.isServer() then self.phase:go("popup", text.get("lost"), true) end
end

-- quit: OK closes the game (the world is gone); otherwise it returns to the title.
function Phase.popup.enter(self, line, quit)
  self:hangUp()
  self.quit = quit
  self.popupText:text(line)
  self.popup:show()
  game.set("popup", line)
end

-- ---------------------------------------------------------------- server messages

-- Reply[phase][message type]: what a message means in the phase that waits for it.
local Reply = { login = {}, lobby = {} }

Reply.login[platform.LOGIN_RESULT] = function(self, data)
  local login = platform.loginResult(data)
  if not login then return self:fail("failed") end
  self.phase:go("lobby", login)
end

-- The channel named at launch (oe run ... --set channel=2), else 0: the emptiest that has room.
local function wantedChannel()
  local wanted = math.tointeger(tonumber(game.arg("channel")) or 0) or 0
  return wanted >= 0 and wanted <= 255 and wanted or 0
end

-- There is no channel list on screen yet: the first list that arrives is answered with Enter.
Reply.lobby[platform.CHANNELS] = function(self, data)
  local list = platform.channels(data)
  if not list then return end
  game.set("channels", list)
  if self.asked then return end
  self.asked = true
  link.send(self.link, platform.ENTER, string.pack("<B", wantedChannel()))
end

Reply.lobby[platform.ENTER_RESULT] = function(self, data)
  local world = platform.enterResult(data)
  if not world then return self:fail("failed") end
  self.phase:go("entering", world)
end

-- An Error from a server, in words for the player.
local function refused(self, data)
  local code = platform.errorCode(data)
  local line = "refused"
  if code == platform.ERR_BUSY or code == platform.ERR_RATE_LIMITED then line = "busy"
  elseif code == platform.ERR_CHANNEL_FULL then line = "full"
  elseif code == platform.ERR_CHANNEL_OFFLINE then line = "closed" end
  self:fail(line)
end

-- The link of the login and lobby phases: its messages, and the ways it can fail. A message that
-- ends the phase ends the turn.
function Connect:listen()
  local polled = link.poll(self.link)
  if not polled then return self:fail("failed") end
  local waiting = self.phase.name
  for _, message in ipairs(polled.messages) do
    local reply = message.type == platform.ERROR and refused or Reply[waiting][message.type]
    if reply then reply(self, message.data) end
    if not self.phase:is(waiting) then return end
  end
  if polled.state == "closed" or self.phase:frames() > WAIT_FRAMES then self:fail("failed") end
end

-- ---------------------------------------------------------------- the link

function Connect:hangUp()
  if self.link then link.close(self.link) end
  self.link = nil
end

-- Calls the server `name` ("login", "lobby"). False, and the popup, when it cannot be reached.
function Connect:dial(name)
  self:hangUp()
  self.link = platform.open(name, platform.CLIENT)
  if not self.link then self:fail("failed") end
  return self.link ~= nil
end

-- Getting in failed: says why (a key of lib/text.lua); OK returns to the title.
function Connect:fail(key)
  self.phase:go("popup", text.get(key), false)
end

-- ---------------------------------------------------------------- buttons (button.lua calls these)

function Connect:login()
  if self.phase:is("title") then self.phase:go("login") end
end

function Connect:offline()
  if self.phase:is("title") then self.phase:go("offline") end
end

function Connect:confirm()
  if not self.phase:is("popup") then return end
  if self.quit then
    game.quit()
    return
  end
  pcall(net.leave)
  self.phase:go("title")
end

-- ---------------------------------------------------------------- script

function Connect:onStart()
  self.title = widgets.find("Title Panel")
  self.status = widgets.find("Title Status")
  self.popup = widgets.find("Popup")
  self.popupText = widgets.find("Popup Text")
  widgets.find("Login"):text(text.get("login"))
  widgets.find("Offline"):text(text.get("offline"))
  widgets.find("Popup OK"):text(text.get("ok"))
  self.phase = phases.new(self, Phase)
  self.phase:go(session.running() and "online" or "title")
end

function Connect:onUpdate()
  -- A server or a listen host has no way in to show: it only waits for its world.
  if net.isServer() and not session.offline() and not self.phase:is("online") then
    if session.running() then self.phase:go("online") end
    return
  end
  self.phase:update()
end

return Connect
