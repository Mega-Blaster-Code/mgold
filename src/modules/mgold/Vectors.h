#pragma once

#include "common/runtime.h"
#include "common/Module.h"

namespace love{

namespace mgold{

void MGOLD_setVectorsFunc(lua_State *L);

void MGOLD_pushVec3(lua_State *L, double x, double y, double z);

}
}