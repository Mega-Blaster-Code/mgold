#include "modules/mgold/Shader.h"

#include "common/runtime.h"
#include "common/Module.h"

#include "modules/graphics/Graphics.h"
#include "modules/graphics/Shader.h"
#include "modules/graphics/wrap_Shader.h"
#include "modules/filesystem/Filesystem.h"
#include "modules/filesystem/wrap_Filesystem.h"

#include <string>
#include <vector>
#include <cstring>

namespace love { namespace graphics { int w_Shader_send(lua_State *L); } }

namespace love
{
namespace mgold
{

#define instance() (Module::getInstance<graphics::Graphics>(Module::M_GRAPHICS))

static const char *DEFAULT_SHADER_KEY = "meg2_default_shader";

static const char default_shader[] =
#include "defaultShader.vert"
;

static void luax_checkgraphicscreated(lua_State *L)
{
	if (instance() == nullptr || !instance()->isCreated())
		luaL_error(L, "love.graphics cannot function without a window.");
}

static int w_getShaderSource(lua_State *L, int startidx, std::vector<std::string> &stages, graphics::Shader::CompileOptions &options)
{
	using namespace love::filesystem;

	luax_checkgraphicscreated(L);

	auto fs = Module::getInstance<Filesystem>(Module::M_FILESYSTEM);

	for (int i = startidx; i < startidx + 2; i++)
	{
		if (!lua_isstring(L, i))
		{
			if (luax_cangetfiledata(L, i))
			{
				FileData *fd = luax_getfiledata(L, i);
				lua_pushlstring(L, (const char *) fd->getData(), fd->getSize());
				fd->release();
				lua_replace(L, i);
			}
			continue;
		}

		size_t slen = 0;
		const char *str = lua_tolstring(L, i, &slen);

		Filesystem::Info info = {};
		if (fs != nullptr && fs->getInfo(str, info))
		{
			FileData *fd = nullptr;
			luax_catchexcept(L, [&]() { fd = fs->read(str); });

			lua_pushlstring(L, (const char *) fd->getData(), fd->getSize());
			fd->release();
			lua_replace(L, i);
		}
		else if (slen > 0 && slen < 64 && !strchr(str, '\n'))
		{
			const char *ext = strchr(str, '.');
			if (ext != nullptr && !strchr(ext, ';') && !strchr(ext, ' '))
				return luaL_error(L, "Could not open file %s. Does not exist.", str);
		}
	}

	bool has_arg1 = lua_isstring(L, startidx + 0) != 0;
	bool has_arg2 = lua_isstring(L, startidx + 1) != 0;

	if (!(has_arg1 || has_arg2))
		luaL_checkstring(L, startidx);

	if (has_arg1)
		stages.push_back(luax_checkstring(L, startidx + 0));
	if (has_arg2)
		stages.push_back(luax_checkstring(L, startidx + 1));

	int optionsidx = has_arg2 ? startidx + 2 : startidx + 1;
	if (!lua_isnoneornil(L, optionsidx))
	{
		luaL_checktype(L, optionsidx, LUA_TTABLE);

		lua_getfield(L, optionsidx, "defines");
		if (!lua_isnoneornil(L, -1))
		{
			if (!lua_istable(L, -1))
				return luaL_argerror(L, optionsidx, "expected 'defines' field to be a table");

			lua_pushnil(L);
			while (lua_next(L, -2))
			{
				std::string defname;
				std::string defval;

				if (lua_type(L, -2) == LUA_TNUMBER && lua_type(L, -1) == LUA_TSTRING)
					defname = luaL_checkstring(L, -1);
				else if (lua_type(L, -2) != LUA_TSTRING)
					return luaL_argerror(L, optionsidx, "all fields in the 'defines' table must use string keys.");
				else
				{
					defname = luaL_checkstring(L, -2);
					if (lua_type(L, -1) == LUA_TBOOLEAN)
						defval = luax_toboolean(L, -1) ? "1" : "0";
					else
					{
						const char *val = lua_tostring(L, -1);
						if (val == nullptr)
							return luaL_argerror(L, optionsidx, "'defines' table values must be strings, numbers, or booleans.");
						defval = val;
					}
				}

				options.defines[defname] = defval;
				lua_pop(L, 1);
			}
		}
		lua_pop(L, 1);

		lua_getfield(L, optionsidx, "debugname");
		if (!lua_isnoneornil(L, -1))
			options.debugName = luax_checkstring(L, -1);
		lua_pop(L, 1);

		for (int feature = 0; feature < graphics::Shader::FEATURE_MAX_ENUM; ++feature)
		{
			const char *featureKey;
			if (graphics::Shader::getConstant((graphics::Shader::Feature) feature, featureKey))
			{
				lua_getfield(L, optionsidx, featureKey);
				if (!lua_isnoneornil(L, -1))
					options.features[feature] = lua_toboolean(L, -1) != 0;
				lua_pop(L, 1);
			}
		}
	}

	return 0;
}

static int w_shader_validate(lua_State *L)
{
	bool gles = luax_checkboolean(L, 1);

	std::vector<std::string> stages;
	graphics::Shader::CompileOptions options;
	w_getShaderSource(L, 2, stages, options);

	bool success = true;
	std::string err;
	try
	{
		success = instance()->validateShader(gles, stages, options, err);
	}
	catch (love::Exception &e)
	{
		success = false;
		err = e.what();
	}

	luax_pushboolean(L, success);
	if (!success)
	{
		luax_pushstring(L, err);
		return 2;
	}
	return 1;
}

static int w_shader_load(lua_State *L)
{
	std::vector<std::string> stages;
	graphics::Shader::CompileOptions options;
	w_getShaderSource(L, 1, stages, options);

	try
	{
		graphics::Shader *shader = instance()->newShader(stages, options);
		luax_pushtype(L, shader);
		shader->release();
		return 1;
	}
	catch (love::Exception &e)
	{
		lua_pushnil(L);
		luax_pushstring(L, e.what());
		return 2;
	}
}

namespace
{
DefaultUniforms defaults;      // so ponteiros: o dono do shader e o registry do Lua
unsigned generationCounter = 0;
}

void MGOLD_pushDefaultShader(lua_State *L)
{
	lua_getfield(L, LUA_REGISTRYINDEX, DEFAULT_SHADER_KEY);
	if (luax_istype(L, -1, graphics::Shader::type))
		return;
	lua_pop(L, 1);

	luax_checkgraphicscreated(L);

	std::vector<std::string> stages;
	stages.push_back(default_shader);
	graphics::Shader::CompileOptions options;

	std::string err;
	bool failed = false;

	try
	{
		graphics::Shader *shader = instance()->newShader(stages, options);
		luax_pushtype(L, shader);
		shader->release();
	}
	catch (love::Exception &e)
	{
		err = e.what();
		failed = true;
	}

	if (failed)
		luaL_error(L, "failed to load default shader: %s", err.c_str());

	lua_pushvalue(L, -1);
	lua_setfield(L, LUA_REGISTRYINDEX, DEFAULT_SHADER_KEY);
}

DefaultUniforms &MGOLD_defaultUniforms(lua_State *L)
{
	if (defaults.shader != nullptr)
		return defaults;

	MGOLD_pushDefaultShader(L);
	graphics::Shader *sh = graphics::luax_checkshader(L, -1);
	lua_pop(L, 1); // o registry continua dono

	defaults.shader = sh;
	defaults.view = sh->getUniformInfo("viewMatrix");
	defaults.proj = sh->getUniformInfo("projectionMatrix");
	defaults.canvas = sh->getUniformInfo("isCanvasEnabled");
	defaults.fogStart = sh->getUniformInfo("fogStart");
	defaults.fogEnd = sh->getUniformInfo("fogEnd");
	defaults.fogColor = sh->getUniformInfo("fogColor");
	defaults.globalLight = sh->getUniformInfo("globalLight");
	defaults.sunDir = sh->getUniformInfo("sunDirToLight");
	defaults.generation = ++generationCounter;
	return defaults;
}

unsigned MGOLD_defaultGeneration()
{
	return defaults.shader != nullptr ? defaults.generation : 0;
}

void MGOLD_resetDefaultShader()
{
	defaults = DefaultUniforms();
	++generationCounter; // invalida quem guardou a geracao anterior
}

static int w_shader_default_get(lua_State *L)
{
	MGOLD_pushDefaultShader(L);
	return 1;
}

static int w_shader_default_send(lua_State *L)
{
	MGOLD_pushDefaultShader(L);
	lua_insert(L, 1);
	return graphics::w_Shader_send(L);
}
void MGOLD_setShaderFunc(lua_State *L)
{
	lua_newtable(L); // shader

	lua_pushcfunction(L, w_shader_load);
	lua_setfield(L, -2, "load");

	lua_pushcfunction(L, w_shader_validate);
	lua_setfield(L, -2, "validate");

	lua_newtable(L); // shader.default

	lua_pushcfunction(L, w_shader_default_get);
	lua_setfield(L, -2, "get");

	lua_pushcfunction(L, w_shader_default_send);
	lua_setfield(L, -2, "send");

	lua_setfield(L, -2, "default");
}

} // mgold
} // love