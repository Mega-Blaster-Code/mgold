#pragma once

#include "common/runtime.h"

#include <cmath>
#include <cstdint>
#include <vector>

// Nucleo de colisao 100% nativo, compartilhado entre Collisions.cpp (API Lua) e
// Physics.cpp (simulacao). Nada aqui usa a API do Lua nem do LOVE, exceto as duas funcoes
// que reconstroem o cache a partir da tabela do modelo.

namespace love
{

namespace mgold
{

// ---------------------------------------------------------------------------------------------
// matematica
// ---------------------------------------------------------------------------------------------

struct V3{
	double x, y, z;
};

static inline V3 operator+(const V3 &a, const V3 &b){ return V3{a.x + b.x, a.y + b.y, a.z + b.z}; }
static inline V3 operator-(const V3 &a, const V3 &b){ return V3{a.x - b.x, a.y - b.y, a.z - b.z}; }
static inline V3 operator*(const V3 &a, double s){ return V3{a.x * s, a.y * s, a.z * s}; }

static inline double dot(const V3 &a, const V3 &b){
	return a.x * b.x + a.y * b.y + a.z * b.z;
}

static inline V3 cross(const V3 &a, const V3 &b){
	return V3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

static inline double length(const V3 &a){
	return std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z);
}

static inline V3 normalize(const V3 &a){
	double len = length(a);
	if (len == 0.0)
		return V3{0.0, 0.0, 0.0};
	return V3{a.x / len, a.y / len, a.z / len};
}

static inline V3 loadV3(const double *p){
	return V3{p[0], p[1], p[2]};
}

struct Hit{
	double len;
	V3 p;
	V3 n;
};

// ---------------------------------------------------------------------------------------------
// cache de um modelo em world space
// ---------------------------------------------------------------------------------------------
//
// Structure-of-arrays: o broad phase por triangulo so toca o array "boxes" (contiguo, 48 bytes
// por triangulo); os vertices so sao lidos para os triangulos que passam no teste.
//
//   tris [t*12 + 0..8]  = x0,y0,z0, x1,y1,z1, x2,y2,z2   (world space)
//   tris [t*12 + 9..11] = nx,ny,nz                       (normal da face)
//   boxes[t*6  + 0..2]  = minX,minY,minZ
//   boxes[t*6  + 3..5]  = maxX,maxY,maxZ

static const int TRI_STRIDE = 12;
static const int BOX_STRIDE = 6;
static const double EPS = 1e-5;

struct CollisionCache{
	// vertices da malha em espaco local (lidos de mesh.verts UMA vez, nao a cada rebuild)
	std::vector<double> localVerts;
	int localCount = 0;
	bool meshDirty = true;

	std::vector<double> tris;
	std::vector<double> boxes;
	std::vector<int> allTris;
	int triCount = 0;
	double aabbMin[3] = {0, 0, 0};
	double aabbMax[3] = {0, 0, 0};

	// zonas: hash espacial achatado (open addressing). Cada celula aponta para um trecho
	// contiguo de zoneTris, sem ponteiros e sem alocacao por celula.
	bool hasZones = false;
	double zoneSize = 0;
	double zoneInv = 0;
	std::vector<uint64_t> zoneKeys;
	std::vector<int> zoneStart;
	std::vector<int> zoneCount;
	std::vector<int> zoneTris;
	uint32_t zoneMask = 0;
	int zoneCells = 0;

	std::vector<uint32_t> seen;
	uint32_t stamp = 0;
	std::vector<int> candidates; // scratch reaproveitado entre queries
};

// ---------------------------------------------------------------------------------------------
// consulta de forma (esfera / capsula). Preparada UMA vez e testada contra N modelos.
// ---------------------------------------------------------------------------------------------

struct ShapeQuery{
	int type; // 0 = esfera, 1 = capsula
	V3 center; // esfera
	V3 tip, base, a, b, norm; // capsula
	double radius;
	double qmin[3], qmax[3];
};

void MGOLD_makeSphereQuery(ShapeQuery &q, const V3 &center, double radius);
void MGOLD_makeCapsuleQuery(ShapeQuery &q, const V3 &tip, const V3 &base, double radius);

static inline bool MGOLD_boxOverlap(const double *bmin, const double *bmax, const double *qmin, const double *qmax){
	return !(qmax[0] < bmin[0] || qmin[0] > bmax[0] ||
	         qmax[1] < bmin[1] || qmin[1] > bmax[1] ||
	         qmax[2] < bmin[2] || qmin[2] > bmax[2]);
}

// ---------------------------------------------------------------------------------------------
// exportado por Collisions.cpp
// ---------------------------------------------------------------------------------------------

// garante que o cache do modelo (tabela no indice idx) existe e esta valido
CollisionCache *MGOLD_getCollisionCache(lua_State *L, int idx);

// reconstroi o cache (usa os vertices locais ja guardados; so le mesh.verts se mudou)
void MGOLD_refreshCollisionCache(lua_State *L, int idx, CollisionCache *c);

// monta o hash espacial; devolve o numero de celulas ocupadas
int MGOLD_buildZones(CollisionCache *c, double zoneSize);

// consultas nativas (o cache precisa estar valido). A normal de saida ja vem normalizada.
// broad = false pula o teste contra a AABB do modelo (quando o chamador ja fez)
bool MGOLD_queryCache(CollisionCache *c, const ShapeQuery &q, Hit &out, bool broad = true);
bool MGOLD_cacheRay(CollisionCache *c, const V3 &src, const V3 &dir, Hit &out);
bool MGOLD_cachePoint(CollisionCache *c, const V3 &src, Hit &out);

}
}