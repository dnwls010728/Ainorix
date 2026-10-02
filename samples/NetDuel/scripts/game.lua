local Game = {}
function Game:onStart()
    self.versus = self.params.versus or false
    self.scores = {}
    self.winner = nil
    self.target = scene.find("Target")
    self.score = scene.find("Score")
    local state = net.state()
    if state.state == "idle" or state.mode ~= "authoritative" then
        local players = state.state == "idle" and {1} or net.players()
        for _, number in ipairs(players) do
            local id = scene.instantiate("prefabs/player.prefab.json", {name = "Player " .. number})
            scene.set(id, "NetPlayer", {player = number})
            scene.set(id, "NetSync", {owner = number})
        end
    end
end
function Game:reset()
    self.scores, self.winner = {}, nil
    scene.set(self.target, "Transform", {position = {x = -2, y = 0.5, z = 0}})
end
function Game:onUpdate()
    local state = net.state()
    local offline = state.state == "idle"
    if not offline and (not state.sync or state.sync.state ~= "running") then return end
    if not offline and state.mode == "authoritative" and not net.isServer() then return end
    local players = scene.all("NetPlayer")
    for _, id in ipairs(players) do
        local number = scene.get(id, "NetPlayer").player
        local p = offline and input or input.player(number)
        if p.pressed("R") then self:reset() end
        if not self.winner and p.pressed("Space") then
            local position = scene.get(id, "Transform").position
            local target = scene.get(self.target, "Transform").position
            local x, z = position.x - target.x, position.z - target.z
            if x*x + z*z < 2.25 then
                local key = self.versus and tostring(number) or "team"
                self.scores[key] = (self.scores[key] or 0) + 1
                local total = 0
                for _, n in pairs(self.scores) do total = total + n end
                -- A fixed route avoids arrival order, clocks and unseeded randomness.
                local route = {{2,0}, {2,-2}, {-2,-2}, {-2,0}}
                local next = route[(total - 1) % #route + 1]
                scene.set(self.target, "Transform", {position = {x = next[1], y = 0.5, z = next[2]}})
                if self.scores[key] >= 5 then self.winner = key end
            end
        end
    end
    local text
    if self.versus then
        text = "First to 5: "
        for _, id in ipairs(players) do
            local n = scene.get(id, "NetPlayer").player
            text = text .. "P" .. n .. " " .. (self.scores[tostring(n)] or 0) .. "   "
        end
    else text = "Team crystals: " .. (self.scores["team"] or 0) .. "/5" end
    if self.winner then text = self.winner == "team" and "Team wins! R: new round" or "Player " .. self.winner .. " wins! R: new round" end
    scene.set(self.score, "UIText", {text = text})
    game.set("crystals", self.scores["team"] or 0)
    game.set("scores", self.scores)
    game.set("winner", self.winner)
end
return Game
