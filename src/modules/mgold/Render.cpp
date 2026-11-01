#include "modules/mgold/Render.h"
#include "modules/mgold/Shader.h"
#include "modules/mgold/Camera.h"

#include "common/runtime.h"
#include "common/Module.h"
#include "modules/graphics/Graphics.h"
#include "modules/graphics/Shader.h"
#include "modules/graphics/wrap_Shader.h"

namespace love{namespace graphics{Mesh *luax_checkmesh(lua_State *L, int idx);}}

namespace love
{
namespace mgold
{

#define instance() (Module::getInstance<graphics::Graphics>(Module::M_GRAPHICS))

static int w_render_finish(lua_State *L)
{
	instance()->setShader();
	return 0;
}

// render.begin([shader])
static int w_render_begin(lua_State *L)
{
	graphics::Shader *shader = nullptr;
	if (lua_toboolean(L, 1))
		shader = graphics::luax_checkshader(L, 1);
	else
	{
		MGOLD_pushDefaultShader(L);
		shader = graphics::luax_checkshader(L, -1);
		lua_pop(L, 1);
	}

	luax_catchexcept(L, [&]() { instance()->setShader(shader); });

	MGOLD_sendCameraMatrix(L);

	instance()->setColor(Colorf(1.0f, 1.0f, 1.0f, 1.0f));
	return 0;
}

// render.render(mesh)
static int w_render_render(lua_State *L)
{
	luaL_checktype(L, 1, LUA_TTABLE); // mesh

	lua_getfield(L, 1, "visibleCount");
	int count = lua_isnumber(L, -1) ? (int) lua_tointeger(L, -1) : 0;
	lua_pop(L, 1);
	if (count <= 0)
		return 0;

	graphics::Shader *customShader = nullptr;
	lua_getfield(L, 1, "shader");
	if (lua_toboolean(L, -1))
		customShader = graphics::luax_checkshader(L, -1);
	lua_pop(L, 1);

	lua_getfield(L, 1, "mesh");
	graphics::Mesh *mesh = graphics::luax_checkmesh(L, -1);
	lua_pop(L, 1);

	StrongRef<graphics::Shader> oldShader(graphics::Shader::current);

	if (customShader)
	{
		luax_catchexcept(L, [&]() { instance()->setShader(customShader); });

		// shader novo: reenvia as matrizes
		MGOLD_sendCameraMatrix(L);
	}

	instance()->setColor(Colorf(1.0f, 1.0f, 1.0f, 1.0f));

	Matrix4 m;
	luax_catchexcept(L, [&]() { mesh->drawInstanced(instance(), m, count); });

	if (customShader)
	{
		if (oldShader.get())
			luax_catchexcept(L, [&]() { instance()->setShader(oldShader.get()); });
		else
			instance()->setShader();
	}

	return 0;
}

void MGOLD_setRenderFunc(lua_State *L)
{
	lua_newtable(L); // render

	lua_pushcfunction(L, w_render_finish);
	lua_setfield(L, -2, "finish");

	lua_pushcfunction(L, w_render_begin);
	lua_setfield(L, -2, "begin");

	lua_pushcfunction(L, w_render_render);
	lua_setfield(L, -2, "render");
}

} // mgold
} // love