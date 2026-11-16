#pragma once

#include "common/runtime.h"

#include <cmath>
#include <cstdint>
#include <vector>

namespace love
{

namespace mgold
{

struct V3
{
	double x, y, z;
};

static inline V3 operator+(const V3 &a, const V3 &b) { return V3{ a.x + b.x, a.y + b.y, a.z + b.z }; }
static inline V3 operator-(const V3 &a, const V3 &b) { return V3{ a.x - b.x, a.y - b.y, a.z - b.z }; }
static inline V3 operator*(const V3 &a, double s) { return V3{ a.x * s, a.y * s, a.z * s }; }

static inline double dot(const V3 &a, const V3 &b)
{
	return a.x * b.x + a.y * b.y + a.z * b.z;
}

static inline V3 cross(const V3 &a, const V3 &b)
{
	return V3{ a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
}

static inline double length(const V3 &a)
{
	return std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z);
}

static inline V3 normalize(const V3 &a)
{
	double len = length(a);
	if (len == 0.0)
		return V3{ 0.0, 0.0, 0.0 };
	return V3{ a.x / len, a.y / len, a.z / len };
}

static inline V3 loadV3(const double *p)
{
	return V3{ p[0], p[1], p[2] };
}

struct Hit
{
	double len;
	V3 p;
	V3 n;
};



static const int TRI_STRIDE = 12;
static const int BOX_STRIDE = 6;
static const double EPS = 1e-5;

struct CollisionCache
{

	std::vector<double> localVerts;
	int localCount = 0;
	bool meshDirty = true;

	std::vector<double> tris;
	std::vector<double> boxes;
	std::vector<int> allTris;
	int triCount = 0;
	double aabbMin[3] = { 0, 0, 0 };
	double aabbMax[3] = { 0, 0, 0 };



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
	std::vector<int> candidates;
};

struct ShapeQuery
{
	int type;
	V3 center;
	V3 tip, base, a, b, norm;
	double radius;
	double qmin[3], qmax[3];
};

void MGOLD_makeSphereQuery(ShapeQuery &q, const V3 &center, double radius);
void MGOLD_makeCapsuleQuery(ShapeQuery &q, const V3 &tip, const V3 &base, double radius);

static inline bool MGOLD_boxOverlap(const double *bmin, const double *bmax, const double *qmin, const double *qmax)
{
	return !(qmax[0] < bmin[0] || qmin[0] > bmax[0] ||
			 qmax[1] < bmin[1] || qmin[1] > bmax[1] ||
			 qmax[2] < bmin[2] || qmin[2] > bmax[2]);
}

CollisionCache *MGOLD_getCollisionCache(lua_State *L, int idx);


void MGOLD_refreshCollisionCache(lua_State *L, int idx, CollisionCache *c);


int MGOLD_buildZones(CollisionCache *c, double zoneSize);

bool MGOLD_queryCache(CollisionCache *c, const ShapeQuery &q, Hit &out, bool broad = true);
bool MGOLD_cacheRay(CollisionCache *c, const V3 &src, const V3 &dir, Hit &out);
bool MGOLD_cachePoint(CollisionCache *c, const V3 &src, Hit &out);

}
}
