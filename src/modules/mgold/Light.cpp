#include "modules/mgold/Light.h"
#include "modules/mgold/ShaderBridge.h"

namespace love
{
namespace mgold
{

namespace
{

struct Rgba{ double r, g, b, a; };
struct Vec3{ double x, y, z; };

double fogStart = 2.0;
double fogEnd = 10.0;
Rgba fogColor = {0.0, 0.0, 1.0, 1.0};
Rgba globalLight = {1.0, 1.0, 1.0, 1.0};
Vec3 sunDirToLight = {0.25, 0.2, 0.5};

bool fogDirty = true;
bool lightDirty = true;
bool sunDirty = true;

int shaderRef = LUA_NOREF; // objeto Shader do LOVE, resolvido uma vez

// le um rgba/vec3 vindo do Lua (tabela com campos r,g,b,a / x,y,z) -- so em setters, nao e hot path
double field(lua_State *L, int idx, const char *key, double def){
	lua_getfield(L, idx, key);
	double v = lua_isnumber(L, -1) ? lua_tonumber(L, -1) : def;
	lua_pop(L, 1);
	return v;
}

Rgba readRgba(lua_State *L, int idx){
	luaL_checktype(L, idx, LUA_TTABLE);
	return {field(L, idx, "r", 0), field(L, idx, "g", 0), field(L, idx, "b", 0), field(L, idx, "a", 1)};
}

// shader:send(name, v1, v2, ...)
void sendNumbers(lua_State *L, const char *name, const double *v, int n){
	lua_rawgeti(L, LUA_REGISTRYINDEX, shaderRef);
	lua_getfield(L, -1, "send");
	lua_insert(L, -2);            // send, shader
	lua_pushstring(L, name);

	if (n == 1){
		lua_pushnumber(L, v[0]);
	}
	else{
		lua_createtable(L, n, 0);
		for (int i = 0; i < n; i++){
			lua_pushnumber(L, v[i]);
			lua_rawseti(L, -2, i + 1);
		}
	}
	lua_call(L, 3, 0);
}

int w_setFog(lua_State *L){
	fogStart = luaL_checknumber(L, 1);
	fogEnd = luaL_checknumber(L, 2);
	fogColor = readRgba(L, 3);
	fogDirty = true;
	return 0;
}

int w_setGlobalLight(lua_State *L){
	globalLight = readRgba(L, 1);
	lightDirty = true;
	return 0;
}

int w_setSunDir(lua_State *L){
	luaL_checktype(L, 1, LUA_TTABLE);
	sunDirToLight = {field(L, 1, "x", 0), field(L, 1, "y", 0), field(L, 1, "z", 0)};
	sunDirty = true;
	return 0;
}

int w_update(lua_State *L){
	if (!(fogDirty || lightDirty || sunDirty))
		return 0; // caminho comum: zero trabalho, zero Lua

	if (shaderRef == LUA_NOREF){
		bridge::pushShader(L);
		shaderRef = luaL_ref(L, LUA_REGISTRYINDEX);
	}

	if (fogDirty){
		sendNumbers(L, "fogStart", &fogStart, 1);
		sendNumbers(L, "fogEnd", &fogEnd, 1);
		double c[4] = {fogColor.r, fogColor.g, fogColor.b, fogColor.a};
		sendNumbers(L, "fogColor", c, 4);
		fogDirty = false;
	}
	if (lightDirty){
		double c[4] = {globalLight.r, globalLight.g, globalLight.b, globalLight.a};
		sendNumbers(L, "globalLight", c, 4);
		lightDirty = false;
	}
	if (sunDirty){
		double d[3] = {sunDirToLight.x, sunDirToLight.y, sunDirToLight.z};
		sendNumbers(L, "sunDirToLight", d, 3);
		sunDirty = false;
	}
	return 0;
}

} // namespace

void MGOLD_updateLight(lua_State *L){
	w_update(L);
}

void MGOLD_setLightFunc(lua_State *L){
	static const luaL_Reg funcs[] = {
		{"setFog", w_setFog},
		{"setGlobalLight", w_setGlobalLight},
		{"setSunDir", w_setSunDir},
		{"update", w_update},
		{nullptr, nullptr}
	};

	lua_newtable(L);
	for (const luaL_Reg *r = funcs; r->name; r++){
		lua_pushcfunction(L, r->func);
		lua_setfield(L, -2, r->name);
	}
}

}
}