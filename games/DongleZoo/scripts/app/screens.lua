-- Stateless screens methods. The App owner explicitly supplies self.services.
local Screens = {}
local SPRITE_SCALE = 2 * 256 / 200
local DIALOGS = { "pause", "over", "shop", "book", "missions", "daily", "settings", "item" }
local min, max = math.min, math.max

function Screens:layout()
  local D = self.services.D
  local w, h = 720, 1280
  if input.viewSize then w, h = input.viewSize() end -- players older than this binding: assume the design shape
  if w == self.vw and h == self.vh then return end
  self.vw, self.vh = w, h
  local aspect = w / max(1, h)
  local base = 1280 / D.PX / 2 -- half the height of the 720 x 1280 design, in units
  local narrow = aspect <= 720 / 1280
  self.ortho = narrow and (720 / D.PX / 2) / aspect or base
  self.aspect = aspect
  self.camY = self.ortho - base -- keeps the floor at the same distance from the bottom edge
  scene.set(self.camera, "Transform", { position = { x = 0, y = self.camY, z = 20 } })
  scene.set(self.camera, "Camera", { orthoSize = self.ortho })
  scene.set(self.canvas, "UICanvas", { match = narrow and 0 or 1 })
  local cover = 2 * self.ortho * max(1, aspect) + 0.5
  scene.set(self.background, "Transform", { position = { x = 0, y = self.camY, z = -2 } })
  scene.set(self.background, "Sprite", { width = cover, height = cover })
end

function Screens:pointer()
  local mx, my = input.mouse()
  local w, h = input.viewSize()
  local hit = camera.screenToPlane({ camera = self.camera, screen = { x = mx * w, y = my * h },
    plane = { point = { x = 0, y = 0, z = 0 }, normal = { x = 0, y = 0, z = 1 } } })
  assert(hit.hit, "jar plane must be in front of the camera")
  return hit.point.x, hit.point.y
end

function Screens:applyTheme()
  local D = self.services.D
  local UI = self.services.UI
  local Meta = self.services.Meta
  local th = D.theme(Meta.data.theme)
  scene.set(self.background, "Sprite", { texture = "assets/bg/" .. th.id .. ".png" })
  self.lineColor = UI.rgb(th.line)
  scene.set(self.line, "Sprite", { color = self.lineColor })
end

function Screens:refreshWallet()
  local UI = self.services.UI
  local Meta = self.services.Meta
  UI.text("Wallet:n", UI.fmt(Meta.data.coins))
end

function Screens:refreshItems()
  local D = self.services.D
  local UI = self.services.UI
  local Meta = self.services.Meta
  for _, it in ipairs(D.ITEMS) do
    local n = Meta.data.items[it.id]
    UI.text("Item:" .. it.id .. ":n", n > 0 and tostring(n) or "+")
    UI.set("Item:" .. it.id .. ":badge", { color = UI.rgb(n > 0 and "#ff6b6b" or "#4caf50") })
  end
end

function Screens:showHome()
  local UI = self.services.UI
  self.mode = "home"
  self:setDialog(nil)
  game.pause(false)
  UI.show("Home", true)
  UI.show("HUD", false)
  scene.set(self.holder, "Sprite", { visible = false })
  scene.set(self.guide, "Sprite", { visible = false })
  self:refreshHome()
  self:refreshWallet()
end

function Screens:refreshHome()
  local I = self.services.I
  local UI = self.services.UI
  local Meta = self.services.Meta
  UI.text("Home:best", I.t("home.best", { n = UI.fmt(Meta.data.best) }))
  UI.show("Home:dot:daily", Meta.dailyAvailable())
  UI.show("Home:dot:missions", Meta.missionsReady() > 0)
end

function Screens:setDialog(name)
  local UI = self.services.UI
  if self.dialog == name then return end
  for _, d in ipairs(DIALOGS) do UI.show("Dlg:" .. d, d == name) end
  self.dialog = name
  if self.mode == "play" then game.pause(name ~= nil) end
end

function Screens:refreshScore()
  local UI = self.services.UI
  local Meta = self.services.Meta
  UI.text("HUD:score", UI.fmt(self.score))
  UI.text("HUD:bestv", UI.fmt(max(Meta.data.best, self.score)))
end

function Screens:refreshHolder()
  local D = self.services.D
  local UI = self.services.UI
  local show = self.mode == "play" and self.cooldown <= 0 and not self.target
  local r = D.TIERS[self.cur].r
  local x = max(-D.JAR_HALF + r, min(D.JAR_HALF - r, self.holderX))
  self.holderX = x
  scene.set(self.holder, "Transform", { position = { x = x, y = D.DROP_Y, z = 0 } })
  scene.set(self.holder, "Sprite", { visible = show, texture = D.texture(self.cur), width = r * SPRITE_SCALE, height = r * SPRITE_SCALE })
  local top = D.DROP_Y - r
  scene.set(self.guide, "Transform", { position = { x = x, y = (top + D.FLOOR_Y) / 2, z = 0 } })
  scene.set(self.guide, "Sprite", { visible = show, height = top - D.FLOOR_Y })
  UI.set("HUD:nextimg", { texture = D.texture(self.nxt) })
end

function Screens:showOver()
  local I = self.services.I
  local UI = self.services.UI
  local Meta = self.services.Meta
  local Audio = self.services.Audio
  local canRevive = not self.revived
  if not canRevive then self:finalize() end
  local best = max(Meta.data.best, self.score)
  local newBest = self.wasBest or self.score > Meta.data.best
  UI.text("Over:title", I.t(newBest and "over.newbest" or "over.title"))
  UI.text("Over:score", UI.fmt(self.score))
  UI.text("Over:best", I.t("home.best", { n = UI.fmt(best) }))
  UI.text("Over:coins", I.t("over.coins", { n = UI.fmt(self:earned() * (self.doubled and 2 or 1)) }))
  UI.show("Over:revive", canRevive)
  UI.show("Over:double", not self.doubled and self:earned() > 0)
  self:setDialog("over")
  if newBest then Audio.sfx("fanfare") end
end

function Screens:openItemDialog(id)
  local D = self.services.D
  local I = self.services.I
  local UI = self.services.UI
  local it = D.item(id)
  self.itemWanted = id
  UI.text("ItemDlg:title", I.t("item.title", { name = I.L(it.name) }))
  UI.text("ItemDlg:desc", I.L(it.desc))
  UI.set("ItemDlg:icon", { texture = "assets/ui/" .. id .. ".png" })
  UI.text("ItemDlg:buy", I.t("item.buy", { n = it.price }))
  self:setDialog("item")
end

function Screens:refreshShop()
  local D = self.services.D
  local I = self.services.I
  local UI = self.services.UI
  local Meta = self.services.Meta
  for _, th in ipairs(D.THEMES) do
    local owned, using = Meta.hasTheme(th.id), Meta.data.theme == th.id
    UI.text("Theme:" .. th.id .. ":name", I.L(th.name))
    UI.text("Theme:" .. th.id .. ":btn", using and I.t("shop.using") or owned and I.t("shop.use") or UI.fmt(th.price))
    UI.show("Theme:" .. th.id .. ":coin", not owned)
    UI.set("Theme:" .. th.id .. ":btn", { interactable = not using })
  end
  for _, p in ipairs(D.PRODUCTS) do
    local owned = p.id == "no_ads" and Meta.data.noAds
    UI.text("Product:" .. p.id .. ":name", I.L(p.name))
    UI.text("Product:" .. p.id .. ":desc", I.L(p.desc))
    UI.text("Product:" .. p.id .. ":btn", owned and I.t("shop.owned") or p.price)
    UI.set("Product:" .. p.id .. ":btn", { interactable = not owned })
  end
end

function Screens:refreshBook()
  local D = self.services.D
  local I = self.services.I
  local UI = self.services.UI
  local Meta = self.services.Meta
  local met = 0
  for tier, def in ipairs(D.TIERS) do
    local seen = Meta.data.seen[tier]
    if seen then met = met + 1 end
    UI.set("Book:img" .. tier, { texture = seen and D.texture(tier) or string.format("assets/animals/%02d_locked.png", tier) })
    UI.text("Book:name" .. tier, seen and I.L(def.name) or I.t("book.unknown"))
  end
  UI.text("Book:count", I.t("book.count", { n = met, m = D.MAX_TIER }))
end

function Screens:refreshMissions()
  local D = self.services.D
  local I = self.services.I
  local UI = self.services.UI
  local Meta = self.services.Meta
  Meta.rollMissions()
  local ms = Meta.data.missions
  for i, id in ipairs(ms.ids) do
    local def = D.mission(id)
    local have = min(ms.progress[id], def.goal)
    local done, claimed = have >= def.goal, ms.claimed[id]
    UI.text("Mission" .. i .. ":text", I.L(def.text):gsub("{n}", UI.fmt(def.goal)))
    UI.text("Mission" .. i .. ":val", UI.fmt(have) .. " / " .. UI.fmt(def.goal))
    UI.set("Mission" .. i .. ":bar", { value = have / def.goal })
    UI.text("Mission" .. i .. ":reward", "+" .. def.reward)
    UI.text("Mission" .. i .. ":btn", claimed and I.t("missions.done") or I.t("missions.claim"))
    UI.set("Mission" .. i .. ":btn", { interactable = done and not claimed })
  end
end

function Screens:refreshDaily()
  local D = self.services.D
  local I = self.services.I
  local UI = self.services.UI
  local Meta = self.services.Meta
  local available = Meta.dailyAvailable()
  local day = Meta.dailyDay()
  for i = 1, #D.DAILY_REWARDS do
    local got = i < day or (i == day and not available)
    UI.text("Day" .. i .. ":label", I.t("daily.day", { n = i }))
    UI.text("Day" .. i .. ":n", "+" .. D.DAILY_REWARDS[i])
    UI.show("Day" .. i .. ":check", got)
    UI.set("Day" .. i, { color = UI.rgb(i == day and available and "#ffe08a" or got and "#d8f5c8" or "#fff3dc") })
  end
  UI.show("Daily:claim", available)
  UI.show("Daily:claimx2", available)
  UI.show("Daily:tomorrow", not available)
end

function Screens:refreshSettings()
  local I = self.services.I
  local UI = self.services.UI
  local Meta = self.services.Meta
  local s = Meta.data.settings
  UI.text("Set:sfx", I.t(s.sfx and "settings.on" or "settings.off"))
  UI.text("Set:music", I.t(s.music and "settings.on" or "settings.off"))
  UI.text("Set:lang", s.locale == "ko" and "한국어" or "English")
end

function Screens:open(name)
  if name == "shop" then
    self:refreshShop()
  elseif name == "book" then
    self:refreshBook()
  elseif name == "missions" then
    self:refreshMissions()
  elseif name == "daily" then
    self:refreshDaily()
  elseif name == "settings" then
    self:refreshSettings()
  end
  self:setDialog(name)
end

function Screens:close()
  self:setDialog(nil)
  if self.mode == "home" then self:refreshHome() end
end

function Screens:handleScreensAction(a, b)
  local I = self.services.I
  local UI = self.services.UI
  local Meta = self.services.Meta
  local Audio = self.services.Audio
  local Board = self.services.Board
  if a == "home" then
    if self.mode == "home" then
      self:close()
    else
      self:leaveGame(function()
        Board.clear()
        self:showHome()
      end)
    end
    return true
  elseif a == "pause" then
    if self.mode == "play" and not self.dialog then
      Meta.save()
      self:open("pause")
    end
    return true
  elseif a == "resume" or a == "close" then
    self:close()
    return true
  elseif a == "open" then
    self:open(b)
    return true
  elseif a == "toggleSfx" or a == "toggleMusic" or a == "toggleLang" then
    local s = Meta.data.settings
    if a == "toggleSfx" then
      s.sfx = not s.sfx
    elseif a == "toggleMusic" then
      s.music = not s.music
    else
      s.locale = s.locale == "ko" and "en" or "ko"
      I.setLocale(s.locale)
      UI.localize()
      self:refreshHome()
    end
    Audio.configure(s.sfx, s.music)
    Meta.save()
    self:refreshSettings()
    return true
  end
  return false
end

Screens.methods = {
  "layout",
  "pointer",
  "applyTheme",
  "refreshWallet",
  "refreshItems",
  "showHome",
  "refreshHome",
  "setDialog",
  "refreshScore",
  "refreshHolder",
  "showOver",
  "openItemDialog",
  "refreshShop",
  "refreshBook",
  "refreshMissions",
  "refreshDaily",
  "refreshSettings",
  "open",
  "close",
  "handleScreensAction",
}
return Screens
