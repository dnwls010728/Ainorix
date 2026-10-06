-- A script that is in exactly one of several named phases (a state machine). Each phase is a
-- table of optional functions, called with the script that owns the machine:
--   enter(owner, ...)   once when the phase begins; the extra arguments are those of go()
--   update(owner)       every frame while it lasts
-- so a flow reads as a list of phases instead of if/elseif on a string:
--   self.phase = phases.new(self, { title = { update = ... }, login = { enter = ..., update = ... } })
--   self.phase:go("login")    self.phase:update()    self.phase:is("login")    self.phase:frames()
local Phases = {}

local Machine = {}
Machine.__index = Machine

function Phases.new(owner, phases)
  return setmetatable({ owner = owner, phases = phases, name = nil, since = 0 }, Machine)
end

-- Begins a phase. enter() may itself go on to another phase.
function Machine:go(name, ...)
  local phase = self.phases[name]
  if not phase then error("no phase named '" .. tostring(name) .. "'", 2) end
  self.name, self.since = name, time.frame()
  if phase.enter then phase.enter(self.owner, ...) end
end

function Machine:update()
  local phase = self.phases[self.name]
  if phase and phase.update then phase.update(self.owner) end
end

function Machine:is(name)
  return self.name == name
end

-- Frames since the phase began.
function Machine:frames()
  return time.frame() - self.since
end

return Phases
