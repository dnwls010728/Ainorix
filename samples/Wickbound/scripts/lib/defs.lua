-- Content tables: weapons, passives, keepers, stages, meta upgrades, achievements, shop.
-- `icon` is the emoji image in assets/icons (the original used emoji glyphs).
local D = {}

D.MAX_ITEM_LEVEL = 5
D.MAX_WEAPONS = 6
D.MAX_PASSIVES = 6
D.RUN_LENGTH = 600 -- seconds; final boss spawns here
D.MID_BOSS_TIME = 300

D.WEAPONS = {
  {
    id = "ember_bolt", icon = "1f525", color = "#ff9a3c", pair = "wick",
    name = { en = "Ember Bolt", ko = "불씨 화살" },
    desc = { en = "Fires a bolt at the nearest critter.", ko = "가장 가까운 장난꾸러기에게 화살을 쏴요." },
    evoName = { en = "Phoenix Bolt", ko = "불사조 화살" },
    evoDesc = { en = "Bolts go boom and fly through everything.", ko = "화살이 펑 터지며 모두를 뚫고 지나가요." },
  },
  {
    id = "flame_ring", icon = "2b55", color = "#ff5e3a", pair = "prism",
    name = { en = "Flame Ring", ko = "화염 고리" },
    desc = { en = "Little flames dance around you.", ko = "작은 불꽃들이 주위를 빙글빙글 돌아요." },
    evoName = { en = "Corona", ko = "코로나" },
    evoDesc = { en = "A double ring that never goes out.", ko = "꺼지지 않는 두 겹 불꽃 고리예요." },
  },
  {
    id = "spark_chain", icon = "26a1", color = "#7fd6ff", pair = "bellows",
    name = { en = "Spark Chain", ko = "연쇄 불꽃" },
    desc = { en = "A spark that hops from critter to critter.", ko = "이리저리 톡톡 튀는 불꽃이에요." },
    evoName = { en = "Storm Wick", ko = "폭풍 심지" },
    evoDesc = { en = "The spark keeps hopping until nobody is left.", ko = "아무도 안 남을 때까지 계속 튀어요." },
  },
  {
    id = "oil_flask", icon = "1f9ea", color = "#c8ff5a", pair = "reservoir",
    name = { en = "Oil Flask", ko = "기름 플라스크" },
    desc = { en = "Tosses a flask that leaves a warm glowing puddle.", ko = "따끈하게 빛나는 웅덩이를 남기는 병을 던져요." },
    evoName = { en = "Bonfire Flask", ko = "모닥불 플라스크" },
    evoDesc = { en = "Huge puddles that also top up your oil.", ko = "커다란 웅덩이가 기름도 채워줘요." },
  },
  {
    id = "moth_swarm", icon = "1f98b", color = "#e6c7ff", pair = "magnet",
    name = { en = "Moth Swarm", ko = "나방 떼" },
    desc = { en = "Friendly moths flutter after critters.", ko = "친구 나방들이 팔랑팔랑 쫓아가요." },
    evoName = { en = "Luna Swarm", ko = "달빛 나방 떼" },
    evoDesc = { en = "Moths come back to you and go again.", ko = "나방이 돌아왔다가 다시 날아가요." },
  },
  {
    id = "beacon_pulse", icon = "1f4a0", color = "#ffe27a", pair = "lens",
    name = { en = "Beacon Pulse", ko = "등대 파동" },
    desc = { en = "A wave as big as your light pushes critters back.", ko = "빛만큼 커다란 파동이 모두를 밀어내요." },
    evoName = { en = "Lighthouse", ko = "등대" },
    evoDesc = { en = "Waves dazzle everything inside the light.", ko = "파동이 빛 안의 모두를 어질어질하게 해요." },
  },
  {
    id = "glass_shards", icon = "1f537", color = "#9ff3ff", pair = "boots",
    name = { en = "Star Shards", ko = "별 조각" },
    desc = { en = "Scatters star shards the way you are moving.", ko = "움직이는 방향으로 별 조각을 뿌려요." },
    evoName = { en = "Sparkle Gale", ko = "반짝 돌풍" },
    evoDesc = { en = "Shards fly everywhere and bounce around.", ko = "별 조각이 사방으로 날아가 통통 튕겨요." },
  },
  {
    id = "sun_lance", icon = "2600", color = "#fff2b0", pair = "plating",
    name = { en = "Sun Lance", ko = "태양 창" },
    desc = { en = "A sunbeam through the biggest crowd.", ko = "가장 북적이는 곳을 지나는 햇살이에요." },
    evoName = { en = "Morning Glory", ko = "아침 햇살" },
    evoDesc = { en = "Three wide sunbeams shine out around you.", ko = "넓은 햇살 세 줄기가 사방을 비춰요." },
  },
}

D.PASSIVES = {
  { id = "wick", icon = "1f56f", color = "#ffb36b", name = { en = "Wick", ko = "심지" }, desc = { en = "+12% damage per level.", ko = "레벨당 피해 +12%." } },
  { id = "bellows", icon = "1f4a8", color = "#b8e0ff", name = { en = "Bellows", ko = "풀무" }, desc = { en = "-7% weapon cooldown per level.", ko = "레벨당 무기 쿨다운 -7%." } },
  { id = "lens", icon = "1f50d", color = "#ffe27a", name = { en = "Lens", ko = "렌즈" }, desc = { en = "+10% light radius per level.", ko = "레벨당 빛 반경 +10%." } },
  { id = "reservoir", icon = "1f6e2", color = "#c8ff5a", name = { en = "Reservoir", ko = "기름통" }, desc = { en = "+20% max oil, -6% oil drain per level.", ko = "레벨당 최대 기름 +20%, 소모 -6%." } },
  { id = "boots", icon = "1f462", color = "#9ff3ff", name = { en = "Boots", ko = "장화" }, desc = { en = "+7% move speed per level.", ko = "레벨당 이동 속도 +7%." } },
  { id = "plating", icon = "1f6e1", color = "#c9c9d6", name = { en = "Cozy Coat", ko = "포근한 외투" }, desc = { en = "+15% max HP and +1 armor per level.", ko = "레벨당 최대 체력 +15%, 방어 +1." } },
  { id = "magnet", icon = "1f9f2", color = "#ff8fb1", name = { en = "Magnet", ko = "자석" }, desc = { en = "+25% pickup range per level.", ko = "레벨당 획득 범위 +25%." } },
  { id = "prism", icon = "1f53a", color = "#d7a6ff", name = { en = "Prism", ko = "프리즘" }, desc = { en = "+10% area and duration per level.", ko = "레벨당 범위와 지속 시간 +10%." } },
}

local function index(list)
  local map = {}
  for _, v in ipairs(list) do map[v.id] = v end
  return map
end

local weaponMap, passiveMap = index(D.WEAPONS), index(D.PASSIVES)
function D.weaponById(id) return weaponMap[id] end
function D.passiveById(id) return passiveMap[id] end

-- ---------------------------------------------------------------- keepers

D.KEEPERS = {
  {
    id = "ada", color = "#ffb347", startWeapon = "ember_bolt", hpMul = 1, speedMul = 1, pickupMul = 1, cost = 0,
    name = { en = "Ada", ko = "에이다" },
    desc = { en = "Balanced. Starts with Ember Bolt.", ko = "균형형. 불씨 화살로 시작." },
  },
  {
    id = "bram", color = "#ff6b4a", startWeapon = "flame_ring", hpMul = 1.3, speedMul = 0.9, pickupMul = 1, cost = 600,
    name = { en = "Bram", ko = "브램" },
    desc = { en = "+30% HP, -10% speed. Starts with Flame Ring.", ko = "체력 +30%, 속도 -10%. 화염 고리로 시작." },
  },
  {
    id = "suri", color = "#c59bff", startWeapon = "moth_swarm", hpMul = 0.8, speedMul = 1.15, pickupMul = 1.4, cost = 1500,
    name = { en = "Suri", ko = "수리" },
    desc = { en = "+15% speed, +40% pickup, -20% HP. Starts with Moth Swarm.", ko = "속도 +15%, 획득 범위 +40%, 체력 -20%. 나방 떼로 시작." },
  },
}

local keeperMap = index(D.KEEPERS)
function D.keeperById(id) return keeperMap[id] or D.KEEPERS[1] end

-- ---------------------------------------------------------------- stages

D.STAGES = {
  {
    id = "moor", ground = "#2c4050", groundAlt = "#36505f", accent = "#ffcf5c",
    oilDrainMul = 1, enemyHpMul = 1, glimMul = 1, requires = "",
    name = { en = "Firefly Meadow", ko = "반딧불 초원" },
    desc = { en = "A gentle place to start.", ko = "모험을 시작하기 좋은 포근한 곳이에요." },
  },
  {
    id = "chapel", ground = "#27456a", groundAlt = "#2f5580", accent = "#8fd0ff",
    oilDrainMul = 1.3, enemyHpMul = 1.15, glimMul = 1.25, requires = "moor",
    name = { en = "Moonlit Pond", ko = "달빛 연못" },
    desc = { en = "Misty air. Oil burns 30% faster. +25% Glims.", ko = "촉촉한 안개. 기름이 30% 빨리 닳아요. 글림 +25%." },
  },
  {
    id = "frost", ground = "#5a6690", groundAlt = "#6b78a6", accent = "#e8f2ff",
    oilDrainMul = 1.15, enemyHpMul = 1.4, glimMul = 1.5, requires = "chapel",
    name = { en = "Snowflake Hill", ko = "눈꽃 언덕" },
    desc = { en = "Critters are 40% tougher. +50% Glims.", ko = "장난꾸러기들이 40% 더 튼튼해요. 글림 +50%." },
  },
}

local stageMap = index(D.STAGES)
function D.stageById(id) return stageMap[id] or D.STAGES[1] end

-- ---------------------------------------------------------------- meta upgrades

D.META = {
  { id = "vitality", icon = "2764", max = 10, baseCost = 60, costMul = 1.45, name = { en = "Vitality", ko = "활력" }, desc = { en = "+5% max HP", ko = "최대 체력 +5%" } },
  { id = "might", icon = "2694", max = 10, baseCost = 80, costMul = 1.5, name = { en = "Might", ko = "힘" }, desc = { en = "+4% damage", ko = "피해 +4%" } },
  { id = "swiftness", icon = "1f3c3", max = 5, baseCost = 100, costMul = 1.6, name = { en = "Swiftness", ko = "신속" }, desc = { en = "+2% move speed", ko = "이동 속도 +2%" } },
  { id = "radiance", icon = "1f31f", max = 5, baseCost = 100, costMul = 1.6, name = { en = "Radiance", ko = "광휘" }, desc = { en = "+4% light radius", ko = "빛 반경 +4%" } },
  { id = "thrift", icon = "1f6e2", max = 5, baseCost = 90, costMul = 1.6, name = { en = "Thrift", ko = "절약" }, desc = { en = "-4% oil drain", ko = "기름 소모 -4%" } },
  { id = "greed", icon = "1f4b0", max = 10, baseCost = 70, costMul = 1.45, name = { en = "Greed", ko = "탐욕" }, desc = { en = "+8% Glims", ko = "글림 +8%" } },
  { id = "wisdom", icon = "1f4d8", max = 5, baseCost = 120, costMul = 1.6, name = { en = "Wisdom", ko = "지혜" }, desc = { en = "+4% XP", ko = "경험치 +4%" } },
  { id = "second_wind", icon = "1f54a", max = 1, baseCost = 800, costMul = 1, name = { en = "Second Wind", ko = "재기" }, desc = { en = "Revive once per run", ko = "런당 1회 부활" } },
  { id = "reroll", icon = "1f3b2", max = 3, baseCost = 150, costMul = 2, name = { en = "Reroll", ko = "다시 뽑기" }, desc = { en = "+1 reroll per run", ko = "런당 다시 뽑기 +1" } },
}

local metaMap = index(D.META)
function D.metaById(id) return metaMap[id] end

-- Per-level effect sizes, used by the meta layer to build run modifiers.
D.META_EFFECT = {
  vitality = 0.05, might = 0.04, swiftness = 0.02, radiance = 0.04, thrift = 0.04,
  greed = 0.08, wisdom = 0.04, second_wind = 1, reroll = 1,
}

-- ---------------------------------------------------------------- achievements

-- check(stats, run, save) is evaluated after stats were updated with `run`.
D.ACHIEVEMENTS = {
  { id = "first_light", reward = 50, name = { en = "First Light", ko = "첫 불빛" }, desc = { en = "Light a brazier.", ko = "화로에 불을 붙이세요." }, check = function(s) return s.braziersLit >= 1 end },
  { id = "survive_3", reward = 50, name = { en = "Still Burning", ko = "아직 타오른다" }, desc = { en = "Survive 3 minutes.", ko = "3분 동안 생존하세요." }, check = function(s) return s.bestTime >= 180 end },
  { id = "survive_5", reward = 100, name = { en = "Halfway There", ko = "절반의 밤" }, desc = { en = "Survive 5 minutes.", ko = "5분 동안 생존하세요." }, check = function(s) return s.bestTime >= 300 end },
  { id = "kingslayer", reward = 150, name = { en = "Nap Time", ko = "낮잠 시간" }, desc = { en = "Send King Snooze off to bed.", ko = "졸음 대왕을 재워 보내세요." }, check = function(s) return s.bossKills >= 1 end },
  { id = "dawn", reward = 300, name = { en = "Good Morning", ko = "좋은 아침" }, desc = { en = "Win a run.", ko = "런에서 승리하세요." }, check = function(s) return s.wins >= 1 end },
  { id = "all_braziers", reward = 150, name = { en = "Lamplighter", ko = "점등원" }, desc = { en = "Light all braziers in one run.", ko = "한 런에서 모든 화로를 밝히세요." }, check = function(_, r) return r.braziersLit >= 6 end },
  { id = "evolve", reward = 150, name = { en = "Glow Up", ko = "반짝 변신" }, desc = { en = "Evolve a weapon.", ko = "무기를 진화시키세요." }, check = function(s) return s.evolutions >= 1 end },
  { id = "kills_1k", reward = 100, name = { en = "Busy Night", ko = "바쁜 밤" }, desc = { en = "Shoo away 1,000 critters in total.", ko = "장난꾸러기를 모두 1,000마리 쫓아내세요." }, check = function(s) return s.kills >= 1000 end },
  { id = "kills_10k", reward = 400, name = { en = "Night Guardian", ko = "밤의 수호자" }, desc = { en = "Shoo away 10,000 critters in total.", ko = "장난꾸러기를 모두 10,000마리 쫓아내세요." }, check = function(s) return s.kills >= 10000 end },
  { id = "level_20", reward = 150, name = { en = "Bright Mind", ko = "밝은 정신" }, desc = { en = "Reach level 20 in a run.", ko = "한 런에서 레벨 20에 도달하세요." }, check = function(s) return s.maxLevel >= 20 end },
  { id = "chapel_win", reward = 400, name = { en = "Dry Feet", ko = "마른 발" }, desc = { en = "Win at the Moonlit Pond.", ko = "달빛 연못에서 승리하세요." }, check = function(_, r) return r.victory and r.stageId == "chapel" end },
  { id = "frost_win", reward = 800, name = { en = "Snow Day", ko = "눈 오는 날" }, desc = { en = "Win on Snowflake Hill.", ko = "눈꽃 언덕에서 승리하세요." }, check = function(_, r) return r.victory and r.stageId == "frost" end },
  { id = "ten_runs", reward = 100, name = { en = "Night Shift", ko = "야간 근무" }, desc = { en = "Play 10 runs.", ko = "10번 플레이하세요." }, check = function(s) return s.runs >= 10 end },
}

D.DAILY_REWARDS = { 50, 75, 100, 125, 150, 200, 300 } -- by streak day (wraps at 7)

D.IAP_PRODUCTS = {
  { id = "no_ads", icon = "1f6ab", priceLabel = "$2.99", glims = 0, name = { en = "No Ads", ko = "광고 제거" }, desc = { en = "Removes interstitials. Rewards become instant.", ko = "전면 광고 제거. 보상을 즉시 받습니다." } },
  { id = "starter", icon = "1f381", priceLabel = "$1.99", glims = 2000, name = { en = "Starter Pack", ko = "스타터 팩" }, desc = { en = "2,000 Glims and Bram.", ko = "글림 2,000개와 브램." } },
  { id = "glims_s", icon = "1f45d", priceLabel = "$0.99", glims = 1000, name = { en = "Pouch of Glims", ko = "글림 주머니" }, desc = { en = "1,000 Glims.", ko = "글림 1,000개." } },
  { id = "glims_l", icon = "1f9f0", priceLabel = "$4.99", glims = 7000, name = { en = "Chest of Glims", ko = "글림 상자" }, desc = { en = "7,000 Glims.", ko = "글림 7,000개." } },
}

return D
