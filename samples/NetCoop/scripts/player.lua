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
    local p = state.state == "idle" and input or input.player(self:get("NetPlayer").player)
    local x = (p.down("D") and 1 or 0) - (p.down("A") and 1 or 0) + p.axis("LeftX")
    local z = (p.down("S") and 1 or 0) - (p.down("W") and 1 or 0) - p.axis("LeftY")
    local length = math.sqrt(x*x + z*z)
    if length > 1 then x, z = x/length, z/length end
    local t = self:get("Transform")
    t.position.x = math.max(-5, math.min(5, t.position.x + x * 4 * dt))
    t.position.z = math.max(-3, math.min(3, t.position.z + z * 4 * dt))
    self:set("Transform", t)
end
return Player
