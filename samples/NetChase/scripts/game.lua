-- Server-side rules: touching the gold crystal scores; first to five wins; R starts a new round.
local Game = {}
-- A fixed route avoids arrival order, clocks and unseeded randomness.
local route = {{0, -4}, {8, 0}, {0, 8}, {-8, 0}, {8, -8}, {-8, 8}, {-8, -8}, {8, 8}}

function Game:onStart()
    self.scores, self.total, self.winner = {}, 0, nil
    self.target = scene.find("Target")
    self.score = scene.find("Score")
    local state = net.state()
    if state.state == "idle" then -- offline practice: network.playerPrefab only spawns in a match
        local id = scene.instantiate("prefabs/player.prefab.json", {name = "Player 1"})
        scene.set(id, "NetPlayer", {player = 1})
        scene.set(id, "NetSync", {owner = 1})
    end
end

function Game:place()
    local spot = route[self.total % #route + 1]
    scene.set(self.target, "Transform", {position = {x = spot[1], y = 0.7, z = spot[2]}})
end

function Game:onUpdate()
    local state = net.state()
    local offline = state.state == "idle"
    if not offline and (not state.sync or state.sync.state ~= "running") then return end
    if not offline and not net.isServer() then return end
    local players = scene.all("NetPlayer")
    local target = scene.get(self.target, "Transform").position
    for _, id in ipairs(players) do
        local number = scene.get(id, "NetPlayer").player
        local p = offline and input or input.player(number)
        if p.pressed("R") then
            self.scores, self.total, self.winner = {}, 0, nil
            self:place()
            target = scene.get(self.target, "Transform").position
        end
        local position = scene.get(id, "Transform").position
        local x, z = position.x - target.x, position.z - target.z
        if not self.winner and x * x + z * z < 1.2 and position.y < 1.6 then
            local key = tostring(number)
            self.scores[key] = (self.scores[key] or 0) + 1
            self.total = self.total + 1
            if self.scores[key] >= 5 then self.winner = key end
            self:place()
            target = scene.get(self.target, "Transform").position
        end
    end
    local text = "First to 5: "
    for _, id in ipairs(players) do
        local n = scene.get(id, "NetPlayer").player
        text = text .. "P" .. n .. " " .. (self.scores[tostring(n)] or 0) .. "   "
    end
    if self.winner then text = "Player " .. self.winner .. " wins! R: new round" end
    scene.set(self.score, "UIText", {text = text})
    game.set("scores", self.scores)
    game.set("winner", self.winner)
end
return Game
