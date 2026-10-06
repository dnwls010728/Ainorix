-- Everything a peer shows for itself, built from what the server replicates and tells:
--   chat      the chat log; Enter focuses the chat field (chatbox.lua sends it)
--   bag       the inventory panel filled by "inv" messages; I or the Bag button shows/hides it
--   avatars   walk cycle, facing and name from the state the server replicates, a tint per player number
--   plates    a name plate over every avatar, a speech bubble for a few seconds after a "say"
--   items     ground items bob, and a prompt appears when one is within reach
-- The sections below follow that order; onUpdate at the end is the frame. Nothing here changes
-- the rules, so it runs the same on a client, a host and offline.
local session = require("scripts.lib.session")
local items = require("scripts.lib.items")
local space = require("scripts.lib.space")
local widgets = require("scripts.lib.widgets")
local Hud = {}

local PLATES = 8          -- name plate / bubble entities in the scene; the lowest-numbered avatars get them
local BUBBLE_FRAMES = 300 -- 5 seconds
local CHAT_LINES = 7
local HEAD = 1.7          -- height of a name plate above the feet, world units
local tints = { { 1, 1, 1 }, { 1, 0.78, 0.74 }, { 0.76, 1, 0.8 }, { 0.8, 0.84, 1 },
                { 1, 0.95, 0.7 }, { 0.95, 0.78, 1 }, { 0.75, 1, 1 }, { 1, 0.86, 0.7 } }

function Hud:onStart()
  self.camera = scene.find("Camera")
  self.chatText = widgets.find("Chat Text")
  self.chatInput = widgets.find("Chat Input")
  self.bag = widgets.find("Bag")
  self.prompt = widgets.find("Prompt")
  self.slots = widgets.list("Slot ", items.slots)
  self.namePlates = widgets.list("Name ", PLATES)
  self.bubbles = widgets.list("Bubble ", PLATES)
  self.bubbleTexts = widgets.list("Bubble Text ", PLATES)
  self.lines = {}   -- chat log, oldest first
  self.said = {}    -- player number -> { text, width, height, untilFrame } of its speech bubble
  self.tinted = {}  -- avatar entity id -> its sprite got the player tint
  self.used = 0     -- plates shown last frame
  session.on("say", function(number, name, text) self:say(number, name, text) end)
  session.on("inv", function(bag) self:inventory(bag) end)
  session.on("notice", function(text) self:line(text) end)
  self:line("Enter: chat. E: pick up. Click a bag slot to drop.")
end

-- ---------------------------------------------------------------- chat

function Hud:line(text)
  if type(text) ~= "string" then return end
  self.lines[#self.lines + 1] = text
  while #self.lines > CHAT_LINES do table.remove(self.lines, 1) end
  self.chatText:text(table.concat(self.lines, "\n"))
  game.set("chat", self.lines)
end

-- The actual wrapped layout at the current local view scale, with existing padding/max width.
local function bubbleSize(text, label)
  local style = { font = label.font, size = label.size, letterSpacing = label.letterSpacing,
    lineSpacing = label.lineSpacing, bold = label.bold, rich = label.richText }
  local widest = 216
  local natural = ui.measureText({ text = text, style = style })
  local width = math.min(widest, math.ceil(natural.width) + 8)
  local wrapped = ui.measureText({ text = text, style = style, wrapWidth = width })
  return width, math.ceil(wrapped.height) + 18
end

-- "say" from the server: a chat line, and a bubble over the speaker.
function Hud:say(number, name, text)
  if type(text) ~= "string" or type(name) ~= "string" or math.type(number) ~= "integer" or not utf8.len(text) then return end
  self:line(name .. ": " .. text)
  local width, height = bubbleSize(text, self.bubbleTexts[1]:get())
  self.said[number] = { text = text, width = width, height = height, untilFrame = time.frame() + BUBBLE_FRAMES }
end

function Hud:openChat()
  self.chatInput:focus()
  session.chatOpened = time.frame() -- the field sees this Enter too; chatbox.lua ignores it
end

-- ---------------------------------------------------------------- bag

-- "inv" from the server: what our bag holds now.
function Hud:inventory(bag)
  if type(bag) ~= "table" then return end
  for index, button in ipairs(self.slots) do
    local slot = bag[index]
    local known = type(slot) == "table" and items.names[slot.key]
    button:set({
      icon = known and items.icon(slot.key) or "",
      text = known and slot.count > 1 and ("x" .. math.tointeger(slot.count)) or "" })
  end
  game.set("bagShown", bag)
end

function Hud:toggleBag()
  self.bag:show(not self.bag:visible())
end

-- ---------------------------------------------------------------- avatars

-- What an avatar does, which way it faces and whose it is. This comes from the server (the Tag
-- player.lua writes: "walk_left,Guest0001"). Guessing it from how the position changed would
-- flicker: a correction replays frames with remote avatars elsewhere.
local function avatarState(id)
  local tag = scene.get(id, "Tag")
  return string.match(tag and tag.tags or "", "^(%a+)_(%a+),(.*)$")
end

-- The avatar's sprite: the player's tint once, then the clip and the flip of what it does.
function Hud:dress(id, number, action, facing)
  local art = scene.child(id, "Art")
  if not art then return end
  if not self.tinted[id] then
    self.tinted[id] = true
    local c = tints[(number - 1) % #tints + 1]
    scene.set(art, "Sprite", { color = { r = c[1], g = c[2], b = c[3] } })
  end
  if not action then return end
  local sideways = facing == "left" or facing == "right"
  local clip = action .. "_" .. (sideways and "side" or facing)
  if scene.get(art, "SpriteAnimation").clip ~= clip then scene.set(art, "SpriteAnimation", { clip = clip }) end
  if scene.get(art, "Sprite").flipX ~= (facing == "left") then scene.set(art, "Sprite", { flipX = facing == "left" }) end
end

-- ---------------------------------------------------------------- name plates and bubbles

-- Puts plate number `plate` over a head at screen point (x, y). said: the player's speech bubble
-- or nil. Returns true while the bubble shows.
function Hud:label(plate, x, y, name, said)
  self.namePlates[plate]:set({ text = name or "", x = x - 80, y = y - 22, visible = true })
  local bubble = self.bubbles[plate]
  if not said or time.frame() >= said.untilFrame then
    bubble:hide()
    return false
  end
  -- Reflow at the current effective scale/style; a message can outlive a view resize.
  local width, height = bubbleSize(said.text, self.bubbleTexts[plate]:get())
  self.bubbleTexts[plate]:set({ text = said.text, width = width })
  bubble:set({ x = x - (width + 24) * 0.5, y = y - 28 - height,
               width = width + 24, height = height, visible = true })
  return true
end

-- Dresses every avatar and hands the plates to those on screen.
function Hud:avatars()
  local plate, speaking = 0, {}
  for _, id in ipairs(scene.all("NetPlayer")) do
    local number = scene.get(id, "NetPlayer").player
    local action, facing, name = avatarState(id)
    self:dress(id, number, action, facing)
    local p = space.at(id)
    local x, y = space.project(self.camera, p.x, p.y + HEAD, p.z)
    if x and plate < PLATES then
      plate = plate + 1
      speaking[number] = self:label(plate, x, y, name, self.said[number])
    end
  end
  for unused = plate + 1, self.used do
    self.namePlates[unused]:hide()
    self.bubbles[unused]:hide()
  end
  self.used = plate
  self:forget(speaking)
end

-- Drops what is kept per player and per avatar once it is over: bubbles nobody shows, avatars
-- that left.
function Hud:forget(speaking)
  for number in pairs(self.said) do
    if not speaking[number] then self.said[number] = nil end
  end
  for id in pairs(self.tinted) do
    if not scene.exists(id) then self.tinted[id] = nil end
  end
end

-- ---------------------------------------------------------------- ground items

local function bob(id)
  local art = scene.child(id, "Art")
  if art then scene.set(art, "Transform", { position = { y = 0.45 + 0.07 * math.sin(time.now() * 3 + id) } }) end
end

-- Items bob; the pick-up prompt shows while one is within our reach.
function Hud:groundItems()
  local me = session.avatar(session.me())
  local at = me and space.at(me)
  local near = false
  for _, id in ipairs(scene.withTag("item")) do
    bob(id)
    near = near or (at ~= nil and space.within(space.at(id), at, items.reach))
  end
  if near ~= self.near then
    self.near = near
    self.prompt:show(near)
  end
end

-- ---------------------------------------------------------------- frame

function Hud:onUpdate()
  if input.pressed("I") then self:toggleBag() end
  if input.pressed("Enter") and not ui.focused() then self:openChat() end
  self:avatars()
  self:groundItems()
end

return Hud
