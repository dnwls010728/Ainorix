-- Pure operations on the engine's named-coordinate vectors.
local M = {}
local function number(v)
    assert(type(v) == "number" and v == v and math.abs(v) < math.huge, "expected finite number")
    return v
end
local function components(v)
    assert(type(v) == "table", "expected {x,y,z} vector")
    return number(v.x == nil and 0 or v.x), number(v.y == nil and 0 or v.y), number(v.z == nil and 0 or v.z)
end
local function length(x, y, z)
    local scale = math.max(math.abs(x), math.abs(y), math.abs(z))
    if scale == 0 then return 0 end
    return scale * math.sqrt((x / scale)^2 + (y / scale)^2 + (z / scale)^2)
end
function M.clamp(value, low, high)
    number(value); number(low); number(high)
    assert(low <= high, "clamp lower bound exceeds upper bound")
    return math.max(low, math.min(high, value))
end
function M.lerp(a, b, t)
    number(a); number(b); number(t)
    return a + (b - a) * t
end
function M.distance(a, b)
    local ax, ay, az = components(a)
    local bx, by, bz = components(b)
    return length(ax - bx, ay - by, az - bz)
end
function M.normalize(v)
    local x, y, z = components(v)
    local scale = math.max(math.abs(x), math.abs(y), math.abs(z))
    if scale == 0 then return {x=0, y=0, z=0} end
    x, y, z = x / scale, y / scale, z / scale
    local d = math.sqrt(x*x + y*y + z*z)
    return {x=x/d, y=y/d, z=z/d}
end
return M
