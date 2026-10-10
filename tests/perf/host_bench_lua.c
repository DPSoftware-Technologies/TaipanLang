/* The same three tests through Lua 5.4's C API (what the engine uses today). */
#include <stdio.h>
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

typedef struct { int value; } Counter;
static Counter counter;

static int l_add(lua_State* L) { lua_pushinteger(L, luaL_checkinteger(L, 1) + luaL_checkinteger(L, 2)); return 1; }
static int l_counter(lua_State* L) {
    Counter** ud = (Counter**)lua_newuserdata(L, sizeof(Counter*));
    *ud = &counter;
    luaL_setmetatable(L, "Counter");
    return 1;
}
static int c_add(lua_State* L) { Counter* c = *(Counter**)luaL_checkudata(L, 1, "Counter"); c->value += (int)luaL_checkinteger(L, 2); return 0; }
static int c_index(lua_State* L) {
    Counter* c = *(Counter**)luaL_checkudata(L, 1, "Counter");
    const char* k = luaL_checkstring(L, 2);
    if (k[0] == 'v') { lua_pushinteger(L, c->value); return 1; }
    lua_getmetatable(L, 1); lua_getfield(L, -1, k); return 1;
}

int main(void) {
    lua_State* L = luaL_newstate();
    luaL_openlibs(L);
    lua_newtable(L);
    lua_pushcfunction(L, l_add); lua_setfield(L, -2, "add");
    lua_pushcfunction(L, l_counter); lua_setfield(L, -2, "counter");
    lua_setglobal(L, "engine");
    luaL_newmetatable(L, "Counter");
    lua_pushcfunction(L, c_add); lua_setfield(L, -2, "add");
    lua_pushcfunction(L, c_index); lua_setfield(L, -2, "__index");
    lua_pop(L, 1);
    const char* script =
        "local function run()\n"
        "  local acc = 0 local t = os.clock()\n"
        "  for i = 1, 1000000 do acc = engine.add(acc, 1) end\n"
        "  print(string.format('1M host function calls   %.1f ms', (os.clock() - t) * 1000))\n"
        "  local c = engine.counter() t = os.clock()\n"
        "  for i = 1, 1000000 do c:add(1) end\n"
        "  print(string.format('1M bound method calls    %.1f ms', (os.clock() - t) * 1000))\n"
        "  local total = 0 t = os.clock()\n"
        "  for i = 1, 1000000 do total = total + c.value end\n"
        "  print(string.format('1M property reads        %.1f ms', (os.clock() - t) * 1000))\n"
        "end\nrun()\n";
    if (luaL_dostring(L, script)) { printf("%s\n", lua_tostring(L, -1)); return 1; }
    lua_close(L);
    return 0;
}
