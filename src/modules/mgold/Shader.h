#pragma once

#include "common/runtime.h"
#include "common/Module.h"

#include "modules/graphics/Graphics.h"
#include "modules/graphics/Shader.h"
#include "modules/graphics/wrap_Shader.h"
#include "filesystem/Filesystem.h"
#include "filesystem/wrap_Filesystem.h"



namespace love
{
namespace mgold
{

void MGOLD_setShaderFunc(lua_State *L);

void MGOLD_pushDefaultShader(lua_State *L);

struct DefaultUniforms
{
	graphics::Shader *shader = nullptr;
	unsigned generation = 0; 
	const graphics::Shader::UniformInfo *view = nullptr, *proj = nullptr, *canvas = nullptr;
	const graphics::Shader::UniformInfo *fogStart = nullptr, *fogEnd = nullptr, *fogColor = nullptr;
	const graphics::Shader::UniformInfo *globalLight = nullptr, *sunDir = nullptr;
};

DefaultUniforms &MGOLD_defaultUniforms(lua_State *L);
inline graphics::Shader *MGOLD_defaultShader(lua_State *L){ return MGOLD_defaultUniforms(L).shader; }


unsigned MGOLD_defaultGeneration();


void MGOLD_resetDefaultShader();

} 
} 