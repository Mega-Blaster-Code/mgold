#pragma once

#include "common/runtime.h"
#include "common/Module.h"

namespace love{

namespace mgold{

// empilha a funcao objloader(path [, uFlip [, vFlip]]) -> tabela de vertices
void MGOLD_setObjLoaderFunc(lua_State *L);

}
}