-- Controller evaluation is script behaviour. Assets are validated by the loader;
-- each owner retains its own parameters/clock, including through native rollback.
local M = {}
local Controller = {}
Controller.__index = Controller
local function finite(value)
    return type(value) == "number" and value == value and math.abs(value) ~= math.huge
end
local function enter(self, name, blend)
    local state = self.definition.states[name]
    animation.play(self.id, state.clip, {speed = state.speed or 1, loop = state.loop ~= false,
        restart = true, blend = blend or 0})
    self.current, self.elapsed = name, 0
end
function M.new(id, path)
    assert(math.type(id) == "integer" and scene.exists(id), "animator needs an existing entity id")
    local definition = animation.controller(path, id)
    local self = setmetatable({id = id, path = path, definition = definition, parameters = {}, elapsed = 0}, Controller)
    for name, parameter in pairs(definition.parameters) do self.parameters[name] = parameter.default end
    enter(self, definition.entry, 0)
    return self
end
function Controller:set(name, value)
    local parameter = self.definition.parameters[name]
    assert(parameter, "unknown controller parameter: " .. tostring(name))
    assert(parameter.type ~= "trigger", "use trigger(name) for triggers")
    assert((parameter.type == "number" and finite(value) and value >= -1e9 and value <= 1e9) or
        (parameter.type == "boolean" and type(value) == "boolean"), "wrong controller parameter type")
    self.parameters[name] = value
    return self
end
function Controller:get(name)
    assert(self.definition.parameters[name], "unknown controller parameter: " .. tostring(name))
    return self.parameters[name]
end
function Controller:trigger(name)
    assert(self.definition.parameters[name] and self.definition.parameters[name].type == "trigger", "unknown controller trigger")
    self.parameters[name] = true
    return self
end
local function matches(condition, parameters)
    local value, wanted, op = parameters[condition.parameter], condition.value, condition.op
    if op == "trigger" then return value end
    if op == "==" then return value == wanted end
    if op == "!=" then return value ~= wanted end
    if op == ">" then return value > wanted end
    if op == ">=" then return value >= wanted end
    if op == "<" then return value < wanted end
    return value <= wanted
end
function Controller:update(dt)
    assert(finite(dt) and dt >= 0 and dt <= 3600, "controller dt needs finite seconds in 0..3600")
    assert(scene.exists(self.id), "controller entity no longer exists")
    local elapsed = math.min(self.elapsed + dt, 3600)
    for _, transition in ipairs(self.definition.transitions) do
        if (transition.from == "*" or transition.from == self.current) and transition.to ~= self.current and
            elapsed >= (transition.minTime or 0) then
            local pass = true
            for _, condition in ipairs(transition.conditions) do
                if not matches(condition, self.parameters) then pass = false; break end
            end
            if pass then
                -- Play first: a missing/deleted clip must not consume a trigger or change controller state.
                enter(self, transition.to, transition.blend)
                for _, condition in ipairs(transition.conditions) do
                    if condition.op == "trigger" then self.parameters[condition.parameter] = false end
                end
                return self.current
            end
        end
    end
    self.elapsed = elapsed
    return self.current
end
function Controller:state()
    local parameters = {}
    for name, value in pairs(self.parameters) do parameters[name] = value end
    return {state = self.current, elapsed = self.elapsed, parameters = parameters, path = self.path, id = self.id}
end
return M
