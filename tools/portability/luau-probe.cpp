// luau-probe.c — §7 probe de utilização do Luau vendido (finding #302).
// Compila+roda contra as libs estáticas MSVC (external/solutions/luau/
// build-gate/Release/Luau.{VM,Compiler,Ast,Bytecode,Common}.lib): compila
// fonte → bytecode (luau_compile), carrega no VM (luau_load), executa
// (lua_pcall) e lê o resultado (lua_tointeger); propaga erro de runtime
// (LUA_ERRRUN) com mensagem; roda uma tabela+função via C API. Exit 0 =
// runtime de scripting vendido é utilizável SEM wiring de CMake.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lua.h"
#include "lualib.h"
#include "luacode.h"

static int failures = 0;

static void check(int cond, const char* label, const char* detail) {
    if (cond) {
        printf("  ok  %s\n", label);
    } else {
        failures++;
        printf("FAIL  %s  %s\n", label, detail);
    }
}

static char* compile_or_die(const char* src, size_t* out_size) {
    size_t size = 0;
    char* bc = luau_compile(src, strlen(src), NULL, &size);
    if (!bc || size == 0) {
        printf("FAIL  compile produced no bytecode\n");
        failures++;
    }
    *out_size = size;
    return bc;
}

int main(void) {
    printf("luau-probe: vendored Luau\n");

    // 1) compile source -> bytecode, load, run, read the returned value.
    const char* src = "local a = 40\nlocal b = 2\nreturn a + b\n";
    size_t bc_size = 0;
    char* bc = compile_or_die(src, &bc_size);
    check(bc != NULL && bc_size > 0, "source compiled to bytecode", "");

    lua_State* L = luaL_newstate();
    luaL_openlibs(L);
    check(luau_load(L, "probe", bc, bc_size, 0) == 0,
          "bytecode loaded into VM", lua_tostring(L, -1));
    check(lua_pcall(L, 0, 1, 0) == 0, "chunk executed", lua_tostring(L, -1));
    int result = (int)lua_tointeger(L, -1);
    check(result == 42, "returned value is 42 (40+2)", "got unexpected value");

    // 2) runtime error propagates as LUA_ERRRUN with the message.
    const char* err_src = "error('boom')";
    size_t err_size = 0;
    char* err_bc = compile_or_die(err_src, &err_size);
    lua_State* L2 = luaL_newstate();
    luaL_openlibs(L2);
    luau_load(L2, "err", err_bc, err_size, 0);
    int pcall_status = lua_pcall(L2, 0, 0, 0);
    const char* err_msg = lua_tostring(L2, -1);
    check(pcall_status == LUA_ERRRUN, "runtime error -> LUA_ERRRUN", "");
    check(err_msg && strstr(err_msg, "boom") != NULL,
          "error message propagated ('boom')", err_msg ? err_msg : "(null)");

    // 3) table + function through the C API (embedded scripting surface).
    lua_State* L3 = luaL_newstate();
    luaL_openlibs(L3);
    lua_newtable(L3);                                   // stack: table
    lua_pushinteger(L3, 7);                             // stack: table, 7
    lua_setfield(L3, -2, "hp");                         // table.hp = 7
    lua_pushinteger(L3, 3);                             // stack: table, 3
    lua_setfield(L3, -2, "dmg");                        // table.dmg = 3
    lua_getfield(L3, -1, "hp");                         // stack: table, hp
    check(lua_tointeger(L3, -1) == 7, "table field read via C API", "");
    // Call a Lua stdlib function (string.format) through lua_pcall.
    lua_getglobal(L3, "string");
    lua_getfield(L3, -1, "format");
    lua_pushstring(L3, "hp=%d");
    lua_pushinteger(L3, 7);
    check(lua_pcall(L3, 2, 1, 0) == 0, "string.format via pcall", lua_tostring(L3, -1));
    const char* formatted = lua_tostring(L3, -1);
    check(formatted && strcmp(formatted, "hp=7") == 0,
          "formatted string is 'hp=7'", formatted ? formatted : "(null)");

    if (failures == 0) {
        printf("luau-probe: ALL PASSED (vendored Luau usable: compile+run+errors+C API)\n");
        return 0;
    }
    printf("luau-probe: %d FAILURES\n", failures);
    return 1;
}
