-- Small helpers for scripts that drive UI entities placed in the scene.
local I = require("scripts.lib.i18n")

local UI = {}

local cache, cacheScene = {}, nil

-- Entity id by name, cached per scene.
function UI.id(name)
  local sc = game.scene()
  if sc ~= cacheScene then cache, cacheScene = {}, sc end
  local id = cache[name]
  if not id then
    id = scene.find(name)
    cache[name] = id
  end
  return id
end

local KINDS = { "UIText", "UIButton", "UIPanel", "UIImage", "UISlider" }

local function kindOf(id)
  for _, k in ipairs(KINDS) do
    if scene.has(id, k) then return k end
  end
end

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

function UI.fmtTime(s)
  local total = math.max(0, math.floor(s))
  return string.format("%d:%02d", total // 60, total % 60)
end

-- Level pips named <prefix>1..count: lit up to `level`.
function UI.pips(prefix, level, count)
  for i = 1, count do
    UI.set(prefix .. i, { color = UI.rgb(i <= level and "#ffa53c" or "#e6d6bd") })
  end
end

-- ---------------------------------------------------------------- toasts

local toasts = {}

local function textWidth(str, size)
  local w = 0
  for _, c in utf8.codes(str) do w = w + (c < 0x2e80 and 0.5 or 0.92) * size end
  return w
end
UI.textWidth = textWidth

local function drawToasts()
  for i = 1, 5 do
    local t = toasts[i]
    UI.show("Toast" .. i, t ~= nil)
    if t then
      UI.set("Toast" .. i, { width = math.min(1100, textWidth(t.text, 20) + 56), opacity = math.min(1, t.life / 0.3) })
      UI.set("Toast" .. i .. ":text", { text = t.text, opacity = math.min(1, t.life / 0.3) })
    end
  end
end

function UI.toast(text, life)
  toasts[#toasts + 1] = { text = text, life = life or 2.2 }
  while #toasts > 5 do table.remove(toasts, 1) end
  drawToasts()
end

function UI.clearToasts()
  toasts = {}
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
