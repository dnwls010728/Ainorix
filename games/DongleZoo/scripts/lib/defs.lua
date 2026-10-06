-- Content and numbers. Distances are world units; the UI is 96 reference pixels per unit.
local D = {}

D.PX = 96 -- reference pixels per world unit
D.JAR_HALF = 3.3 -- inner half width of the jar
D.FLOOR_Y = -5.0
D.LINE_Y = 3.4 -- an animal resting above this line ends the game
D.DROP_Y = 4.3 -- where the held animal waits
D.PLAY_TOP = 5.1 -- pointer presses above this (HUD) or below the floor (items) do not aim
D.OVER_TIME = 2.0 -- seconds above the line before the game ends
D.GRACE = 1.0 -- seconds a new animal is ignored by the line
D.DROP_COOLDOWN = 0.5
D.COMBO_WINDOW = 1.0 -- seconds between merges that keep a combo alive

-- The eleven animals, small to large. `score` is awarded when the animal is created by a merge.
D.TIERS = {
  { id = "chick", r = 0.26, score = 0, color = "#ffd84d", name = { en = "Chick", ko = "병아리" } },
  { id = "hamster", r = 0.34, score = 1, color = "#ffc27a", name = { en = "Hamster", ko = "햄스터" } },
  { id = "rabbit", r = 0.45, score = 3, color = "#ffd0d8", name = { en = "Rabbit", ko = "토끼" } },
  { id = "cat", r = 0.52, score = 6, color = "#aab8d6", name = { en = "Cat", ko = "고양이" } },
  { id = "dog", r = 0.66, score = 10, color = "#f2cf9a", name = { en = "Puppy", ko = "강아지" } },
  { id = "fox", r = 0.80, score = 15, color = "#ff9a4a", name = { en = "Fox", ko = "여우" } },
  { id = "panda", r = 0.92, score = 21, color = "#ffffff", name = { en = "Panda", ko = "판다" } },
  { id = "bear", r = 1.08, score = 28, color = "#b98358", name = { en = "Bear", ko = "곰" } },
  { id = "lion", r = 1.25, score = 36, color = "#e8792c", name = { en = "Lion", ko = "사자" } },
  { id = "elephant", r = 1.43, score = 45, color = "#b9c7e0", name = { en = "Elephant", ko = "코끼리" } },
  { id = "whale", r = 1.62, score = 55, color = "#5aa9ff", name = { en = "Whale", ko = "고래" } },
}
D.MAX_TIER = #D.TIERS
D.WHALE_BONUS = 100 -- two whales pop
D.DROP_WEIGHTS = { 32, 26, 20, 14, 8 } -- chance of tiers 1..5 for the next animal

function D.texture(tier)
  return string.format("assets/animals/%02d.png", tier)
end
function D.prefab(tier)
  return string.format("prefabs/piece_%02d.prefab.json", tier)
end

-- Helper items. `price` in coins; an ad gives one for free.
D.ITEMS = {
  { id = "hammer", price = 120, name = { en = "Toy Hammer", ko = "뿅망치" }, desc = { en = "Tap an animal to send it home.", ko = "동물을 톡 눌러 집으로 보내요." } },
  { id = "shake", price = 80, name = { en = "Shake", ko = "흔들기" }, desc = { en = "Shakes the jar so animals settle.", ko = "병을 흔들어 동물들을 섞어요." } },
  { id = "wand", price = 150, name = { en = "Magic Wand", ko = "요술봉" }, desc = { en = "Tap an animal to grow it one step.", ko = "동물을 눌러 한 단계 키워요." } },
}
function D.item(id)
  for _, it in ipairs(D.ITEMS) do
    if it.id == id then return it end
  end
end
D.START_ITEMS = 2 -- of each, for a new player

-- Backgrounds bought with coins.
D.THEMES = {
  { id = "meadow", price = 0, name = { en = "Sunny Meadow", ko = "햇살 초원" }, line = "#ff6b6b" },
  { id = "beach", price = 800, name = { en = "Sunset Beach", ko = "노을 해변" }, line = "#ff5a7a" },
  { id = "night", price = 1500, name = { en = "Starry Night", ko = "별빛 밤" }, line = "#ffd84d" },
}
function D.theme(id)
  for _, th in ipairs(D.THEMES) do
    if th.id == id then return th end
  end
  return D.THEMES[1]
end

-- Store products (real money). Prices are display text; the platform store decides the real ones.
D.PRODUCTS = require("scripts.config.shop").products
function D.product(id)
  for _, p in ipairs(D.PRODUCTS) do
    if p.id == id then return p end
  end
end

-- Without a host store the shop opens a demo sheet that grants the product for free.
-- Set this to false in a released build: products then report "store not available".
D.DEMO_STORE = true

D.DAILY_REWARDS = { 50, 60, 80, 100, 120, 150, 300 } -- coins for day 1..7 of a streak
D.COINS_PER_SCORE = 1 / 30 -- coins earned at the end of a game
D.NEW_ANIMAL_COINS = 20 -- first time an animal is made
D.INTERSTITIAL_GAP = 150 -- seconds between break ads
D.INTERSTITIAL_GAMES = 2 -- finished games between break ads

-- Daily missions: three of these are picked per day. `stat` is a counter kept for the day
-- (or the best single-game value when `best` is set).
D.MISSIONS = {
  { id = "merge", stat = "merges", goal = 60, reward = 60, text = { en = "Merge {n} times", ko = "{n}번 합치기" } },
  { id = "drop", stat = "drops", goal = 120, reward = 50, text = { en = "Drop {n} animals", ko = "동물 {n}마리 떨어뜨리기" } },
  { id = "games", stat = "games", goal = 3, reward = 50, text = { en = "Play {n} games", ko = "{n}판 플레이" } },
  { id = "score", stat = "score", best = true, goal = 1200, reward = 80, text = { en = "Score {n} in one game", ko = "한 판에 {n}점 달성" } },
  { id = "panda", stat = "tier7", goal = 2, reward = 70, text = { en = "Make {n} pandas", ko = "판다 {n}마리 만들기" } },
  { id = "bear", stat = "tier8", goal = 1, reward = 90, text = { en = "Make a bear", ko = "곰 만들기" } },
  { id = "combo", stat = "combo", best = true, goal = 4, reward = 70, text = { en = "Reach a x{n} combo", ko = "{n}콤보 달성" } },
  { id = "item", stat = "items", goal = 2, reward = 40, text = { en = "Use {n} items", ko = "아이템 {n}번 사용" } },
}
function D.mission(id)
  for _, m in ipairs(D.MISSIONS) do
    if m.id == id then return m end
  end
end

return D
