local function fib(n) if n < 2 then return n end return fib(n - 1) + fib(n - 2) end
local function intLoop() local acc = 0 for i = 0, 5000000 - 1 do acc = (acc + i * 3) % 1000003 end return acc end
local Vec2 = {} Vec2.__index = Vec2
local function new() return setmetatable({x = 0, y = 0}, Vec2) end
function Vec2:add(o) self.x = self.x + o.x self.y = self.y + o.y end
local function methods() local p, v = new(), new() v.x = 0.5 v.y = 0.25 for i = 1, 1000000 do p:add(v) end return p.x end
local function natives() local acc = 0.0 for i = 0, 1000000 - 1 do acc = acc + math.sqrt(i) end return acc end
local t = os.clock() local r = fib(27) print(string.format("fib(27)        %.1f ms  (%d)", (os.clock() - t) * 1000, r))
t = os.clock() local a = intLoop() print(string.format("5M int loop    %.1f ms  (%d)", (os.clock() - t) * 1000, a))
t = os.clock() local m = methods() print(string.format("1M method call %.1f ms  (%g)", (os.clock() - t) * 1000, m))
t = os.clock() local n = natives() print(string.format("1M native call %.1f ms  (%d)", (os.clock() - t) * 1000, math.floor(n + 0.5)))
