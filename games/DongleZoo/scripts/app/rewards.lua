-- Stateless rewards methods. The App owner explicitly supplies self.services.
local Rewards = {}
local floor = math.floor

function Rewards:earned()
  local D = self.services.D
  return floor(self.score * D.COINS_PER_SCORE)
end

function Rewards:finalize()
  local Meta = self.services.Meta
  local Ads = self.services.Ads
  if self.finalized then return end
  self.finalized = true
  self.wasBest = self.score > Meta.data.best
  Meta.applyGame(self.score)
  Ads.gameFinished()
  self:refreshWallet()
end

function Rewards:claimDaily(doubled)
  local I = self.services.I
  local UI = self.services.UI
  local Meta = self.services.Meta
  local Audio = self.services.Audio
  local n = Meta.claimDaily(doubled)
  if n <= 0 then return end
  Audio.sfx("gift")
  UI.toast(I.t("toast.coins", { n = UI.fmt(n) }))
  self:refreshWallet()
  self:refreshDaily()
end

function Rewards:leaveGame(next)
  local Ads = self.services.Ads
  self:finalize()
  self:setDialog(nil)
  Ads.interstitial(next)
end

function Rewards:handleRewardsAction(a, b)
  local D = self.services.D
  local I = self.services.I
  local UI = self.services.UI
  local Meta = self.services.Meta
  local Audio = self.services.Audio
  local Ads = self.services.Ads
  if a == "revive" then
    Ads.rewarded("revive", function(ok)
      if ok then self:revive() end
    end)
    return true
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
    return true
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
    return true
  elseif a == "itemFree" then
    local id = self.itemWanted
    Ads.rewarded("free_item", function(ok)
      if not ok then return end
      Meta.grantItem(id, 1)
      UI.toast(I.t("toast.item", { name = I.L(D.item(id).name) }))
      self:refreshItems()
      self:close()
    end)
    return true
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
    return true
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
    return true
  elseif a == "claimMission" then
    local id = Meta.data.missions.ids[b]
    local n = id and Meta.claimMission(id) or 0
    if n > 0 then
      Audio.sfx("coin")
      UI.toast(I.t("toast.coins", { n = n }))
      self:refreshWallet()
      self:refreshMissions()
    end
    return true
  elseif a == "dailyClaim" then
    self:claimDaily(false)
    return true
  elseif a == "dailyClaim2" then
    Ads.rewarded("daily_double", function(ok) self:claimDaily(ok) end)
    return true
  end
  return false
end

Rewards.methods = {
  "earned",
  "finalize",
  "claimDaily",
  "leaveGame",
  "handleRewardsAction",
}
return Rewards
