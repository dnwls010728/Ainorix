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
  ["common.close"] = { en = "Close", ko = "닫기" },
  ["common.cancel"] = { en = "Cancel", ko = "취소" },
  ["common.ok"] = { en = "OK", ko = "확인" },

  ["home.title"] = { en = "Dongle Zoo", ko = "동글 동물원" },
  ["home.tagline"] = { en = "Drop, merge, grow!", ko = "떨어뜨리고, 합치고, 키워요!" },
  ["home.play"] = { en = "Play", ko = "시작하기" },
  ["home.best"] = { en = "Best {n}", ko = "최고 {n}" },
  ["home.shop"] = { en = "Shop", ko = "상점" },
  ["home.book"] = { en = "Friends", ko = "도감" },
  ["home.missions"] = { en = "Missions", ko = "미션" },
  ["home.daily"] = { en = "Gift", ko = "선물" },
  ["home.settings"] = { en = "Settings", ko = "설정" },

  ["hud.next"] = { en = "NEXT", ko = "다음" },
  ["hud.best"] = { en = "BEST", ko = "최고" },
  ["hud.combo"] = { en = "Combo x{n}", ko = "{n} 콤보!" },
  ["hud.tutorial"] = { en = "Drag to aim, let go to drop.\nTwo of a kind become a bigger friend!", ko = "드래그로 조준하고 손을 떼면 떨어져요.\n같은 친구 둘이 만나면 더 커져요!" },
  ["hud.pick.hammer"] = { en = "Tap an animal to send it home", ko = "집에 보낼 동물을 눌러요" },
  ["hud.pick.wand"] = { en = "Tap an animal to grow it", ko = "키울 동물을 눌러요" },
  ["hud.cancel"] = { en = "Cancel", ko = "취소" },

  ["pause.title"] = { en = "Paused", ko = "일시정지" },
  ["pause.resume"] = { en = "Resume", ko = "계속하기" },
  ["pause.restart"] = { en = "Restart", ko = "다시 시작" },
  ["pause.home"] = { en = "Home", ko = "홈으로" },

  ["over.title"] = { en = "Game Over", ko = "게임 종료" },
  ["over.newbest"] = { en = "New Best!", ko = "신기록!" },
  ["over.score"] = { en = "Score", ko = "점수" },
  ["over.coins"] = { en = "+{n}", ko = "+{n}" },
  ["over.revive"] = { en = "Continue (Ad)", ko = "이어하기 (광고)" },
  ["over.double"] = { en = "Coins x2 (Ad)", ko = "코인 2배 (광고)" },
  ["over.retry"] = { en = "Play Again", ko = "다시 하기" },
  ["over.home"] = { en = "Home", ko = "홈으로" },

  ["shop.title"] = { en = "Shop", ko = "상점" },
  ["shop.themes"] = { en = "Backgrounds", ko = "배경" },
  ["shop.store"] = { en = "Store", ko = "스토어" },
  ["shop.use"] = { en = "Use", ko = "사용" },
  ["shop.using"] = { en = "In use", ko = "사용 중" },
  ["shop.owned"] = { en = "Owned", ko = "보유 중" },

  ["book.title"] = { en = "Friends", ko = "동물 도감" },
  ["book.count"] = { en = "{n} / {m} met", ko = "{n} / {m} 만남" },
  ["book.unknown"] = { en = "???", ko = "???" },

  ["missions.title"] = { en = "Daily Missions", ko = "오늘의 미션" },
  ["missions.claim"] = { en = "Claim", ko = "받기" },
  ["missions.done"] = { en = "Done", ko = "완료" },
  ["missions.reset"] = { en = "New missions every day!", ko = "미션은 매일 새로 바뀌어요!" },

  ["daily.title"] = { en = "Daily Gift", ko = "매일 선물" },
  ["daily.desc"] = { en = "Come back every day for bigger gifts!", ko = "매일 오면 선물이 점점 커져요!" },
  ["daily.day"] = { en = "Day {n}", ko = "{n}일차" },
  ["daily.claim"] = { en = "Claim", ko = "받기" },
  ["daily.claimx2"] = { en = "Claim x2 (Ad)", ko = "2배 받기 (광고)" },
  ["daily.tomorrow"] = { en = "See you tomorrow!", ko = "내일 또 만나요!" },

  ["settings.title"] = { en = "Settings", ko = "설정" },
  ["settings.sound"] = { en = "Sound", ko = "효과음" },
  ["settings.music"] = { en = "Music", ko = "배경음악" },
  ["settings.language"] = { en = "Language", ko = "언어" },
  ["settings.on"] = { en = "On", ko = "켜짐" },
  ["settings.off"] = { en = "Off", ko = "꺼짐" },
  ["settings.version"] = { en = "Dongle Zoo 1.0", ko = "동글 동물원 1.0" },

  ["item.title"] = { en = "Out of {name}", ko = "{name} 없음" },
  ["item.buy"] = { en = "Buy  {n}", ko = "구매  {n}" },
  ["item.free"] = { en = "Free (Ad)", ko = "무료 (광고)" },

  ["toast.coins"] = { en = "+{n} coins", ko = "코인 +{n}" },
  ["toast.poor"] = { en = "Not enough coins", ko = "코인이 부족해요" },
  ["toast.newfriend"] = { en = "New friend: {name}! +{n}", ko = "새 친구 {name}! +{n}" },
  ["toast.revived"] = { en = "Second chance!", ko = "한 번 더!" },
  ["toast.noad"] = { en = "No ad available right now", ko = "지금은 광고를 볼 수 없어요" },
  ["toast.nostore"] = { en = "The store is not available here", ko = "여기서는 스토어를 쓸 수 없어요" },
  ["toast.item"] = { en = "{name} +1", ko = "{name} +1" },
  ["toast.theme"] = { en = "{name} unlocked!", ko = "{name} 획득!" },
  ["toast.thanks"] = { en = "Thank you!", ko = "고마워요!" },
  ["toast.whale"] = { en = "Whale party! +{n}", ko = "고래 파티! +{n}" },

  ["ad.rewarded"] = { en = "Rewarded ad (demo)", ko = "보상형 광고 (데모)" },
  ["ad.rewardedSub"] = { en = "Watch to the end to get your reward.", ko = "끝까지 보면 보상을 받아요." },
  ["ad.break"] = { en = "Ad break (demo)", ko = "쉬는 광고 (데모)" },
  ["ad.skip"] = { en = "Skip", ko = "건너뛰기" },
  ["ad.purchase"] = { en = "Store (demo)", ko = "스토어 (데모)" },
  ["ad.purchaseSub"] = { en = "{name}  {price}\nNothing is charged in this build.", ko = "{name}  {price}\n이 빌드에서는 결제되지 않아요." },
  ["ad.buy"] = { en = "Buy", ko = "구매" },
}

-- I.t("home.best", {n = 12}) -> text with {n} filled in; unknown keys return the key.
function I.t(key, vars)
  local e = STR[key]
  local s = e and (e[locale] or e.en) or key
  if vars then
    s = s:gsub("{(%w+)}", function(k)
      local v = vars[k]
      return v ~= nil and tostring(v) or "{" .. k .. "}"
    end)
  end
  return s
end

return I
