-- English and Korean UI strings.
local I = {}

local locale = "en"

function I.setLocale(l)
  locale = l == "ko" and "ko" or "en"
end
function I.getLocale()
  return locale
end

-- Picks the current language from a {en=, ko=} text.
function I.L(text)
  return text[locale] or text.en
end

local STR = {
  ["common.back"] = { en = "Back", ko = "뒤로" },
  ["common.close"] = { en = "Close", ko = "닫기" },
  ["common.cancel"] = { en = "Cancel", ko = "취소" },
  ["common.max"] = { en = "MAX", ko = "최대" },

  ["title.tagline"] = { en = "Keep the little light glowing!", ko = "작은 불빛을 지켜줘요!" },
  ["menu.play"] = { en = "Play", ko = "플레이" },
  ["menu.upgrades"] = { en = "Upgrades", ko = "강화" },
  ["menu.achievements"] = { en = "Achievements", ko = "업적" },
  ["menu.shop"] = { en = "Shop", ko = "상점" },
  ["menu.settings"] = { en = "Settings", ko = "설정" },

  ["menu.daily"] = { en = "Daily gift", ko = "오늘의 선물" },
  ["daily.title"] = { en = "Daily Gift", ko = "오늘의 선물" },
  ["daily.desc"] = { en = "Welcome back! A little something for you.", ko = "어서 와요! 작은 선물이 도착했어요." },
  ["daily.claim"] = { en = "Yay, thanks!", ko = "고마워요!" },
  ["daily.claimx2"] = { en = "Get x2 (ad)", ko = "2배로 받기 (광고)" },
  ["toast.claimed"] = { en = "+{n} Glims", ko = "+{n} 글림" },

  ["play.title"] = { en = "Pick your buddy", ko = "친구를 골라요" },
  ["play.keeper"] = { en = "Lantern keeper", ko = "등불지기" },
  ["play.stage"] = { en = "Stage", ko = "스테이지" },
  ["play.start"] = { en = "Start", ko = "시작" },
  ["play.unlock"] = { en = "Unlock", ko = "해금" },
  ["play.winReq"] = { en = "Win {stage} to open", ko = "{stage} 클리어하면 열려요" },
  ["play.selected"] = { en = "Selected", ko = "선택됨" },

  ["upgrades.title"] = { en = "Cozy upgrades", ko = "아늑한 강화" },
  ["upgrades.buy"] = { en = "Buy", ko = "구매" },

  ["ach.title"] = { en = "Achievements", ko = "업적" },
  ["ach.stats"] = { en = "Your adventure so far", ko = "지금까지의 모험" },
  ["ach.reward"] = { en = "+{n} Glims", ko = "+{n} 글림" },
  ["stat.runs"] = { en = "Runs", ko = "플레이 횟수" },
  ["stat.wins"] = { en = "Victories", ko = "승리" },
  ["stat.kills"] = { en = "Enemies defeated", ko = "처치한 적" },
  ["stat.bossKills"] = { en = "Bosses defeated", ko = "처치한 보스" },
  ["stat.bestTime"] = { en = "Best time", ko = "최장 생존" },
  ["stat.braziersLit"] = { en = "Braziers lit", ko = "밝힌 화로" },
  ["stat.glimsEarned"] = { en = "Glims earned", ko = "모은 글림" },
  ["stat.maxLevel"] = { en = "Highest level", ko = "최고 레벨" },
  ["stat.evolutions"] = { en = "Evolutions", ko = "진화 횟수" },

  ["shop.title"] = { en = "Shop", ko = "상점" },
  ["shop.demo"] = { en = "Demo build: no real payments are made.", ko = "데모 빌드예요. 실제 결제는 되지 않아요." },
  ["shop.owned"] = { en = "Owned", ko = "보유 중" },
  ["shop.thanks"] = { en = "Thank you so much!", ko = "정말 고마워요!" },
  ["shop.failed"] = { en = "Purchase cancelled", ko = "구매가 취소되었어요" },

  ["settings.title"] = { en = "Settings", ko = "설정" },
  ["settings.music"] = { en = "Music", ko = "음악" },
  ["settings.sfx"] = { en = "Sound effects", ko = "효과음" },
  ["settings.shake"] = { en = "Screen shake", ko = "화면 흔들림" },
  ["settings.damage"] = { en = "Damage numbers", ko = "피해량 표시" },
  ["settings.language"] = { en = "Language", ko = "언어" },
  ["settings.reset"] = { en = "Reset progress", ko = "진행 초기화" },
  ["settings.resetAsk"] = { en = "Erase everything and start fresh? This cannot be undone.", ko = "모든 진행 상황이 지워져요. 되돌릴 수 없어요!" },
  ["settings.resetYes"] = { en = "Yes, start fresh", ko = "네, 처음부터 할래요" },
  ["common.on"] = { en = "On", ko = "켜짐" },
  ["common.off"] = { en = "Off", ko = "꺼짐" },

  ["hud.lv"] = { en = "Lv {n}", ko = "Lv {n}" },

  ["levelup.title"] = { en = "Level Up!", ko = "레벨 업!" },
  ["levelup.sub"] = { en = "Pick a treat!", ko = "하나 골라요!" },
  ["levelup.new"] = { en = "NEW", ko = "신규" },
  ["levelup.lv"] = { en = "Lv {n}", ko = "Lv {n}" },
  ["levelup.evolve"] = { en = "EVOLVE", ko = "진화" },
  ["levelup.reroll"] = { en = "Reroll ({n})", ko = "다시 뽑기 ({n})" },
  ["levelup.rerollAd"] = { en = "Reroll (ad)", ko = "다시 뽑기 (광고)" },
  ["kind.heal"] = { en = "Restore", ko = "회복" },
  ["kind.glims"] = { en = "Treasure", ko = "보물" },

  ["chest.title"] = { en = "Treasure!", ko = "보물이다!" },
  ["chest.sub"] = { en = "Look what you found!", ko = "이런 걸 찾았어요!" },
  ["chest.continue"] = { en = "Continue", ko = "계속" },

  ["pause.title"] = { en = "Taking a break", ko = "잠깐 쉬어요" },
  ["pause.resume"] = { en = "Resume", ko = "계속하기" },
  ["pause.quit"] = { en = "Go home", ko = "그만하기" },
  ["pause.quitAsk"] = { en = "Head home now? You will keep the Glims you found.", ko = "이제 돌아갈까요? 모은 글림은 그대로 받아요." },
  ["pause.build"] = { en = "Current build", ko = "현재 빌드" },
  ["pause.weapons"] = { en = "Weapons", ko = "무기" },
  ["pause.passives"] = { en = "Passives", ko = "패시브" },
  ["pause.empty"] = { en = "Nothing yet", ko = "아직 없음" },

  ["dying.title"] = { en = "The lantern is flickering...", ko = "등불이 깜빡거려요..." },
  ["dying.sub"] = { en = "Want to give it another go?", ko = "다시 힘내 볼까요?" },
  ["dying.revive"] = { en = "Revive", ko = "부활" },
  ["dying.reviveAd"] = { en = "Revive (ad)", ko = "부활 (광고)" },
  ["dying.giveUp"] = { en = "Go home", ko = "그만하기" },

  ["victory.title"] = { en = "Good morning!", ko = "아침이 밝았어요!" },
  ["victory.sub"] = { en = "The sun is up and the shadows are sleepy. You did it!", ko = "해가 떴고 그림자들은 졸려해요. 해냈어요!" },
  ["victory.endless"] = { en = "Keep playing", ko = "계속 놀기" },
  ["victory.finish"] = { en = "All done", ko = "마무리" },

  ["result.victory"] = { en = "Good morning!", ko = "아침이 밝았어요!" },
  ["result.defeat"] = { en = "The lantern went out...", ko = "등불이 꺼졌어요..." },
  ["result.victorySub"] = { en = "What a wonderful night!", ko = "멋진 밤이었어요!" },
  ["result.defeatSub"] = { en = "Let's try again!", ko = "다시 도전해 볼까요?" },
  ["result.time"] = { en = "Time", ko = "생존 시간" },
  ["result.kills"] = { en = "Kills", ko = "처치" },
  ["result.level"] = { en = "Level", ko = "레벨" },
  ["result.braziers"] = { en = "Braziers", ko = "화로" },
  ["result.glims"] = { en = "Glims earned", ko = "획득 글림" },
  ["result.damage"] = { en = "Damage dealt", ko = "입힌 피해" },
  ["result.newAch"] = { en = "New badges!", ko = "새로운 업적!" },
  ["result.double"] = { en = "Double Glims (ad)", ko = "글림 2배 (광고)" },
  ["result.doubled"] = { en = "Doubled!", ko = "2배 적용!" },
  ["result.again"] = { en = "Try again", ko = "다시 도전" },
  ["result.upgrades"] = { en = "Upgrades", ko = "강화" },
  ["result.menu"] = { en = "Menu", ko = "메뉴" },

  ["toast.noAd"] = { en = "No reward this time", ko = "이번엔 보상이 없어요" },
  ["toast.poor"] = { en = "Not enough Glims yet", ko = "글림이 조금 모자라요" },
  ["banner.warning"] = { en = "Uh-oh! Here comes...", ko = "앗! 등장..." },

  -- mock ad / purchase overlays (the web build's demo platform provider)
  ["ad.rewarded"] = { en = "Rewarded ad (demo)", ko = "보상형 광고 (데모)" },
  ["ad.rewardedSub"] = { en = "Watch to the end to earn your reward.", ko = "끝까지 보면 보상을 받아요." },
  ["ad.skip"] = { en = "Skip", ko = "건너뛰기" },
  ["ad.break"] = { en = "Ad break (demo)", ko = "광고 시간 (데모)" },
  ["ad.purchase"] = { en = "Demo purchase", ko = "데모 구매" },
  ["ad.purchaseSub"] = { en = "{name} {price} — no real money is charged", ko = "{name} {price} — 실제 결제는 되지 않아요" },
  ["ad.buy"] = { en = "Buy", ko = "구매" },
}

function I.t(key, vars)
  local entry = STR[key]
  local s = entry and (entry[locale] or entry.en) or key
  if vars then
    for k, v in pairs(vars) do
      s = s:gsub("{" .. k .. "}", function() return tostring(v) end)
    end
  end
  return s
end

return I
