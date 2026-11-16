#include "modules/mgold/Network.h"

#include <string>

namespace love
{
namespace mgold
{

static const char networkLuaPart1[] =
#include "NetCore.lua"
;

static const char networkLuaPart2[] =
#include "NetSession.lua"
;

static const char networkLuaPart3[] =
#include "NetUpdate.lua"
;

static const char networkLuaPart4[] =
#include "NetPlayers.lua"
;

static const char networkLuaScoreboard[] =
#include "NetScoreboard.lua"
;

static const char networkLuaPart5[] =
#include "NetPlayerSync.lua"
;

static const char networkLuaPart6[] =
#include "NetMessaging.lua"
;

static const char networkLuaPart7[] =
#include "NetTeamsModesVoice.lua"
;

static const char networkLuaPart8[] =
#include "NetModeration.lua"
;

static const char networkLuaPart9[] =
#include "NetBootstrap.lua"
;

void MGOLD_setNetworkFunc(lua_State *L)
{
	int module = lua_gettop(L);
	std::string networkLua;
	networkLua.reserve(sizeof(networkLuaPart1) + sizeof(networkLuaPart2)
		+ sizeof(networkLuaPart3) + sizeof(networkLuaPart4) + sizeof(networkLuaScoreboard)
		+ sizeof(networkLuaPart5) + sizeof(networkLuaPart6)
		+ sizeof(networkLuaPart7) + sizeof(networkLuaPart8)
		+ sizeof(networkLuaPart9) - 10);
	networkLua.append(networkLuaPart1, sizeof(networkLuaPart1) - 1);
	networkLua.append(networkLuaPart2, sizeof(networkLuaPart2) - 1);
	networkLua.append(networkLuaPart3, sizeof(networkLuaPart3) - 1);
	networkLua.append(networkLuaPart4, sizeof(networkLuaPart4) - 1);
	networkLua.append(networkLuaScoreboard, sizeof(networkLuaScoreboard) - 1);
	networkLua.append(networkLuaPart5, sizeof(networkLuaPart5) - 1);
	networkLua.append(networkLuaPart6, sizeof(networkLuaPart6) - 1);
	networkLua.append(networkLuaPart7, sizeof(networkLuaPart7) - 1);
	networkLua.append(networkLuaPart8, sizeof(networkLuaPart8) - 1);
	networkLua.append(networkLuaPart9, sizeof(networkLuaPart9) - 1);
	if (luaL_loadbuffer(L, networkLua.data(), networkLua.size(), "=[love.mgold.multiplayer]") != 0)
		lua_error(L);

	lua_pushvalue(L, module);
	lua_call(L, 1, 1);
	if (!lua_istable(L, -1))
		luaL_error(L, "mgold network initializer must return a table");

	lua_setfield(L, module, "multiplayer");
}

}
}
