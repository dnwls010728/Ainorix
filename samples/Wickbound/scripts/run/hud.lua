-- In-run HUD: bars, timer, counters, build icons, boss bar, banner, damage numbers,
-- off-screen indicators, hurt vignette and the virtual joystick. The entities are laid
-- out in scenes/run.scene.json; this script fills them in.
local D = require("scripts.lib.defs")
local B = require("scripts.lib.balance")
local W = require("scripts.lib.world")
local I = require("scripts.lib.i18n")
local UI = require("scripts.lib.ui")
local Audio = require("scripts.lib.audio")

local Hud = {}

local FONT = "assets/fonts/Jua-Regular.ttf"
local ICON = "assets/icons/"
local VIEW_H = 290 * B.U * 2 -- world units across the view height (Camera.orthoSize * 2)
local PX = 720 / VIEW_H -- reference pixels per world unit
local floor, min, max, sin, cos, atan, abs = math.floor, math.min, math.max, math.sin, math.cos, math.atan, math.abs

function Hud:onStart()
  W.hud = self
  self.cache = {}
  self.nums = {} -- { x, y, text, life, big, color }
  self.numPool = {} -- UIText entities
  self.numRoot = UI.id("Numbers")
  self.cam = scene.find("Camera")
  self.arrows = {
    brazier = scene.find("Arrow brazier"),
    chest = { scene.find("Arrow chest 1"), scene.find("Arrow chest 2") },
    boss = { scene.find("Arrow boss 1"), scene.find("Arrow boss 2") },
  }
  self.arrowOn = {}
  self.bannerLife = 0
  self.bossMusic = false
  self.t = 0
  I.setLocale(require("scripts.lib.meta").data.settings.locale)
  UI.clearToasts()
  UI.localize()
end

function Hud:changed(key, v)
  if self.cache[key] == v then return false end
  self.cache[key] = v
  return true
end

function Hud:toast(text)
  UI.toast(text, 2.6)
end

function Hud:banner(name)
  UI.show("Banner", true)
  UI.text("Banner:kicker", "* " .. I.t("banner.warning") .. " *")
  UI.text("Banner:name", I.L(name))
  UI.set("Banner", { width = max(420, UI.textWidth(I.L(name), 64) + 120) })
  self.bannerLife = 3.1
end

function Hud:number(x, y, text, color, big)
  if #self.nums >= 55 and not big then return end
  self.nums[#self.nums + 1] = { x = x + (math.random() - 0.5) * 14 * B.U, y = y, text = text, life = big and 0.9 or 0.6, big = big, color = color }
end

function Hud:drawNumbers(dt, cx, cy)
  local nums = self.nums
  local j = 1
  for i = 1, #nums do
    local n = nums[i]
    n.life = n.life - dt
    n.y = n.y + 38 * B.U * dt
    if n.life > 0 then
      nums[j] = n
      j = j + 1
    end
  end
  for i = j, #nums do nums[i] = nil end
  for i = 1, max(#nums, #self.numPool) do
    local n, slot = nums[i], self.numPool[i]
    if n then
      local f = n.life / (n.big and 0.9 or 0.6)
      local values = {
        text = n.text, size = n.big and 25 or 18, color = UI.rgb(n.color), x = (n.x - cx) * PX, y = -(n.y - cy) * PX,
        opacity = min(1, f * 2), visible = true,
      }
      if not slot then
        values.font, values.anchor, values.outlineWidth, values.richText = FONT, "center", 2, false
        slot = scene.create("Number", { UIText = values })
        self.numPool[i] = slot
      else
        scene.set(slot, "UIText", values)
      end
    elseif slot and self.numShown and i <= self.numShown then
      scene.set(slot, "UIText", { visible = false })
    end
  end
  self.numShown = #nums
end

-- Points an arrow sprite at a world position that is off screen.
function Hud:arrow(id, wx, wy, cx, cy, halfW)
  local margin = 46 / PX
  local halfH = VIEW_H / 2
  local x, y = wx - cx, wy - cy
  local shown = false
  if not (x > -halfW + margin and x < halfW - margin and y > -halfH + margin and y < halfH - margin - 40 / PX) then
    local a = atan(y, x)
    local tx, ty = cos(a), sin(a)
    local kx = tx ~= 0 and (halfW - margin) / abs(tx) or math.huge
    local ky = ty ~= 0 and (halfH - margin - 30 / PX) / abs(ty) or math.huge
    local k = min(kx, ky)
    scene.set(id, "Transform", { position = { x = cx + tx * k, y = cy + ty * k - 10 / PX, z = 0 }, rotation = { x = 0, y = 0, z = math.deg(a) } })
    scene.set(id, "Sprite", { visible = true, opacity = 0.75 + sin(self.t * 5) * 0.2 })
    shown = true
  end
  if not shown and self.arrowOn[id] then scene.set(id, "Sprite", { visible = false }) end
  self.arrowOn[id] = shown
end

function Hud:hideArrow(id)
  if self.arrowOn[id] then
    scene.set(id, "Sprite", { visible = false })
    self.arrowOn[id] = false
  end
end

function Hud:drawItems(row, items)
  for i = 1, 6 do
    local it = items[i]
    local name = row .. i
    UI.show(name, it ~= nil)
    if it then
      local info = D.weaponById(it.id) or D.passiveById(it.id)
      UI.set(name, { color = UI.rgb(it.evolved and "#fff3c4" or "#fff8ec"), borderColor = UI.rgb(it.evolved and "#ffcf5c" or "#ffe9c0") })
      UI.set(name .. ":icon", { texture = ICON .. info.icon .. ".png" })
      UI.pips(name .. ":pip", it.level, 5)
    end
  end
end

local function sig(items)
  local parts = {}
  for i, it in ipairs(items) do parts[i] = it.id .. it.level .. (it.evolved and "*" or "") end
  return table.concat(parts, "|")
end

function Hud:onUpdate(dt)
  self.t = self.t + 1 / 60
  Audio.update(1 / 60)
  UI.updateToasts()
  local g, p = W.game, W.player
  if not g or not p or not p.maxHp then return end
  local changed = self.changed

  local xp = g.xpNext > 0 and min(1, max(0, g.xp / g.xpNext)) or 0
  if changed(self, "xp", floor(xp * 1000)) then UI.set("XP", { value = xp }) end
  if changed(self, "lv", g.level .. I.getLocale()) then UI.text("Lv", I.t("hud.lv", { n = g.level })) end
  local sec = floor(g.time)
  if changed(self, "time", sec .. (g.endless and "e" or "")) then
    UI.set("Timer", { text = (g.endless and "+ " or "") .. UI.fmtTime(sec), color = UI.rgb(g.endless and "#ffcf5c" or "#ffffff") })
  end
  local hp = min(1, max(0, p.hp / p.maxHp))
  if changed(self, "hp", floor(hp * 1000)) then UI.set("HP", { value = hp, fillColor = UI.rgb(hp < 0.25 and "#ff5a7a" or "#ff8fa8") }) end
  local hpText = math.ceil(max(0, p.hp)) .. " / " .. math.ceil(p.maxHp)
  if changed(self, "hpt", hpText) then UI.text("HP:text", hpText) end
  local oil = min(1, max(0, p.oil / p.maxOil))
  if changed(self, "oil", floor(oil * 1000)) then UI.set("Oil", { value = oil, fillColor = UI.rgb(oil < 0.25 and "#ff9a5c" or "#ffcf5c") }) end
  local oilText = math.ceil(oil * 100) .. "%"
  if changed(self, "oilt", oilText) then UI.text("Oil:text", oilText) end
  if changed(self, "kills", g.kills) then UI.text("Kills", UI.fmt(g.kills)) end
  if changed(self, "glims", floor(g.glims)) then UI.text("Run glims", UI.fmt(floor(g.glims))) end
  local brz = g.braziersLit .. "/" .. g.braziersTotal
  if changed(self, "brz", brz) then UI.text("Braziers", brz) end
  if changed(self, "weapons", sig(g.weapons)) then self:drawItems("Weapons", g.weapons) end
  if changed(self, "passives", sig(g.passives)) then self:drawItems("Passives", g.passives) end

  -- boss bar and music
  local boss = nil
  for _, e in ipairs(W.enemyList) do
    if e.boss then
      boss = e
      break
    end
  end
  if changed(self, "boss", boss ~= nil) then
    UI.show("Boss name", boss ~= nil)
    UI.show("Boss bar", boss ~= nil)
    Audio.music(boss and "boss" or "run")
  end
  if boss then
    local name = I.L(B.ENEMIES[boss.kind].name)
    if changed(self, "bossn", name) then UI.text("Boss name", name) end
    local frac = max(0, boss.hp / boss.maxHp)
    if changed(self, "bossf", floor(frac * 1000)) then UI.set("Boss bar", { value = frac }) end
  end

  if self.bannerLife > 0 then
    self.bannerLife = self.bannerLife - 1 / 60
    if self.bannerLife <= 0 then UI.show("Banner", false) end
  end

  -- world-anchored overlays follow the camera
  local cam = scene.get(self.cam, "Transform").position
  local cx, cy = cam.x, cam.y
  self:drawNumbers(dt, cx, cy)
  local halfW = VIEW_H * 16 / 9 / 2
  local best, bd = nil, math.huge
  for _, b in ipairs(W.braziers) do
    if not b.lit then
      local d = (b.x - p.x) ^ 2 + (b.y - p.y) ^ 2
      if d < bd then best, bd = b, d end
    end
  end
  if best then self:arrow(self.arrows.brazier, best.x, best.y, cx, cy, halfW) else self:hideArrow(self.arrows.brazier) end
  local chests = {}
  for _, id in ipairs(scene.withTag("chest")) do chests[#chests + 1] = scene.get(id, "Transform").position end
  for i, id in ipairs(self.arrows.chest) do
    if chests[i] then self:arrow(id, chests[i].x, chests[i].y, cx, cy, halfW) else self:hideArrow(id) end
  end
  local bosses = {}
  for _, e in ipairs(W.enemyList) do
    if e.boss then bosses[#bosses + 1] = e end
  end
  for i, id in ipairs(self.arrows.boss) do
    if bosses[i] then self:arrow(id, bosses[i].x, bosses[i].y, cx, cy, halfW) else self:hideArrow(id) end
  end

  -- hurt / low health vignette
  local low = p.hp < p.maxHp * 0.3
  if p.hurtFlash > 0 or low then
    local a = max(p.hurtFlash * 1.6, low and 0.18 + sin(self.t * 6) * 0.08 or 0)
    UI.set("Vignette", { visible = true, opacity = min(0.5, a) })
    self.vignette = true
  elseif self.vignette then
    self.vignette = false
    UI.show("Vignette", false)
  end

  -- virtual joystick
  local s = p.stick
  if s.active and dt > 0 then
    UI.set("Stick", { visible = true, x = s.ox - 70, y = s.oy - 70 })
    UI.set("Stick knob", { visible = true, x = s.x - 28, y = s.y - 28 })
    self.stick = true
  elseif self.stick then
    self.stick = false
    UI.show("Stick", false)
    UI.show("Stick knob", false)
  end
end

return Hud
