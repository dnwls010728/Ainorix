local Lobby = {}
function Lobby:onStart()
    self.address = self.params.address or "127.0.0.1"
    self.port = self.params.port or 7778
    self.action = self.params.action or "status"
end
function Lobby:onClick()
    local state = net.state()
    if state.sync and state.sync.state == "running" then return end
    local ok, message = pcall(function()
        if self.action == "host" then net.host({port = self.port, seed = 71}); net.ready({ready = true})
        elseif self.action == "join" then net.join({address = self.address, port = self.port})
        elseif self.action == "ready" then net.ready({ready = true})
        elseif self.action == "start" then net.start() end
    end)
    if not ok then scene.set(scene.find("Status"), "UIText", {text = tostring(message)}) end
end
function Lobby:onUpdate()
    local state = net.state()
    local running = state.sync and state.sync.state == "running"
    if self.action ~= "status" then
        self:set("UIButton", {visible = not running})
        return
    end
    if running then self:set("UIText", {visible = false}); return end
    self:set("UIText", {visible = true})
    -- Lobby state is local. The ready barrier restores the original scene before frame zero.
    if state.state == "idle" then return end
    local message = state.state .. " | player " .. tostring(net.localPlayer()) .. " | " .. #net.players() .. " in lobby"
    if state.error and state.error ~= "" then message = message .. " | " .. state.error end
    self:set("UIText", {text = message})
end
return Lobby
