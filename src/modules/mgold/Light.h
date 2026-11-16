#pragma once

#include "common/runtime.h"

namespace love
{
namespace mgold
{

void MGOLD_setLightFunc(lua_State *L);

void MGOLD_updateLight(lua_State *L);

}
}