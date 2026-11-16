#include "modules/mgold/Textures.h"

#include "modules/filesystem/physfs/Filesystem.h"

namespace love { namespace filesystem { int getDirectoryItems(lua_State *L); }namespace graphics { int w_newTexture(lua_State *L); } }

namespace love{

namespace mgold{

#define instance() (Module::getInstance<filesystem::Filesystem>(Module::M_FILESYSTEM))

static int abs_index(lua_State *L, int index){
	if (index > 0 || index <= LUA_REGISTRYINDEX)
		return index;

	return lua_gettop(L) + index + 1;
}

static int w_loadTextures(lua_State *L){
	size_t size;
	const char *dir = luaL_checklstring(L, 1, &size);

	if (lua_isnoneornil(L, 2)){
		lua_getglobal(L, "love");
		lua_getfield(L, -1, "mgold");
		lua_getfield(L, -1, "textures");
		lua_getfield(L, -1, "textures");

		lua_remove(L, -2);
		lua_remove(L, -2);
		lua_remove(L, -2);
	}
	else{
		luaL_checktype(L, 2, LUA_TTABLE);
		lua_pushvalue(L, 2);
	}

	int target = abs_index(L, -1);

	std::vector<std::string> items;

	bool result = instance()->getDirectoryItems(dir, items);
	if (!result){
		lua_pushnil(L);
		lua_pushstring(L, "Love failed to get directory items");
		return 2;
	}

	for (const auto &item : items)
	{
		std::string filename = item;

		if (filename.size() >= 4){
			filename.resize(filename.size() - 4);
		}

		lua_pushlstring(L, filename.c_str(), filename.size());

		lua_getglobal(L, "love");
		lua_getfield(L, -1, "graphics");
		lua_getfield(L, -1, "newTexture");

		lua_remove(L, -2);
		lua_remove(L, -2);

		std::string fullpath = std::string(dir) + item;
		lua_pushlstring(L, fullpath.c_str(), fullpath.size());

		if (lua_pcall(L, 1, 1, 0) != LUA_OK){
			return lua_error(L);

		}

		lua_settable(L, target);
	}

	return 0;
}

static int w_getTexture(lua_State *L){
	luaL_checktype(L, 1, LUA_TSTRING);

	if (lua_isnoneornil(L, 2)){
		lua_getglobal(L, "love");
		lua_getfield(L, -1, "mgold");
		lua_getfield(L, -1, "textures");
		lua_getfield(L, -1, "textures");

		lua_remove(L, -2);
		lua_remove(L, -2);
		lua_remove(L, -2);
	}
	else{
		luaL_checktype(L, 2, LUA_TTABLE);
		lua_pushvalue(L, 2);
	}

	lua_pushvalue(L, 1);
	lua_gettable(L, -2);

	lua_remove(L, -2);

	return 1;
}

void MGOLD_setTextureFunc(lua_State *L){
	lua_newtable(L);

	lua_newtable(L);
	lua_setfield(L, -2, "textures");

	lua_pushcfunction(L, w_loadTextures);
	lua_setfield(L, -2, "load");

	lua_pushcfunction(L, w_getTexture);
	lua_setfield(L, -2, "get");
}

}
}
