#pragma once

// Ponte minima para o shader padrao do mgold (love.mgold.shader.default).
//
// Camera.cpp e Light.cpp so falam com o shader quando algo MUDOU (flags dirty), entao este e o
// unico ponto onde ainda passamos pelo Lua. As referencias das funcoes sao resolvidas uma unica
// vez e guardadas no registry.
//
// TODO: se Shader.cpp expuser uma API nativa (ex.: MGOLD_defaultShaderSend...), troque as funcoes
// abaixo por chamadas diretas e o Lua some totalmente do caminho.

#include "common/runtime.h"

namespace love
{
namespace mgold
{

namespace bridge
{

inline int &sendRef(){ static int r = LUA_NOREF; return r; }
inline int &getRef(){ static int r = LUA_NOREF; return r; }

inline void resolve(lua_State *L){
	if (sendRef() != LUA_NOREF)
		return;

	lua_getglobal(L, "love");
	lua_getfield(L, -1, "mgold");
	lua_getfield(L, -1, "shader");
	lua_getfield(L, -1, "default");

	lua_getfield(L, -1, "send");
	sendRef() = luaL_ref(L, LUA_REGISTRYINDEX);
	lua_getfield(L, -1, "get");
	getRef() = luaL_ref(L, LUA_REGISTRYINDEX);

	lua_pop(L, 4);
}

// default.send(name, <valor ja empilhado no topo>)  -- consome o valor
inline void sendTop(lua_State *L, const char *name){
	resolve(L);
	lua_rawgeti(L, LUA_REGISTRYINDEX, sendRef()); // valor, fn
	lua_insert(L, -2);                            // fn, valor
	lua_pushstring(L, name);                      // fn, valor, nome
	lua_insert(L, -2);                            // fn, nome, valor
	lua_call(L, 2, 0);
}

inline void sendBool(lua_State *L, const char *name, bool v){
	lua_pushboolean(L, v ? 1 : 0);
	sendTop(L, name);
}

// default.get() -> objeto Shader do LOVE (empilhado)
inline void pushShader(lua_State *L){
	resolve(L);
	lua_rawgeti(L, LUA_REGISTRYINDEX, getRef());
	lua_call(L, 0, 1);
}

}

}
}