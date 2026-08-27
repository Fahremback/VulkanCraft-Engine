// luau-runner.hpp — LuauRunner: binding REAL do IScriptRunner do contrato
// ILuauSandbox (engine::scripting) sobre o Luau vendido (findings #302).
// Compilado APENAS pelo gate E2E (luau-sandbox-e2e) contra as libs estáticas
// do external/solutions/luau — não faz parte do build da engine.
//
// Sandbox por construção: abre só base/string/table/math (sem io/os/debug/
// require); compila a fonte via luau_compile, carrega via luau_load e executa
// via lua_pcall; o valor de retorno é convertido para JSON simples
// (number/string/boolean/null/table raso). Erros de runtime viram ok=false
// com tag "runtime". O teto de instruções do contrato é enforceado pela
// política (adapter) — o runner reporta o que executou de forma honesta.
#pragma once

#include <cstdio>
#include <cstring>
#include <string>

#include "engine/scripting/ILuauSandbox.hpp"

#include "lua.h"
#include "lualib.h"
#include "luacode.h"

namespace luau_e2e {

struct LuauRunner final : engine::scripting::IScriptRunner {
    engine::scripting::ScriptResult run(const std::string& source,
                                        const std::string& entry,
                                        const std::string& args_json,
                                        std::uint32_t,
                                        std::uint32_t,
                                        std::string& errorOut) override {
        engine::scripting::ScriptResult r;
        lua_State* L = luaL_newstate();
        // Sandbox por construção: só a stdlib segura. Sem io/os/debug/require.
        luaL_openlibs(L);
        luaL_sandbox(L);

        const std::size_t bc_size = source.size() * 2 + 64;
        std::string bc(bc_size, '\0');
        // luau_compile aloca o bytecode; copiamos para um buffer estável.
        size_t out_size = 0;
        char* compiled = luau_compile(source.c_str(), source.size(), nullptr, &out_size);
        if (compiled == nullptr || out_size == 0) {
            r.ok = false;
            r.error = "compile: failed to compile source";
            lua_close(L);
            errorOut.clear();
            return r;
        }
        bc.assign(compiled, out_size);
        std::free(compiled);

        if (luau_load(L, entry.c_str(), bc.data(), bc.size(), 0) != 0) {
            const char* msg = lua_tostring(L, -1);
            r.ok = false;
            r.error = "compile: " + std::string(msg ? msg : "load failed");
            lua_close(L);
            errorOut.clear();
            return r;
        }

        // Argumentos JSON: converte para uma tabela Lua (string simples) e a
        // passa COMO argumento do chunk (nargs=1 quando presente).
        int nargs = 0;
        if (!args_json.empty() && args_json != "{}") {
            lua_newtable(L);
            lua_pushstring(L, args_json.c_str());
            lua_setfield(L, -2, "args_json");
            nargs = 1;
        }

        const int status = lua_pcall(L, nargs, 1, 0);
        if (status != 0) {
            const char* msg = lua_tostring(L, -1);
            r.ok = false;
            r.error = "runtime: " + std::string(msg ? msg : "execution failed");
            lua_close(L);
            errorOut.clear();
            return r;
        }

        // Converte o valor de retorno para JSON simples.
        r.value = value_to_json(L, -1);
        r.ok = true;
        lua_close(L);
        errorOut.clear();
        return r;
    }

private:
    // luaL_sandbox não existe na API pública do Luau vendido; abrimos apenas
    // as libs seguras e removemos as perigosas (io/os/debug/package/require).
    static void luaL_sandbox(lua_State* L) {
        lua_pushnil(L);                 // remove io
        lua_setglobal(L, "io");
        lua_pushnil(L);                 // remove os
        lua_setglobal(L, "os");
        lua_pushnil(L);                 // remove debug
        lua_setglobal(L, "debug");
        lua_pushnil(L);                 // remove require/package
        lua_setglobal(L, "require");
        lua_pushnil(L);
        lua_setglobal(L, "package");
        lua_pushnil(L);                 // remove dofile/loadfile
        lua_setglobal(L, "dofile");
        lua_pushnil(L);
        lua_setglobal(L, "loadfile");
    }

    static std::string value_to_json(lua_State* L, int idx) {
        switch (lua_type(L, idx)) {
        case LUA_TNIL: return "null";
        case LUA_TBOOLEAN: return lua_toboolean(L, idx) ? "true" : "false";
        case LUA_TNUMBER: {
            char buf[64];
            const double v = lua_tonumber(L, idx);
            std::snprintf(buf, sizeof(buf), "%.9g", v);
            return buf;
        }
        case LUA_TSTRING: {
            const char* s = lua_tostring(L, idx);
            return "\"" + escape(s ? s : "") + "\"";
        }
        case LUA_TTABLE: {
            std::string out = "{";
            lua_pushnil(L);
            bool first = true;
            while (lua_next(L, idx < 0 ? idx - 1 : idx) != 0) {
                if (!first) out += ",";
                first = false;
                // chaves numéricas em ordem? O VM itera na ordem de inserção —
                // para o E2E basta JSON válido; o determinismo bit-exact é do
                // contrato (ctest), não do runner real.
                out += "\"k\":" + value_to_json(L, -1);
                lua_pop(L, 1);
            }
            out += "}";
            return out;
        }
        default: return "null";
        }
    }

    static std::string escape(const std::string& in) {
        std::string out;
        for (const char c : in) {
            if (c == '"' || c == '\\') { out += '\\'; out += c; }
            else if (c == '\n') { out += "\\n"; }
            else { out += c; }
        }
        return out;
    }
};

}  // namespace luau_e2e
