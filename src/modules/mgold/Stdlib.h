#pragma once

#include "common/runtime.h"
#include "common/Module.h"

#include "modules/graphics/Graphics.h"
#include "modules/graphics/Shader.h"
#include "modules/graphics/wrap_Shader.h"
#include "filesystem/Filesystem.h"
#include "filesystem/wrap_Filesystem.h"

#include "modules/mgold/Shader.h"


namespace love
{
namespace mgold
{

void MGOLD_setStdlibFunc(lua_State *L);

}
}