-- Widget composition; native focus signatures and global ui remain unchanged.
local entities = require("oe.entities")
local native = ui
local M = { focus = native.focus, focused = native.focused, screenToCanvas = native.screenToCanvas, measureText = native.measureText }
local kinds = { "UIText", "UIButton", "UIInput", "UIPanel", "UIImage", "UISlider", "UIDropdown", "UIJoystick" }
local textKinds = { UIText = true, UIButton = true, UIInput = true }
local focusKinds = { UIButton = true, UIInput = true, UISlider = true, UIDropdown = true }
local records = setmetatable({}, { __mode = "k" })
local Widget = {}
Widget.__index = Widget

local function record(widget, operation, supported)
    local r = records[widget]
    assert(r, "foreign or reconstructed widget handle")
    entities.id(r.entity)
    if supported then
        assert(supported[r.kind], "widget '" .. r.name .. "' (expected " .. operation .. " capability): " .. r.kind .. " does not support " .. operation)
    end
    return r
end

function M.tryFind(name, capability)
    local entity = entities.tryFind(name, capability)
    if not entity then return nil end
    local selected
    for _, kind in ipairs(kinds) do
        if not capability or capability == kind then
            if scene.has(entity:id(), kind) then
                assert(not selected, "widget '" .. name .. "' (expected one UI component): ambiguous; select a component explicitly")
                selected = kind
            end
        end
    end
    if not selected then return nil end
    local widget = setmetatable({}, Widget)
    records[widget] = { entity = entities.find(name, selected), kind = selected, name = name }
    return widget
end

function M.find(name, capability)
    local widget = M.tryFind(name, capability)
    assert(widget, "widget '" .. tostring(name) .. "' (expected " .. (capability or "UI component") .. "): missing entity or capability")
    return widget
end

function M.list(prefix, count)
    assert(type(prefix) == "string" and math.type(count) == "integer" and count >= 0, "widget list expects prefix and nonnegative integer count")
    local list = {}
    for index = 1, count do list[index] = M.find(prefix .. index) end
    return list
end

function M.get(widget, component)
    local r = record(widget)
    return entities.get(r.entity, component or r.kind)
end

function M.set(widget, component, values)
    local r = record(widget)
    if type(component) == "table" and values == nil then
        return entities.set(r.entity, r.kind, component)
    end
    return entities.set(r.entity, component, values)
end

function Widget:id() return entities.id(record(self).entity) end
function Widget:get(component) return M.get(self, component) end
function Widget:set(component, values) return M.set(self, component, values) end
function Widget:text(line)
    local r = record(self, "text", textKinds)
    return entities.set(r.entity, r.kind, { text = line })
end
function Widget:show(on) return self:set({ visible = on ~= false }) end
function Widget:hide() return self:show(false) end
function Widget:visible() return self:get().visible end
function Widget:focus()
    local r = record(self, "focus", focusKinds)
    return native.focus(entities.id(r.entity))
end

return M
