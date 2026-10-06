-- Stateless gameplay methods. The App owner explicitly supplies self.services.
local Gameplay = {}
local min, max = math.min, math.max

local function hash(s)
  local h = 5381
  for i = 1, #s do h = (h * 33 + s:byte(i)) % 2147483647 end
  return h
end

local function randomTier(D)
  local total = 0
  for _, w in ipairs(D.DROP_WEIGHTS) do total = total + w end
  local roll = math.random() * total
  for tier, w in ipairs(D.DROP_WEIGHTS) do
    roll = roll - w
    if roll < 0 then return tier end
  end
  return 1
end

function Gameplay:newGame()
  local D = self.services.D
  local UI = self.services.UI
  local Meta = self.services.Meta
  local Board = self.services.Board
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
  self.cur, self.nxt = randomTier(D), randomTier(D)
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

function Gameplay:drop()
  local D = self.services.D
  local UI = self.services.UI
  local Meta = self.services.Meta
  local Audio = self.services.Audio
  local Board = self.services.Board
  Board.spawn(self.cur, self.holderX, D.DROP_Y, false)
  Audio.sfx("drop", 1.25 - self.cur * 0.07)
  self.cur, self.nxt = self.nxt, randomTier(D)
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

function Gameplay:onMerge(tier, x, y, newId)
  local D = self.services.D
  local I = self.services.I
  local UI = self.services.UI
  local Meta = self.services.Meta
  local Audio = self.services.Audio
  local Scoring = self.services.Scoring
  self.combo = self.comboLeft > 0 and self.combo + 1 or 1
  self.comboLeft = D.COMBO_WINDOW
  self.bestCombo = max(self.bestCombo, self.combo)
  local made = tier + 1
  local base = newId and D.TIERS[made].score or D.WHALE_BONUS
  local gain = Scoring.mergePoints(base, self.combo)
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

function Gameplay:gameOver()
  local UI = self.services.UI
  local Audio = self.services.Audio
  self.mode = "over"
  self.overFrames = 50
  self.target = nil
  UI.show("HUD:pick", false)
  scene.set(self.holder, "Sprite", { visible = false })
  scene.set(self.guide, "Sprite", { visible = false })
  Audio.sfx("gameover")
end

function Gameplay:revive()
  local D = self.services.D
  local I = self.services.I
  local UI = self.services.UI
  local Audio = self.services.Audio
  local Board = self.services.Board
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

function Gameplay:useItem(id)
  local I = self.services.I
  local UI = self.services.UI
  local Meta = self.services.Meta
  local Audio = self.services.Audio
  local Board = self.services.Board
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

function Gameplay:cancelPick()
  local UI = self.services.UI
  self.target = nil
  UI.show("HUD:pick", false)
  self:refreshHolder()
end

function Gameplay:pick(x, y)
  local D = self.services.D
  local I = self.services.I
  local UI = self.services.UI
  local Meta = self.services.Meta
  local Audio = self.services.Audio
  local Board = self.services.Board
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

function Gameplay:updatePlay(dt)
  local D = self.services.D
  local UI = self.services.UI
  local Audio = self.services.Audio
  local Board = self.services.Board
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

function Gameplay:handleGameplayAction(a, b)
  if a == "play" or a == "restart" then
    if self.mode == "play" and self.score > 0 then self:finalize() end
    self:newGame()
    return true
  elseif a == "retry" then
    self:leaveGame(function() self:newGame() end)
    return true
  elseif a == "item" then
    self:useItem(b)
    return true
  elseif a == "pickCancel" then
    self:cancelPick()
    return true
  end
  return false
end

Gameplay.methods = {
  "newGame",
  "drop",
  "onMerge",
  "gameOver",
  "revive",
  "useItem",
  "cancelPick",
  "pick",
  "updatePlay",
  "handleGameplayAction",
}
return Gameplay
