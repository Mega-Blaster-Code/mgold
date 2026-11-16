#pragma once

#include "common/runtime.h"

namespace love
{
namespace mgold
{

extern "C" {
    void MGOLD_setTabFunc(lua_State *L);
}

}
}