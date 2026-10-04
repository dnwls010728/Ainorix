-- Small helpers for scripts that drive UI entities placed in the scene.
local I = require("scripts.lib.i18n")

local UI = {}

local cache = {}

-- Entity id by name (names are unique in the scene).
function UI.id(name)
  local id = cache[name]
  if not id or not scene.exists(id) then
    id = scene.find(name)
    cache[name] = id
  end
  return id
end

local KINDS = { "UIText", "UIButton", "UIPanel", "UIImage", "UISlider" }
local kinds = {}

local function kindOf(id)
  local k = kinds[id]
  if k then return k end
  for _, name in ipairs(KINDS) do
    if scene.has(id, name) then
      kinds[id] = name
      return name
    end
  end
end
UI.kind = kindOf

-- Sets fields on the entity's UI component, whichever kind it is.
function UI.set(name, values)
  local id = UI.id(name)
  if id then scene.set(id, kindOf(id), values) end
end

function UI.show(name, visible)
  UI.set(name, { visible = visible and true or false })
end

function UI.text(name, text)
  UI.set(name, { text = text })
end

local colors = {}
function UI.rgb(hex)
  local c = colors[hex]
  if not c then
    c = { tonumber(hex:sub(2, 3), 16) / 255, tonumber(hex:sub(4, 5), 16) / 255, tonumber(hex:sub(6, 7), 16) / 255 }
    colors[hex] = c
  end
  return c
end

-- Fills every text and button tagged "t:<key>" with the string for the current language.
function UI.localize()
  for _, id in ipairs(scene.all("Tag")) do
    local key = scene.get(id, "Tag").tags:match("t:([%w%._]+)")
    if key then
      if scene.has(id, "UIText") then
        scene.set(id, "UIText", { text = I.t(key) })
      elseif scene.has(id, "UIButton") then
        scene.set(id, "UIButton", { text = I.t(key) })
      end
    end
  end
end

function UI.fmt(n)
  local s = tostring(math.floor(n + 0.5))
  local out = s:reverse():gsub("(%d%d%d)", "%1,"):reverse()
  return (out:gsub("^,", ""))
end

-- Rough width of a text in reference pixels (Latin letters about half an em, CJK about one).
function UI.textWidth(str, size)
  local w = 0
  for _, c in utf8.codes(str) do w = w + (c < 0x2e80 and 0.52 or 0.94) * size end
  return w
end

-- ---------------------------------------------------------------- toasts

local toasts = {}
local TOASTS = 3

local function drawToasts()
  for i = 1, TOASTS do
    local t = toasts[i]
    UI.show("Toast" .. i, t ~= nil)
    if t then
      local a = math.min(1, t.life / 0.3)
      UI.set("Toast" .. i, { width = math.min(660, UI.textWidth(t.text, 30) + 72), opacity = a })
      UI.set("Toast" .. i .. ":text", { text = t.text, opacity = a })
    end
  end
end

function UI.toast(text, life)
  toasts[#toasts + 1] = { text = text, life = life or 2.2 }
  while #toasts > TOASTS do table.remove(toasts, 1) end
  drawToasts()
end

-- Toasts run on real frames so they also fade while the game is paused.
function UI.updateToasts()
  if #toasts == 0 then return end
  for i = #toasts, 1, -1 do
    toasts[i].life = toasts[i].life - 1 / 60
    if toasts[i].life <= 0 then table.remove(toasts, i) end
  end
  drawToasts()
end

return UI
