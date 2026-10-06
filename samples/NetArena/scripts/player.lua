local Controls = require("oe.input")
local declarations = {actions={"W","A","S","D","Space","R"}, axes={"LeftX","LeftY"}}
local actions = {move={kind="vector", left={"A"}, right={"D"}, up={"W"}, down={"S"}, axisX="LeftX", axisY="LeftY"}}
local localInput = Controls.localSource()
local Player = {}
local colors = {{0.22, 0.65, 1}, {1, 0.35, 0.3}, {0.3, 0.9, 0.5}, {0.8, 0.45, 1}}
function Player:onStart()
    local number = self:get("NetPlayer").player
    self:set("Transform", {position = {x = (number - 1) * 2 - 2, y = 0.5, z = 2}})
    local c = colors[(number - 1) % #colors + 1]
    self:set("MeshRenderer", {color = {r = c[1], g = c[2], b = c[3]}})
end
function Player:onUpdate(dt)
    local state = net.state()
    if state.state ~= "idle" and (not state.sync or state.sync.state ~= "running") then return end
    local source = state.state == "idle" and localInput or Controls.playerSource(self:get("NetPlayer").player, declarations)
    local move = Controls.read(source, actions, "move")
    local x, z = move.x, -move.y
    local t = self:get("Transform")
    t.position.x = math.max(-5, math.min(5, t.position.x + x * 4 * dt))
    t.position.z = math.max(-3, math.min(3, t.position.z + z * 4 * dt))
    self:set("Transform", t)
end
return Player
