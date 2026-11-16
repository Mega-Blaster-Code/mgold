#include "common/runtime.h"
#include "common/Module.h"

#include "modules/graphics/Graphics.h"
#include "modules/graphics/Shader.h"
#include "modules/graphics/wrap_Shader.h"
#include "filesystem/Filesystem.h"
#include "filesystem/wrap_Filesystem.h"

#include "modules/mgold/Shader.h"
#include "modules/mgold/Render.h"
#include "modules/mgold/Textures.h"
#include "modules/mgold/Matrices.h"
#include "modules/mgold/Collisions.h"
#include "modules/mgold/Vectors.h"
#include "modules/mgold/Physics.h"
#include "modules/mgold/ObjLoader.h"
#include "modules/mgold/Camera.h"
#include "modules/mgold/Light.h"
#include "modules/mgold/Object.h"
#include "modules/mgold/Player.h"
#include "modules/mgold/Stdlib.h"
#include "modules/mgold/Tab.h"
#include "modules/mgold/Network.h"

#define MGOLD_LOG(...) if(mgold_conf_config_printf){printf(__VA_ARGS__);}

namespace love
{
namespace mgold
{

bool mgold_conf_config_printf = true;

static int w_mgold_config(lua_State *L)
{
    lua_getglobal(L, "love");

	if (lua_istable(L, -1)){
		lua_getfield(L, -1, "_conf");
		if (lua_istable(L, -1)){
			lua_getfield(L, -1, "mgold");
			if (lua_istable(L, -1)){
				lua_getfield(L, -1, "config");
				if (lua_istable(L, -1)){
					lua_getfield(L, -1, "printf");
					mgold_conf_config_printf = lua_toboolean(L, -1);
					lua_pop(L, 1);
				}
			}else{
				mgold_conf_config_printf = true;
			}
			lua_pop(L, 1);
		}else{
			mgold_conf_config_printf = true;
		}

		lua_pop(L, 1);
	}

	lua_pop(L, 1);
    return 0;
}

extern "C" int luaopen_love_mgold(lua_State *L)
{

	w_mgold_config(L);

	MGOLD_LOG("MGOLD: INIT\n");

	lua_newtable(L);

	love::mgold::MGOLD_resetDefaultShader();

	MGOLD_LOG("MGOLD: TAB [");
	love::mgold::MGOLD_setTabFunc(L);
	lua_setfield(L, -2, "tab");
	MGOLD_LOG("OK]\n");

	MGOLD_LOG("MGOLD: OL [");
	love::mgold::MGOLD_setObjLoaderFunc(L);
	lua_setfield(L, -2, "ObjLoader");
	MGOLD_LOG("OK]\n");

	MGOLD_LOG("MGOLD: MAT [");
	love::mgold::MGOLD_setMatricesFunc(L);
	lua_setfield(L, -2, "matrices");
	MGOLD_LOG("OK]\n");

	MGOLD_LOG("MGOLD: SHD [");
	love::mgold::MGOLD_setShaderFunc(L);
	lua_setfield(L, -2, "shader");
	MGOLD_LOG("OK]\n");

	MGOLD_LOG("MGOLD: RND [");
	love::mgold::MGOLD_setRenderFunc(L);
	lua_setfield(L, -2, "render");
	MGOLD_LOG("OK]\n");

	MGOLD_LOG("MGOLD: TEX [");
	love::mgold::MGOLD_setTextureFunc(L);
	lua_setfield(L, -2, "textures");
	MGOLD_LOG("OK]\n");

	MGOLD_LOG("MGOLD: COL [");
	love::mgold::MGOLD_setCollisionsFunc(L);
	lua_setfield(L, -2, "collisions");
	MGOLD_LOG("OK]\n");

	MGOLD_LOG("MGOLD: VEC [");
	love::mgold::MGOLD_setVectorsFunc(L);
	lua_setfield(L, -2, "vector");
	MGOLD_LOG("OK]\n");

	MGOLD_LOG("MGOLD: PHY [");
	love::mgold::MGOLD_setPhysicsFunc(L);
	lua_setfield(L, -2, "physics");
	MGOLD_LOG("OK]\n");

	MGOLD_LOG("MGOLD: CAM [");
	love::mgold::MGOLD_setCameraFunc(L);
	lua_setfield(L, -2, "camera");
	MGOLD_LOG("OK]\n");

	MGOLD_LOG("MGOLD: LIT [");
	love::mgold::MGOLD_setLightFunc(L);
	lua_setfield(L, -2, "light");
	MGOLD_LOG("OK]\n");

	MGOLD_LOG("MGOLD: OBJ [");
	love::mgold::MGOLD_setObjectFunc(L);
	lua_setfield(L, -2, "object");
	MGOLD_LOG("OK]\n");

	MGOLD_LOG("MGOLD: PLY [");
	love::mgold::MGOLD_setPlayerFunc(L);
	lua_setfield(L, -2, "player");
	MGOLD_LOG("OK]\n");

	MGOLD_LOG("MGOLD: STL [");
	love::mgold::MGOLD_setStdlibFunc(L);
	lua_setfield(L, -2, "stdlib");
	MGOLD_LOG("OK]\n");

	MGOLD_LOG("MGOLD: NET [");
	love::mgold::MGOLD_setNetworkFunc(L);
	MGOLD_LOG("OK]\n");

	lua_getglobal(L, "love");

	if (lua_istable(L, -1))
	{
		lua_pushvalue(L, -2);
		lua_setfield(L, -2, "mgold");
	}

	lua_pop(L, 1);

	MGOLD_LOG("MGOLD: OK\n");

	return 1;
}

} // mgold
} // love
