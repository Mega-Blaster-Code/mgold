#pragma once

#include "common/runtime.h"

namespace love { namespace graphics { class Mesh; } }

namespace love
{
namespace mgold
{

void MGOLD_setObjectFunc(lua_State *L);

bool MGOLD_getMeshDrawInfo(lua_State *L, int idx, graphics::Mesh *&mesh, int &visibleCount);

}
}