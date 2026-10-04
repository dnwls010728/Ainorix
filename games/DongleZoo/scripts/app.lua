-- The whole game on one entity: screen layout, the play loop (aim, drop, merge, danger
-- line), items, dialogs, shop, missions and the daily gift. Board rules live in
-- scripts/lib/board.lua, numbers in scripts/lib/defs.lua, save data in scripts/lib/meta.lua.
local D = require("scripts.lib.defs")
local I = require("scripts.lib.i18n")
local UI = require("scripts.lib.ui")
local Meta = require("scripts.lib.meta")
local Audio = require("scripts.lib.audio")
local Ads = require("scripts.lib.ads")
local Board = require("scripts.lib.board")

local App = {}

local FRAME = 1 / 60
local SPRITE_SCALE = 2 * 256 / 200 -- sprite size per unit of radius (the body fills 200 of 256 px)
local POPS = 6
local DIALOGS = { "pause", "over", "shop", "book", "missions", "daily", "settings", "item" }

local floor, min, max = math.floor, math.min, math.max

-- ---------------------------------------------------------------- start

function App:onStart()
  Meta.load()
  if Meta.data.settings.locale == "" then
    -- first launch: follow the device language (players older than the host bridge start in English)
    local bridge = rawget(_G, "host")
    local lang = self.params.locale or (bridge and bridge.language()) or "en"
    Meta.data.settings.locale = lang == "ko" and "ko" or "en"
  end
  I.setLocale(Meta.data.settings.locale)
  Audio.configure(Meta.data.settings.sfx, Meta.data.settings.music)

  self.camera = scene.find("Camera")
  self.canvas = scene.find("Canvas")
  self.background = scene.find("Background")
  self.holder = scene.find("Holder")
  self.guide = scene.find("Guide")
  self.line = scene.find("Line")
  self.mode = "home" -- home | play | over
  self.dialog = nil
  self.score, self.combo, self.comboLeft = 0, 0, 0
  self.fx = {} -- { id, frames left }
  self.pop = 0
  self.frame = 0
  self.vw, self.vh = 0, 0
  Board.clear()

  UI.localize()
  self:layout()
  self:applyTheme()
  self:showHome()
  Ads.ready()
end

-- ---------------------------------------------------------------- layout

-- Fits the jar and the UI to the screen: 96 reference pixels per world unit at every shape.
-- Narrow (phone) screens scale with the width and gain room above the jar; wide screens
-- scale with the height and show more background at the sides.
function App:layout()
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

-- Pointer position in world units.
function App:pointer()
  local mx, my = input.mouse()
  return (mx - 0.5) * 2 * self.ortho * self.aspect, self.camY + (0.5 - my) * 2 * self.ortho
end

function App:applyTheme()
  local th = D.theme(Meta.data.theme)
  scene.set(self.background, "Sprite", { texture = "assets/bg/" .. th.id .. ".png" })
  self.lineColor = UI.rgb(th.line)
  scene.set(self.line, "Sprite", { color = self.lineColor })
end

-- ---------------------------------------------------------------- screens

function App:refreshWallet()
  UI.text("Wallet:n", UI.fmt(Meta.data.coins))
end

function App:refreshItems()
  for _, it in ipairs(D.ITEMS) do
    local n = Meta.data.items[it.id]
    UI.text("Item:" .. it.id .. ":n", n > 0 and tostring(n) or "+")
    UI.set("Item:" .. it.id .. ":badge", { color = UI.rgb(n > 0 and "#ff6b6b" or "#4caf50") })
  end
end

function App:showHome()
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

function App:refreshHome()
  UI.text("Home:best", I.t("home.best", { n = UI.fmt(Meta.data.best) }))
  UI.show("Home:dot:daily", Meta.dailyAvailable())
  UI.show("Home:dot:missions", Meta.missionsReady() > 0)
end

function App:setDialog(name)
  if self.dialog == name then return end
  for _, d in ipairs(DIALOGS) do UI.show("Dlg:" .. d, d == name) end
  self.dialog = name
  if self.mode == "play" then game.pause(name ~= nil) end
end

-- ---------------------------------------------------------------- a game

local function hash(s)
  local h = 5381
  for i = 1, #s do h = (h * 33 + s:byte(i)) % 2147483647 end
  return h
end

local function randomTier()
  local total = 0
  for _, w in ipairs(D.DROP_WEIGHTS) do total = total + w end
  local roll = math.random() * total
  for tier, w in ipairs(D.DROP_WEIGHTS) do
    roll = roll - w
    if roll < 0 then return tier end
  end
  return 1
end

function App:newGame()
  -- The session's Lua random seed is fixed; mix in the day, the games played and the
  -- moment the player pressed Play so every game deals different animals.
  math.randomseed(hash(time.date()) + Meta.data.stats.games * 7919 + time.frame())
  Board.clear()
  for _, f in ipairs(self.fx) do
    if scene.exists(f.id) then scene.destroy(f.id) end
  end
  self.fx = {}
  self.mode = "play"
  self:setDialog(nil)
  game.pause(false)
  self.score, self.combo, self.comboLeft, self.bestCombo = 0, 0, 0, 0
  self.cur, self.nxt = randomTier(), randomTier()
  self.cooldown, self.aiming, self.target = 0, false, nil
  self.holderX = 0
  self.revived, self.finalized, self.doubled, self.wasBest = false, false, false, false
  self.overFrames, self.warned = nil, false
  UI.show("Home", false)
  UI.show("HUD", true)
  UI.show("HUD:pick", false)
  UI.show("HUD:hint", not Meta.data.tutorial)
  UI.text("HUD:combo", "")
  self:refreshScore()
  self:refreshItems()
  self:refreshWallet()
  self:refreshHolder()
end

function App:refreshScore()
  UI.text("HUD:score", UI.fmt(self.score))
  UI.text("HUD:bestv", UI.fmt(max(Meta.data.best, self.score)))
end

function App:refreshHolder()
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

function App:drop()
  Board.spawn(self.cur, self.holderX, D.DROP_Y, false)
  Audio.sfx("drop", 1.25 - self.cur * 0.07)
  self.cur, self.nxt = self.nxt, randomTier()
  self.cooldown = D.DROP_COOLDOWN
  Meta.data.stats.drops = Meta.data.stats.drops + 1
  Meta.bump("drops", 1)
  if Meta.data.stats.drops % 10 == 0 then Meta.save() end -- a phone may close the app at any moment
  if not Meta.data.tutorial then
    Meta.data.tutorial = true
    UI.show("HUD:hint", false)
    Meta.save()
  end
  self:refreshHolder()
end

-- ---------------------------------------------------------------- effects

-- World point -> reference pixels from the top centre of the screen.
function App:toUI(x, y)
  return x * D.PX, (self.camY + self.ortho - y) * D.PX
end

function App:popText(text, x, y, color, size)
  self.pop = self.pop % POPS + 1
  local id = UI.id("Pop" .. self.pop)
  local ux, uy = self:toUI(x, y)
  tween.stop(id, "UIText")
  scene.set(id, "UIText", { text = text, x = ux, y = uy, opacity = 1, size = size or 34, color = UI.rgb(color or "#ffffff") })
  tween.to(id, "UIText", { y = uy - 80, opacity = 0 }, 0.9, { ease = "outQuad" })
end

function App:burst(x, y, tier, big)
  local r = D.TIERS[tier].r
  local id = scene.instantiate("prefabs/fx_burst.prefab.json", { position = { x = x, y = y, z = 0 } })
  local c = UI.rgb(D.TIERS[tier].color)
  scene.set(id, "ParticleEmitter", { startColor = c, endColor = c, speed = 2.5 + r * 3, startSize = 0.12 + r * 0.22 })
  particles.burst(id, big and 22 or 12)
  self.fx[#self.fx + 1] = { id = id, left = 80 }
  local ring = scene.instantiate("prefabs/fx_ring.prefab.json", { position = { x = x, y = y, z = 0 } })
  scene.set(ring, "Sprite", { width = r * 2, height = r * 2, color = c })
  tween.to(ring, "Sprite", { width = r * 4.4, height = r * 4.4, opacity = 0 }, 0.4, { ease = "outQuad", scaled = true })
  self.fx[#self.fx + 1] = { id = ring, left = 40 }
end

function App:updateFx()
  if game.paused() then return end
  local fx = self.fx
  for i = #fx, 1, -1 do
    fx[i].left = fx[i].left - 1
    if fx[i].left <= 0 then
      if scene.exists(fx[i].id) then scene.destroy(fx[i].id) end
      table.remove(fx, i)
    end
  end
end

-- ---------------------------------------------------------------- merging and scoring

function App:onMerge(tier, x, y, newId)
  self.combo = self.comboLeft > 0 and self.combo + 1 or 1
  self.comboLeft = D.COMBO_WINDOW
  self.bestCombo = max(self.bestCombo, self.combo)
  local made = tier + 1
  local base = newId and D.TIERS[made].score or D.WHALE_BONUS
  local gain = floor(base * (1 + 0.25 * min(self.combo - 1, 6)) + 0.5)
  self.score = self.score + gain
  Meta.data.stats.merges = Meta.data.stats.merges + 1
  Meta.bump("merges", 1)
  Meta.bump("combo", self.combo)

  local shown = newId and made or tier
  self:burst(x, y, shown, shown >= 7)
  self:popText("+" .. gain, x, y + D.TIERS[shown].r * 0.5, self.combo > 1 and "#ffe066" or "#ffffff", 30 + min(shown, 8) * 2)
  if newId then
    Audio.sfx(made >= 7 and "merge_big" or "merge", min(1.9, 0.85 + made * 0.04 + (self.combo - 1) * 0.07))
    Meta.bump("tier" .. made, 1)
    if Meta.see(made) then
      UI.toast(I.t("toast.newfriend", { name = I.L(D.TIERS[made].name), n = D.NEW_ANIMAL_COINS }), 3)
      Audio.sfx("fanfare")
      self:refreshWallet()
      Meta.save()
    end
  else
    Meta.data.stats.whales = Meta.data.stats.whales + 1
    UI.toast(I.t("toast.whale", { n = gain }), 3)
    Audio.sfx("fanfare")
  end
  if self.combo >= 2 then UI.text("HUD:combo", I.t("hud.combo", { n = self.combo })) end
  self:refreshScore()
end

-- ---------------------------------------------------------------- game over

function App:earned()
  return floor(self.score * D.COINS_PER_SCORE)
end

-- Books the game into the save once: coins, best score, missions.
function App:finalize()
  if self.finalized then return end
  self.finalized = true
  self.wasBest = self.score > Meta.data.best
  Meta.applyGame(self.score)
  Ads.gameFinished()
  self:refreshWallet()
end

function App:gameOver()
  self.mode = "over"
  self.overFrames = 50
  self.target = nil
  UI.show("HUD:pick", false)
  scene.set(self.holder, "Sprite", { visible = false })
  scene.set(self.guide, "Sprite", { visible = false })
  Audio.sfx("gameover")
end

function App:showOver()
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

function App:revive()
  self.revived = true
  local gone = Board.clearAbove((D.FLOOR_Y + D.LINE_Y) / 2)
  for _, g in ipairs(gone) do self:burst(g.x, g.y, g.tier, false) end
  Audio.sfx("item")
  self.mode = "play"
  self:setDialog(nil)
  game.pause(false)
  self.cooldown, self.overFrames, self.warned = 0.3, nil, false
  UI.toast(I.t("toast.revived"))
  self:refreshHolder()
end

-- ---------------------------------------------------------------- items

function App:useItem(id)
  if self.mode ~= "play" or self.dialog then return end
  if self.target == id then -- pressing the item again cancels it
    self:cancelPick()
    return
  end
  if Meta.data.items[id] <= 0 then
    self:openItemDialog(id)
    return
  end
  if id == "shake" then
    if Board.count() == 0 then
      Audio.sfx("deny")
      return
    end
    Meta.useItem(id)
    Board.shake()
    Audio.sfx("shake")
    self:refreshItems()
    return
  end
  self.target = id
  UI.show("HUD:pick", true)
  UI.text("HUD:pick:text", I.t("hud.pick." .. id))
  Audio.sfx("click")
  self:refreshHolder()
end

function App:cancelPick()
  self.target = nil
  UI.show("HUD:pick", false)
  self:refreshHolder()
end

-- A press on an animal while the hammer or the wand is armed.
function App:pick(x, y)
  local p = Board.at(x, y)
  if not p then return end
  local id = self.target
  local px, py = Board.pos(p.id)
  if id == "wand" and p.tier >= D.MAX_TIER then
    Audio.sfx("deny")
    return
  end
  Meta.useItem(id)
  Board.remove(p.id)
  if id == "hammer" then
    self:burst(px, py, p.tier, false)
    Audio.sfx("hammer")
  else
    Board.spawn(p.tier + 1, px, py, true)
    self:burst(px, py, p.tier + 1, true)
    Audio.sfx("item")
    Meta.bump("tier" .. (p.tier + 1), 1)
    if Meta.see(p.tier + 1) then
      UI.toast(I.t("toast.newfriend", { name = I.L(D.TIERS[p.tier + 1].name), n = D.NEW_ANIMAL_COINS }), 3)
      self:refreshWallet()
    end
  end
  self:refreshItems()
  self:cancelPick()
  self.cooldown = max(self.cooldown, 0.25)
end

function App:openItemDialog(id)
  local it = D.item(id)
  self.itemWanted = id
  UI.text("ItemDlg:title", I.t("item.title", { name = I.L(it.name) }))
  UI.text("ItemDlg:desc", I.L(it.desc))
  UI.set("ItemDlg:icon", { texture = "assets/ui/" .. id .. ".png" })
  UI.text("ItemDlg:buy", I.t("item.buy", { n = it.price }))
  self:setDialog("item")
end

-- ---------------------------------------------------------------- dialogs

function App:refreshShop()
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

function App:refreshBook()
  local met = 0
  for tier, def in ipairs(D.TIERS) do
    local seen = Meta.data.seen[tier]
    if seen then met = met + 1 end
    UI.set("Book:img" .. tier, { texture = seen and D.texture(tier) or string.format("assets/animals/%02d_locked.png", tier) })
    UI.text("Book:name" .. tier, seen and I.L(def.name) or I.t("book.unknown"))
  end
  UI.text("Book:count", I.t("book.count", { n = met, m = D.MAX_TIER }))
end

function App:refreshMissions()
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

function App:refreshDaily()
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

function App:refreshSettings()
  local s = Meta.data.settings
  UI.text("Set:sfx", I.t(s.sfx and "settings.on" or "settings.off"))
  UI.text("Set:music", I.t(s.music and "settings.on" or "settings.off"))
  UI.text("Set:lang", s.locale == "ko" and "한국어" or "English")
end

function App:open(name)
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

function App:close()
  self:setDialog(nil)
  if self.mode == "home" then self:refreshHome() end
end

function App:claimDaily(doubled)
  local n = Meta.claimDaily(doubled)
  if n <= 0 then return end
  Audio.sfx("gift")
  UI.toast(I.t("toast.coins", { n = UI.fmt(n) }))
  self:refreshWallet()
  self:refreshDaily()
end

-- ---------------------------------------------------------------- actions (buttons)

function App:leaveGame(next)
  self:finalize()
  self:setDialog(nil)
  Ads.interstitial(next)
end

function App:onAction(a, b)
  if Ads.action(a) then return end
  if Ads.busy() then return end
  Audio.sfx("click")
  if a == "play" or a == "restart" then
    if self.mode == "play" and self.score > 0 then self:finalize() end
    self:newGame()
  elseif a == "retry" then
    self:leaveGame(function() self:newGame() end)
  elseif a == "home" then
    if self.mode == "home" then
      self:close()
    else
      self:leaveGame(function()
        Board.clear()
        self:showHome()
      end)
    end
  elseif a == "pause" then
    if self.mode == "play" and not self.dialog then
      Meta.save()
      self:open("pause")
    end
  elseif a == "resume" or a == "close" then
    self:close()
  elseif a == "open" then
    self:open(b)
  elseif a == "item" then
    self:useItem(b)
  elseif a == "pickCancel" then
    self:cancelPick()
  elseif a == "revive" then
    Ads.rewarded("revive", function(ok)
      if ok then self:revive() end
    end)
  elseif a == "double" then
    Ads.rewarded("double_coins", function(ok)
      if not ok or self.doubled then return end
      self:finalize()
      self.doubled = true
      Meta.addCoins(self:earned())
      Meta.save()
      Audio.sfx("coin")
      self:refreshWallet()
      self:showOver()
    end)
  elseif a == "itemBuy" then
    if Meta.buyItem(self.itemWanted) then
      Audio.sfx("coin")
      UI.toast(I.t("toast.item", { name = I.L(D.item(self.itemWanted).name) }))
      self:refreshWallet()
      self:refreshItems()
      self:close()
    else
      Audio.sfx("deny")
      UI.toast(I.t("toast.poor"))
    end
  elseif a == "itemFree" then
    local id = self.itemWanted
    Ads.rewarded("free_item", function(ok)
      if not ok then return end
      Meta.grantItem(id, 1)
      UI.toast(I.t("toast.item", { name = I.L(D.item(id).name) }))
      self:refreshItems()
      self:close()
    end)
  elseif a == "theme" then
    if Meta.hasTheme(b) then
      Meta.selectTheme(b)
    elseif Meta.buyTheme(b) then
      Audio.sfx("coin")
      UI.toast(I.t("toast.theme", { name = I.L(D.theme(b).name) }))
    else
      Audio.sfx("deny")
      UI.toast(I.t("toast.poor"))
    end
    self:applyTheme()
    self:refreshWallet()
    self:refreshShop()
  elseif a == "product" then
    local p = D.product(b)
    Ads.purchase(p, function(ok)
      if not ok then return end
      Meta.purchaseApplied(p.id)
      Audio.sfx("coin")
      UI.toast(I.t("toast.thanks"))
      self:refreshWallet()
      self:refreshShop()
    end)
  elseif a == "claimMission" then
    local id = Meta.data.missions.ids[b]
    local n = id and Meta.claimMission(id) or 0
    if n > 0 then
      Audio.sfx("coin")
      UI.toast(I.t("toast.coins", { n = n }))
      self:refreshWallet()
      self:refreshMissions()
    end
  elseif a == "dailyClaim" then
    self:claimDaily(false)
  elseif a == "dailyClaim2" then
    Ads.rewarded("daily_double", function(ok) self:claimDaily(ok) end)
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
  end
end

-- ---------------------------------------------------------------- frame

function App:updatePlay(dt)
  local worst = Board.step(dt, function(tier, x, y, newId) self:onMerge(tier, x, y, newId) end)

  if self.comboLeft > 0 then
    self.comboLeft = self.comboLeft - dt
    if self.comboLeft <= 0 then
      self.combo = 0
      UI.text("HUD:combo", "")
    end
  end

  -- the danger line: blinks while something stays above it
  local warn = worst > 0.25
  if warn then
    local a = 0.55 + 0.45 * math.sin(self.frame * 0.35)
    scene.set(self.line, "Sprite", { opacity = a, color = UI.rgb("#ff3b3b") })
    if not self.warned then Audio.sfx("warn") end
  elseif self.warned then
    scene.set(self.line, "Sprite", { opacity = 0.45, color = self.lineColor })
  end
  self.warned = warn
  if worst >= D.OVER_TIME then
    self:gameOver()
    return
  end

  if self.cooldown > 0 then
    self.cooldown = self.cooldown - dt
    if self.cooldown <= 0 then self:refreshHolder() end
  end

  local x, y = self:pointer()
  local inside = y < D.PLAY_TOP and y > D.FLOOR_Y - 0.1
  if self.target then
    if input.pressed("MouseLeft") and inside then self:pick(x, y) end
    return
  end
  if input.pressed("MouseLeft") and inside and self.cooldown <= 0 then self.aiming = true end
  if self.aiming then
    self.holderX = x
    self:refreshHolder()
    if not input.down("MouseLeft") then
      self.aiming = false
      self:drop()
    end
  end
end

function App:onUpdate(dt)
  self.frame = self.frame + 1
  self:layout()
  Ads.update()
  UI.updateToasts()
  self:updateFx()

  if input.pressed("Escape") and not Ads.busy() then
    if self.dialog and self.dialog ~= "over" then
      self:close()
    elseif self.mode == "play" then
      self:open("pause")
    end
  end

  if self.mode == "play" then
    if not self.dialog and not Ads.busy() and dt > 0 then self:updatePlay(dt) end
  elseif self.mode == "over" and self.overFrames then
    Board.step(dt, function(tier, x, y, newId) self:onMerge(tier, x, y, newId) end)
    self.overFrames = self.overFrames - 1
    if self.overFrames <= 0 then
      self.overFrames = nil
      self:showOver()
    end
  end
end

return App
