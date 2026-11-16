#pragma once











#include "common/runtime.h"
#include "modules/graphics/Shader.h"

namespace love
{
namespace mgold
{

namespace bridge
{

typedef graphics::Shader Shader;
typedef graphics::Shader::UniformInfo Uniform;


inline void setFloats(Shader *s, const Uniform *u, const double *v, int n){
	if (!s || !u || u->baseType != Shader::UNIFORM_FLOAT)
		return;
	int c = u->components < n ? u->components : n;
	for (int i = 0; i < c; i++)
		u->floats[i] = (float) v[i];
	s->updateUniform(u, 1);
}


inline void setMat4RowMajor(Shader *s, const Uniform *u, const double *m){
	if (!s || !u || u->baseType != Shader::UNIFORM_MATRIX || u->matrix.columns != 4 || u->matrix.rows != 4)
		return;
	for (int row = 0; row < 4; row++)
		for (int col = 0; col < 4; col++)
			u->floats[col * 4 + row] = (float) m[row * 4 + col];
	s->updateUniform(u, 1);
}

inline void setBool(Shader *s, const Uniform *u, bool v){
	if (!s || !u || u->baseType != Shader::UNIFORM_BOOL)
		return;
	u->ints[0] = v ? 1 : 0;
	s->updateUniform(u, 1);
}

}

}
}
