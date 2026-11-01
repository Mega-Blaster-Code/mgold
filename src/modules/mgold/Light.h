#pragma once

#include "common/runtime.h"

namespace love
{
namespace mgold
{

void MGOLD_setLightFunc(lua_State *L);

// envia fog / luz / sol ao shader padrao se algo mudou (usado pelo Object.cpp)
void MGOLD_updateLight(lua_State *L);

}
}