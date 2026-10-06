-- Explicit sources prevent local UI/device input from becoming network gameplay.
local M = {}
local function active()
    local state = net.state().state
    return state ~= "none" and state ~= "idle"
end
function M.localSource()
    local function check() assert(not active(), "local gameplay input is unavailable in a network session; use playerSource") end
    return {
        down=function(key) check(); return input.down(key) end,
        pressed=function(key) check(); return input.pressed(key) end,
        axis=function(name) check(); return input.axis(name) end,
    }
end
function M.playerSource(id, declarations)
    assert(type(id)=="number" and id%1==0 and id>=1 and id<4294967294, "invalid player id")
    assert(type(declarations)=="table", "playerSource requires declared actions and axes")
    local actions, axes = {}, {}
    for _, name in ipairs(declarations.actions or {}) do
        assert(type(name)=="string", "declared action must be a string"); actions[name]=true
    end
    for _, name in ipairs(declarations.axes or {}) do
        assert(type(name)=="string", "declared axis must be a string"); axes[name]=true
    end
    local p = input.player(id)
    local function check(name, allowed)
        local state = net.state()
        assert(state.sync and state.sync.state == "running", "playerSource requires a running network match; use localSource for offline practice")
        assert(allowed[name], "undeclared player input '" .. tostring(name) .. "'")
    end
    return {
        down=function(key) check(key, actions); return p.down(key) end,
        pressed=function(key) check(key, actions); return p.pressed(key) end,
        axis=function(name) check(name, axes); return p.axis(name) end,
    }
end
local function any(source, method, keys)
    local value = false
    for _, key in ipairs(keys or {}) do
        assert(type(key)=="string", "input binding key must be a string")
        -- Read every binding so an undeclared alias cannot hide behind a held key.
        value = source[method](key) or value
    end
    return value
end
function M.read(source, actions, name)
    assert(type(source)=="table" and type(actions)=="table", "read requires source and actions")
    local binding = actions[name]
    assert(type(binding)=="table", "unknown input action '" .. tostring(name) .. "'")
    if binding.kind == "button" then
        return {held=any(source,"down",binding.keys), pressed=any(source,"pressed",binding.keys)}
    end
    assert(binding.kind=="vector", "input action requires kind vector or button")
    local x = (any(source,"down",binding.right) and 1 or 0) - (any(source,"down",binding.left) and 1 or 0)
    local y = (any(source,"down",binding.up) and 1 or 0) - (any(source,"down",binding.down) and 1 or 0)
    if binding.axisX then x = x + source.axis(binding.axisX) end
    if binding.axisY then y = y + source.axis(binding.axisY) end
    local d = math.sqrt(x*x + y*y)
    if d > 1 then x, y = x/d, y/d end
    return {x=x, y=y, z=0}
end
return M
