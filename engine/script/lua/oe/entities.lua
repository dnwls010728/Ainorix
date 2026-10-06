-- Runtime handles belong to this Lua session and one scene incarnation.
local M = {}
local session = {}
local records = setmetatable({}, { __mode = "k" })
local Handle = {}
Handle.__index = Handle

local function describe(name, capability)
    return "entity '" .. tostring(name) .. "' (expected " .. (capability or "entity") .. ")"
end

local function hasCapability(id, capability)
    -- Unknown component names follow the same descriptive find/get/set failure path.
    local ok, present = pcall(scene.has, id, capability)
    return ok and present
end

local function validate(handle, required)
    local r = records[handle]
    assert(r and r.session == session, "foreign or reconstructed entity handle")
    local label = describe(r.name, required or r.capability)
    assert(r.epoch == scene.epoch(), label .. ": world was replaced; find again")
    assert(scene.exists(r.id), label .. ": entity was destroyed")
    assert(not r.capability or hasCapability(r.id, r.capability), label .. ": missing capability " .. tostring(r.capability))
    assert(not required or hasCapability(r.id, required), label .. ": missing capability " .. tostring(required))
    return r.id
end

function M.tryFind(name, capability)
    assert(type(name) == "string", "entity name must be a string")
    assert(capability == nil or type(capability) == "string", "expected capability must be a string")
    local id = scene.find(name)
    if not id or (capability and not hasCapability(id, capability)) then return nil end
    local handle = setmetatable({}, Handle)
    records[handle] = { session = session, epoch = scene.epoch(), id = id, name = name, capability = capability }
    return handle
end

function M.find(name, capability)
    local handle = M.tryFind(name, capability)
    assert(handle, describe(name, capability) .. ": missing entity or capability")
    return handle
end

function M.id(handle) return validate(handle) end
function M.get(handle, component) return scene.get(validate(handle, component), component) end
function M.set(handle, component, values) return scene.set(validate(handle, component), component, values) end
function Handle:id() return M.id(self) end
function Handle:get(component) return M.get(self, component) end
function Handle:set(component, values) return M.set(self, component, values) end

return M
