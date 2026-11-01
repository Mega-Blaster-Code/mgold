#include "modules/mgold/Collisions.h"
#include "modules/mgold/CollisionCore.h"

#include <cmath>
#include <cstdint>
#include <vector>
#include <algorithm>
#include <limits>

#if LUA_VERSION_NUM >= 502
#define m_objlen(L, i) lua_rawlen(L, i)
#else
#define m_objlen(L, i) lua_objlen(L, i)
#endif

namespace love
{

namespace mgold
{

static const double TINY = 2.2204460492503131e-16;
static const uint64_t EMPTY_KEY = ~(uint64_t) 0;

// ---------------------------------------------------------------------------------------------
// chaves internadas (uma vez, no registry). Evita strlen + hash + busca no intern table a cada
// acesso a um campo do modelo: agora e so um lua_rawgeti por indice inteiro.
// ---------------------------------------------------------------------------------------------

enum CKey{
	CK_X, CK_Y, CK_Z,
	CK_CACHE, CK_DIRTY, CK_MESH, CK_VERTS,
	CK_POSITION, CK_SCALE, CK_ROTATION,
	CK_AABB, CK_MIN, CK_MAX,
	CK_COUNT
};

static const char *CKEY_NAMES[CK_COUNT] = {
	"x", "y", "z",
	"collisionCache", "collisionDirty", "mesh", "verts",
	"position", "scale", "rotation",
	"aabb", "min", "max"
};

static int ckeyRef[CK_COUNT];
static int cacheMetaRef = LUA_NOREF;

static inline void pushKey(lua_State *L, CKey k){
	lua_rawgeti(L, LUA_REGISTRYINDEX, ckeyRef[k]);
}

static int abs_index(lua_State *L, int index){
	if (index > 0 || index <= LUA_REGISTRYINDEX)
		return index;

	return lua_gettop(L) + index + 1;
}

// componente i (0..2) de um vec3: tenta o campo cru x/y/z; se nao existir cai no [i+1]
static double compOf(lua_State *L, int idx, int i){
	pushKey(L, (CKey) (CK_X + i));
	lua_gettable(L, idx);
	if (!lua_isnumber(L, -1)){
		lua_pop(L, 1);
		lua_pushinteger(L, i + 1);
		lua_gettable(L, idx);
	}
	double v = lua_tonumber(L, -1);
	lua_pop(L, 1);
	return v;
}

static V3 getVec3(lua_State *L, int idx){
	idx = abs_index(L, idx);
	return V3{compOf(L, idx, 0), compOf(L, idx, 1), compOf(L, idx, 2)};
}

// self[key] como vec3; se nao existir devolve def
static V3 getVec3Field(lua_State *L, int idx, CKey key, const V3 &def){
	idx = abs_index(L, idx);
	pushKey(L, key);
	lua_gettable(L, idx);
	V3 r = def;
	if (!lua_isnoneornil(L, -1))
		r = getVec3(L, lua_gettop(L));
	lua_pop(L, 1);
	return r;
}

// ---------------------------------------------------------------------------------------------
// cache (userdata dentro da tabela do modelo)
// ---------------------------------------------------------------------------------------------

static int w_cache_gc(lua_State *L){
	CollisionCache **p = (CollisionCache **) lua_touserdata(L, 1);
	if (p && *p){
		delete *p;
		*p = nullptr;
	}
	return 0;
}

static CollisionCache *getCache(lua_State *L, int idx){
	idx = abs_index(L, idx);
	CollisionCache *cache = nullptr;

	pushKey(L, CK_CACHE);
	lua_rawget(L, idx);
	if (lua_type(L, -1) == LUA_TUSERDATA){
		CollisionCache **p = (CollisionCache **) lua_touserdata(L, -1);
		if (p)
			cache = *p;
	}
	lua_pop(L, 1);

	return cache;
}

static CollisionCache *createCache(lua_State *L, int idx){
	idx = abs_index(L, idx);

	pushKey(L, CK_CACHE);
	CollisionCache **p = (CollisionCache **) lua_newuserdata(L, sizeof(CollisionCache *));
	*p = new CollisionCache();
	lua_rawgeti(L, LUA_REGISTRYINDEX, cacheMetaRef);
	lua_setmetatable(L, -2);
	lua_rawset(L, idx);

	return *p;
}

static inline void rotateXYZWithTrig(double &x, double &y, double &z, double cx, double sx, double cy, double sy, double cz, double sz){
	double nx, ny, nz;

	// rotate X
	ny = y * cx - z * sx;
	nz = y * sx + z * cx;
	y = ny; z = nz;

	// rotate Y
	nx = x * cy + z * sy;
	nz = -x * sy + z * cy;
	x = nx; z = nz;

	// rotate Z
	nx = x * cz - y * sz;
	ny = x * sz + y * cz;
	x = nx; y = ny;
}

// recalcula triangulos em world space + AABB do modelo
static void updateWorldVerts(lua_State *L, int idx, CollisionCache *c){
	idx = abs_index(L, idx);

	// mesh.verts: so e lido quando a malha mudou (ou foi marcada como suja)
	pushKey(L, CK_MESH);
	lua_gettable(L, idx);
	if (!lua_istable(L, -1))
		luaL_error(L, "collisions: model.mesh must be a table");
	pushKey(L, CK_VERTS);
	lua_gettable(L, -2);
	if (!lua_istable(L, -1))
		luaL_error(L, "collisions: model.mesh.verts must be a table");
	int vt = lua_gettop(L);

	int vertCount = ((int) m_objlen(L, vt) / 3) * 3;
	if (c->meshDirty || vertCount != c->localCount){
		c->localVerts.resize((size_t) vertCount * 3);
		double *dst = c->localVerts.data();
		for (int i = 0; i < vertCount; i++){
			lua_rawgeti(L, vt, i + 1);
			int v = lua_gettop(L);
			lua_rawgeti(L, v, 1);
			lua_rawgeti(L, v, 2);
			lua_rawgeti(L, v, 3);
			dst[i * 3 + 0] = lua_tonumber(L, -3);
			dst[i * 3 + 1] = lua_tonumber(L, -2);
			dst[i * 3 + 2] = lua_tonumber(L, -1);
			lua_pop(L, 4);
		}
		c->localCount = vertCount;
		c->meshDirty = false;
	}
	lua_pop(L, 2); // verts, mesh

	V3 pos = getVec3Field(L, idx, CK_POSITION, V3{0, 0, 0});
	V3 scl = getVec3Field(L, idx, CK_SCALE, V3{1, 1, 1});
	V3 rot = getVec3Field(L, idx, CK_ROTATION, V3{0, 0, 0});

	bool rotated = rot.x != 0 || rot.y != 0 || rot.z != 0;

	// sin/cos UMA vez por atualizacao (e nenhum se nao houver rotacao)
	double cx = 1, sx = 0, cy = 1, sy = 0, cz = 1, sz = 0;
	if (rotated){
		cx = std::cos(rot.x); sx = std::sin(rot.x);
		cy = std::cos(rot.y); sy = std::sin(rot.y);
		cz = std::cos(rot.z); sz = std::sin(rot.z);
	}

	int triCount = vertCount / 3;
	c->tris.resize((size_t) triCount * TRI_STRIDE);
	c->boxes.resize((size_t) triCount * BOX_STRIDE);

	double mn[3] = {std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity()};
	double mx[3] = {-std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity()};

	const double *local = c->localVerts.data();
	double *tris = c->tris.data();
	double *boxes = c->boxes.data();

	for (int t = 0; t < triCount; t++){
		V3 p[3];
		for (int k = 0; k < 3; k++){
			const double *v = local + ((size_t) t * 3 + k) * 3;
			double x = v[0] * scl.x, y = v[1] * scl.y, z = v[2] * scl.z;
			if (rotated)
				rotateXYZWithTrig(x, y, z, cx, sx, cy, sy, cz, sz);
			p[k] = V3{x + pos.x, y + pos.y, z + pos.z};
		}

		// normal da face (NAO a normal suave do vertice)
		V3 n = normalize(cross(p[1] - p[0], p[2] - p[0]));

		double *o = tris + (size_t) t * TRI_STRIDE;
		for (int k = 0; k < 3; k++){
			o[k * 3 + 0] = p[k].x;
			o[k * 3 + 1] = p[k].y;
			o[k * 3 + 2] = p[k].z;
		}
		o[9] = n.x; o[10] = n.y; o[11] = n.z;

		double *bx = boxes + (size_t) t * BOX_STRIDE;
		bx[0] = std::min(p[0].x, std::min(p[1].x, p[2].x));
		bx[1] = std::min(p[0].y, std::min(p[1].y, p[2].y));
		bx[2] = std::min(p[0].z, std::min(p[1].z, p[2].z));
		bx[3] = std::max(p[0].x, std::max(p[1].x, p[2].x));
		bx[4] = std::max(p[0].y, std::max(p[1].y, p[2].y));
		bx[5] = std::max(p[0].z, std::max(p[1].z, p[2].z));

		for (int k = 0; k < 3; k++){
			if (bx[k] < mn[k]) mn[k] = bx[k];
			if (bx[3 + k] > mx[k]) mx[k] = bx[3 + k];
		}
	}

	if ((int) c->allTris.size() != triCount){
		c->allTris.resize(triCount);
		for (int i = 0; i < triCount; i++)
			c->allTris[i] = i;
	}

	c->triCount = triCount;
	for (int k = 0; k < 3; k++){
		c->aabbMin[k] = mn[k];
		c->aabbMax[k] = mx[k];
	}

	// as zonas antigas ficam invalidas
	c->hasZones = false;

	pushKey(L, CK_DIRTY);
	lua_pushboolean(L, 0);
	lua_rawset(L, idx);
}

// garante que o cache existe e esta valido
static CollisionCache *ensureWorld(lua_State *L, int idx){
	idx = abs_index(L, idx);
	luaL_checktype(L, idx, LUA_TTABLE);

	CollisionCache *cache = getCache(L, idx);
	bool fresh = false;
	if (!cache){
		cache = createCache(L, idx);
		fresh = true;
	}

	pushKey(L, CK_DIRTY);
	lua_rawget(L, idx);
	bool clean = lua_isboolean(L, -1) && !lua_toboolean(L, -1);
	lua_pop(L, 1);

	if (fresh || !clean)
		updateWorldVerts(L, idx, cache);

	return cache;
}

CollisionCache *MGOLD_getCollisionCache(lua_State *L, int idx){
	return ensureWorld(L, idx);
}

void MGOLD_refreshCollisionCache(lua_State *L, int idx, CollisionCache *c){
	updateWorldVerts(L, idx, c);
}

// ---------------------------------------------------------------------------------------------
// zonas (hash espacial achatado)
// ---------------------------------------------------------------------------------------------

// celulas inteiras empacotadas em um uint64 (sem aliasing, sem strings)
static inline uint64_t zoneKey(long long cx, long long cy, long long cz){
	const long long OFF = 1048576; // 2^20
	const uint64_t SPAN = 2097152; // 2^21
	return (((uint64_t) (cx + OFF)) * SPAN + (uint64_t) (cy + OFF)) * SPAN + (uint64_t) (cz + OFF);
}

static inline uint32_t hashKey(uint64_t k){
	k *= 0x9E3779B97F4A7C15ull;
	return (uint32_t) (k >> 32);
}

static inline bool findZone(const CollisionCache *c, uint64_t key, int &start, int &count){
	uint32_t slot = hashKey(key) & c->zoneMask;
	for (;;){
		uint64_t k = c->zoneKeys[slot];
		if (k == key){
			start = c->zoneStart[slot];
			count = c->zoneCount[slot];
			return true;
		}
		if (k == EMPTY_KEY)
			return false;
		slot = (slot + 1) & c->zoneMask;
	}
}

struct ZonePair{
	uint64_t key;
	int tri;
};

int MGOLD_buildZones(CollisionCache *c, double zoneSize){
	double inv = 1.0 / zoneSize;

	std::vector<ZonePair> pairs;
	pairs.reserve((size_t) c->triCount * 2);

	for (int t = 0; t < c->triCount; t++){
		const double *b = &c->boxes[(size_t) t * BOX_STRIDE];

		long long cx1 = (long long) std::floor(b[0] * inv), cx2 = (long long) std::floor(b[3] * inv);
		long long cy1 = (long long) std::floor(b[1] * inv), cy2 = (long long) std::floor(b[4] * inv);
		long long cz1 = (long long) std::floor(b[2] * inv), cz2 = (long long) std::floor(b[5] * inv);

		// o triangulo entra em toda celula que a AABB dele toca
		for (long long cx = cx1; cx <= cx2; cx++)
			for (long long cy = cy1; cy <= cy2; cy++)
				for (long long cz = cz1; cz <= cz2; cz++)
					pairs.push_back(ZonePair{zoneKey(cx, cy, cz), t});
	}

	std::sort(pairs.begin(), pairs.end(), [](const ZonePair &a, const ZonePair &b){
		return a.key != b.key ? a.key < b.key : a.tri < b.tri;
	});

	// celulas unicas
	int unique = 0;
	for (size_t i = 0; i < pairs.size(); i++)
		if (i == 0 || pairs[i].key != pairs[i - 1].key)
			unique++;

	uint32_t size = 16;
	while (size < (uint32_t) unique * 2)
		size <<= 1;

	c->zoneKeys.assign(size, EMPTY_KEY);
	c->zoneStart.assign(size, 0);
	c->zoneCount.assign(size, 0);
	c->zoneMask = size - 1;
	c->zoneTris.resize(pairs.size());

	size_t i = 0;
	while (i < pairs.size()){
		size_t j = i;
		while (j < pairs.size() && pairs[j].key == pairs[i].key){
			c->zoneTris[j] = pairs[j].tri;
			j++;
		}

		uint32_t slot = hashKey(pairs[i].key) & c->zoneMask;
		while (c->zoneKeys[slot] != EMPTY_KEY)
			slot = (slot + 1) & c->zoneMask;
		c->zoneKeys[slot] = pairs[i].key;
		c->zoneStart[slot] = (int) i;
		c->zoneCount[slot] = (int) (j - i);

		i = j;
	}

	c->zoneSize = zoneSize;
	c->zoneInv = inv;
	c->hasZones = true;
	c->zoneCells = unique;
	c->seen.assign(c->triCount, 0);
	c->stamp = 0;

	return unique;
}

// ---------------------------------------------------------------------------------------------
// broad phase
// ---------------------------------------------------------------------------------------------

static inline bool broadPhase(const CollisionCache *c, const double *qmin, const double *qmax){
	return MGOLD_boxOverlap(c->aabbMin, c->aabbMax, qmin, qmax);
}

struct Cands{
	const int *p;
	int n;
};

// triangulos candidatos que tocam a AABB da consulta.
// Caso comum (consulta dentro de UMA celula): devolve o trecho de zoneTris direto,
// sem copiar nem deduplicar.
static Cands getCandidates(CollisionCache *c, const double *qmin, const double *qmax){
	Cands all = {c->allTris.data(), c->triCount};
	if (!c->hasZones)
		return all;

	double inv = c->zoneInv;
	long long cx1 = (long long) std::floor(qmin[0] * inv), cx2 = (long long) std::floor(qmax[0] * inv);
	long long cy1 = (long long) std::floor(qmin[1] * inv), cy2 = (long long) std::floor(qmax[1] * inv);
	long long cz1 = (long long) std::floor(qmin[2] * inv), cz2 = (long long) std::floor(qmax[2] * inv);

	// consulta gigante: mais barato testar tudo do que varrer milhoes de celulas
	double cells = (double) (cx2 - cx1 + 1) * (double) (cy2 - cy1 + 1) * (double) (cz2 - cz1 + 1);
	if (cells > 4096.0)
		return all;

	if (cx1 == cx2 && cy1 == cy2 && cz1 == cz2){
		int s, n;
		Cands r = {nullptr, 0};
		if (findZone(c, zoneKey(cx1, cy1, cz1), s, n)){
			r.p = &c->zoneTris[s];
			r.n = n;
		}
		return r;
	}

	// varias celulas: dedupe por carimbo, sem alocar "seen" por query
	uint32_t stamp = ++c->stamp;
	if (stamp == 0){
		std::fill(c->seen.begin(), c->seen.end(), 0);
		stamp = c->stamp = 1;
	}

	std::vector<int> &list = c->candidates;
	list.clear();

	for (long long cx = cx1; cx <= cx2; cx++)
		for (long long cy = cy1; cy <= cy2; cy++)
			for (long long cz = cz1; cz <= cz2; cz++){
				int s, n;
				if (!findZone(c, zoneKey(cx, cy, cz), s, n))
					continue;
				const int *zt = &c->zoneTris[s];
				for (int i = 0; i < n; i++){
					int t = zt[i];
					if (c->seen[t] != stamp){
						c->seen[t] = stamp;
						list.push_back(t);
					}
				}
			}

	Cands r = {list.data(), (int) list.size()};
	return r;
}

// ---------------------------------------------------------------------------------------------
// narrow phase
// ---------------------------------------------------------------------------------------------

static inline V3 closestPointOnLineSegment(const V3 &a, const V3 &b, const V3 &p){
	V3 ab = b - a;
	double denom = dot(ab, ab);
	double t = denom > 0.0 ? dot(p - a, ab) / denom : 0.0;
	t = std::min(1.0, std::max(0.0, t));
	return a + ab * t;
}

// ray - triangle (CPML / lighthouse3d)
static bool triangleRay(const V3 &p0, const V3 &p1, const V3 &p2, const V3 &n, const V3 &src, const V3 &dir, Hit &out){
	V3 e1 = p1 - p0;
	V3 e2 = p2 - p0;
	V3 h = cross(dir, e2);
	double a = dot(h, e1);

	if (std::fabs(a) <= TINY)
		return false;

	V3 s = src - p0;
	double u = dot(s, h) / a;
	if (u < 0 || u > 1)
		return false;

	V3 q = cross(s, e1);
	double v = dot(dir, q) / a;
	if (v < 0 || u + v > 1)
		return false;

	double t = dot(q, e2) / a;
	if (t >= TINY){
		out.len = t;
		out.p = src + dir * t;
		out.n = n;
		return true;
	}
	return false;
}

static inline bool finishHit(const V3 &src, const V3 &itx, const V3 &triN, Hit &out){
	V3 d = src - itx;
	out.p = itx;
	if (d.x == 0 && d.y == 0 && d.z == 0){
		// esfera exatamente em cima do triangulo: usa a normal da face
		out.len = 0;
		out.n = triN;
	}
	else{
		out.len = length(d);
		out.n = d;
	}
	return true;
}

static inline bool insideTriangle(const V3 &p0, const V3 &p1, const V3 &p2, const V3 &n, const V3 &itx){
	V3 c0 = cross(itx - p0, p1 - p0);
	V3 c1 = cross(itx - p1, p2 - p1);
	V3 c2 = cross(itx - p2, p0 - p2);
	return dot(c0, n) <= 0 && dot(c1, n) <= 0 && dot(c2, n) <= 0;
}

// esfera - triangulo (wickedengine)
static bool triangleSphere(const V3 &p0, const V3 &p1, const V3 &p2, const V3 &tn, const V3 &src, double radius, Hit &out){
	double dist = dot(src - p0, tn);
	if (dist < -radius || dist > radius)
		return false;

	V3 itx = src - tn * dist;

	if (insideTriangle(p0, p1, p2, tn, itx))
		return finishHit(src, itx, tn, out);

	// itx fora do triangulo: ponto mais proximo em cada aresta
	double radiussq = radius * radius;
	bool found = false;
	double smallest = 0;

	const V3 *edges[3][2] = {{&p0, &p1}, {&p1, &p2}, {&p2, &p0}};
	for (int i = 0; i < 3; i++){
		V3 c = closestPointOnLineSegment(*edges[i][0], *edges[i][1], src);
		V3 d = src - c;
		double dsq = dot(d, d);
		if (dsq <= radiussq && (!found || dsq < smallest)){
			found = true;
			smallest = dsq;
			itx = c;
		}
	}

	if (found)
		return finishHit(src, itx, tn, out);

	return false;
}

// ponto mais proximo do triangulo
static bool trianglePoint(const V3 &p0, const V3 &p1, const V3 &p2, const V3 &tn, const V3 &src, Hit &out){
	double dist = dot(src - p0, tn);
	V3 itx = src - tn * dist;

	if (insideTriangle(p0, p1, p2, tn, itx))
		return finishHit(src, itx, tn, out);

	const V3 *edges[3][2] = {{&p0, &p1}, {&p1, &p2}, {&p2, &p0}};
	double smallest = 0;
	for (int i = 0; i < 3; i++){
		V3 c = closestPointOnLineSegment(*edges[i][0], *edges[i][1], src);
		V3 d = src - c;
		double dsq = dot(d, d);
		if (i == 0 || dsq < smallest){
			smallest = dsq;
			itx = c;
		}
	}

	return finishHit(src, itx, tn, out);
}

// capsula - triangulo (a, b = extremos da linha central, capn = direcao da capsula)
static bool triangleCapsule(const V3 &p0, const V3 &p1, const V3 &p2, const V3 &n, const V3 &base, const V3 &a, const V3 &b, const V3 &capn, double radius, Hit &out){
	double dotOfNormals = std::fabs(dot(n, capn));

	// capsula paralela ao triangulo: usa um ponto qualquer dele
	V3 ref = p0;

	if (dotOfNormals > 0){
		double t = dot(n, (p0 - base) * (1.0 / dotOfNormals));
		V3 planeItx = base + capn * t;

		Hit tmp;
		trianglePoint(p0, p1, p2, n, planeItx, tmp);
		ref = tmp.p;
	}

	V3 c = closestPointOnLineSegment(a, b, ref);
	return triangleSphere(p0, p1, p2, n, c, radius, out);
}

// ---------------------------------------------------------------------------------------------
// consultas nativas
// ---------------------------------------------------------------------------------------------

void MGOLD_makeSphereQuery(ShapeQuery &q, const V3 &center, double radius){
	double r = radius + EPS;
	q.type = 0;
	q.center = center;
	q.radius = radius;
	q.qmin[0] = center.x - r; q.qmin[1] = center.y - r; q.qmin[2] = center.z - r;
	q.qmax[0] = center.x + r; q.qmax[1] = center.y + r; q.qmax[2] = center.z + r;
}

void MGOLD_makeCapsuleQuery(ShapeQuery &q, const V3 &tip, const V3 &base, double radius){
	double r = radius + EPS;
	q.type = 1;
	q.tip = tip;
	q.base = base;
	q.radius = radius;
	q.norm = normalize(tip - base);
	q.a = base + q.norm * radius;
	q.b = tip - q.norm * radius;
	q.qmin[0] = std::min(tip.x, base.x) - r; q.qmin[1] = std::min(tip.y, base.y) - r; q.qmin[2] = std::min(tip.z, base.z) - r;
	q.qmax[0] = std::max(tip.x, base.x) + r; q.qmax[1] = std::max(tip.y, base.y) + r; q.qmax[2] = std::max(tip.z, base.z) + r;
}

bool MGOLD_queryCache(CollisionCache *c, const ShapeQuery &q, Hit &out, bool broad){
	// rejeicao no nivel do modelo
	if (broad && !broadPhase(c, q.qmin, q.qmax))
		return false;

	Cands cd = getCandidates(c, q.qmin, q.qmax);

	const double *boxes = c->boxes.data();
	const double *tris = c->tris.data();

	bool found = false;
	Hit best = {};

	for (int i = 0; i < cd.n; i++){
		int t = cd.p[i];
		const double *b = boxes + (size_t) t * BOX_STRIDE;

		// rejeicao por triangulo: so toca o array de caixas
		if (b[0] > q.qmax[0] || b[3] < q.qmin[0] ||
		    b[1] > q.qmax[1] || b[4] < q.qmin[1] ||
		    b[2] > q.qmax[2] || b[5] < q.qmin[2])
			continue;

		const double *o = tris + (size_t) t * TRI_STRIDE;
		Hit h;
		bool ok;
		if (q.type == 0)
			ok = triangleSphere(loadV3(o), loadV3(o + 3), loadV3(o + 6), loadV3(o + 9), q.center, q.radius, h);
		else
			ok = triangleCapsule(loadV3(o), loadV3(o + 3), loadV3(o + 6), loadV3(o + 9), q.base, q.a, q.b, q.norm, q.radius, h);

		if (ok && (!found || h.len < best.len)){
			best = h;
			found = true;
		}
	}

	if (found){
		best.n = normalize(best.n);
		out = best;
	}
	return found;
}

bool MGOLD_cacheRay(CollisionCache *c, const V3 &src, const V3 &dir, Hit &out){
	// rejeicao rapida: o raio cruza a AABB do modelo?
	const double o[3] = {src.x, src.y, src.z};
	const double d[3] = {dir.x, dir.y, dir.z};
	double tmin = 0, tmax = std::numeric_limits<double>::infinity();
	for (int k = 0; k < 3; k++){
		double lo = c->aabbMin[k] - EPS, hi = c->aabbMax[k] + EPS;
		if (std::fabs(d[k]) < 1e-12){
			if (o[k] < lo || o[k] > hi)
				return false;
		}
		else{
			double t1 = (lo - o[k]) / d[k], t2 = (hi - o[k]) / d[k];
			if (t1 > t2)
				std::swap(t1, t2);
			tmin = std::max(tmin, t1);
			tmax = std::min(tmax, t2);
			if (tmin > tmax)
				return false;
		}
	}

	bool found = false;
	Hit best = {};

	const double *tris = c->tris.data();
	for (int t = 0; t < c->triCount; t++){
		const double *tr = tris + (size_t) t * TRI_STRIDE;
		Hit h;
		if (triangleRay(loadV3(tr), loadV3(tr + 3), loadV3(tr + 6), loadV3(tr + 9), src, dir, h) && (!found || h.len < best.len)){
			best = h;
			found = true;
		}
	}

	if (found){
		best.n = normalize(best.n);
		out = best;
	}
	return found;
}

bool MGOLD_cachePoint(CollisionCache *c, const V3 &src, Hit &out){
	double q[3] = {src.x, src.y, src.z};
	Cands cd = getCandidates(c, q, q);

	bool found = false;
	Hit best = {};

	for (int i = 0; i < cd.n; i++){
		const double *o = &c->tris[(size_t) cd.p[i] * TRI_STRIDE];
		Hit h;
		if (trianglePoint(loadV3(o), loadV3(o + 3), loadV3(o + 6), loadV3(o + 9), src, h) && (!found || h.len < best.len)){
			best = h;
			found = true;
		}
	}

	if (found){
		best.n = normalize(best.n);
		out = best;
	}
	return found;
}

// ---------------------------------------------------------------------------------------------
// wrappers Lua (so leem argumentos e empilham o resultado)
// ---------------------------------------------------------------------------------------------

static int pushHit(lua_State *L, const Hit &h){
	lua_pushnumber(L, h.len);
	lua_pushnumber(L, h.p.x);
	lua_pushnumber(L, h.p.y);
	lua_pushnumber(L, h.p.z);
	lua_pushnumber(L, h.n.x);
	lua_pushnumber(L, h.n.y);
	lua_pushnumber(L, h.n.z);
	return 7;
}

static int w_prepare(lua_State *L){
	ensureWorld(L, 1);
	return 0;
}

// invalidate(model [, reloadMesh]): marca o cache como velho. Com reloadMesh = true a malha
// (mesh.verts) tambem sera relida; sem isso so a transformacao e refeita.
static int w_invalidate(lua_State *L){
	luaL_checktype(L, 1, LUA_TTABLE);

	pushKey(L, CK_DIRTY);
	lua_pushboolean(L, 1);
	lua_rawset(L, 1);

	CollisionCache *cache = getCache(L, 1);
	if (cache){
		cache->hasZones = false;
		if (lua_toboolean(L, 2))
			cache->meshDirty = true;
	}
	return 0;
}

static int w_createCollisionZones(lua_State *L){
	double zoneSize = luaL_checknumber(L, 2);
	if (zoneSize <= 0)
		return luaL_argerror(L, 2, "zoneSize must be greater than 0");

	CollisionCache *c = ensureWorld(L, 1);
	lua_pushinteger(L, MGOLD_buildZones(c, zoneSize));
	return 1;
}

static int w_rayIntersection(lua_State *L){
	V3 src = V3{luaL_checknumber(L, 2), luaL_checknumber(L, 3), luaL_checknumber(L, 4)};
	V3 dir = V3{luaL_checknumber(L, 5), luaL_checknumber(L, 6), luaL_checknumber(L, 7)};
	CollisionCache *c = ensureWorld(L, 1);
	Hit h;
	return MGOLD_cacheRay(c, src, dir, h) ? pushHit(L, h) : 0;
}

static int w_sphereIntersection(lua_State *L){
	V3 src = V3{luaL_checknumber(L, 2), luaL_checknumber(L, 3), luaL_checknumber(L, 4)};
	double radius = luaL_checknumber(L, 5);
	CollisionCache *c = ensureWorld(L, 1);

	ShapeQuery q;
	MGOLD_makeSphereQuery(q, src, radius);
	Hit h;
	return MGOLD_queryCache(c, q, h) ? pushHit(L, h) : 0;
}

static int w_closestPoint(lua_State *L){
	V3 src = V3{luaL_checknumber(L, 2), luaL_checknumber(L, 3), luaL_checknumber(L, 4)};
	CollisionCache *c = ensureWorld(L, 1);
	Hit h;
	return MGOLD_cachePoint(c, src, h) ? pushHit(L, h) : 0;
}

static int w_capsuleIntersection(lua_State *L){
	V3 tip = V3{luaL_checknumber(L, 2), luaL_checknumber(L, 3), luaL_checknumber(L, 4)};
	V3 base = V3{luaL_checknumber(L, 5), luaL_checknumber(L, 6), luaL_checknumber(L, 7)};
	double radius = luaL_checknumber(L, 8);
	CollisionCache *c = ensureWorld(L, 1);

	ShapeQuery q;
	MGOLD_makeCapsuleQuery(q, tip, base, radius);
	Hit h;
	return MGOLD_queryCache(c, q, h) ? pushHit(L, h) : 0;
}

// ---------------------------------------------------------------------------------------------
// AABB
// ---------------------------------------------------------------------------------------------

static int w_generateAABB(lua_State *L){
	luaL_checktype(L, 1, LUA_TTABLE);

	pushKey(L, CK_MESH);
	lua_gettable(L, 1);
	if (!lua_istable(L, -1))
		return luaL_error(L, "collisions: model.mesh must be a table");
	pushKey(L, CK_VERTS);
	lua_gettable(L, -2);
	if (!lua_istable(L, -1))
		return luaL_error(L, "collisions: model.mesh.verts must be a table");
	int vt = lua_gettop(L);

	double mn[3] = {std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity()};
	double mx[3] = {-std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity()};

	int n = (int) m_objlen(L, vt);
	for (int i = 1; i <= n; i++){
		lua_rawgeti(L, vt, i);
		for (int k = 0; k < 3; k++){
			lua_rawgeti(L, -1, k + 1);
			double v = lua_tonumber(L, -1);
			lua_pop(L, 1);
			if (v < mn[k]) mn[k] = v;
			if (v > mx[k]) mx[k] = v;
		}
		lua_pop(L, 1);
	}
	lua_pop(L, 2);

	lua_newtable(L); // aabb

	lua_newtable(L);
	for (int k = 0; k < 3; k++){
		lua_pushnumber(L, mn[k]);
		lua_rawseti(L, -2, k + 1);
	}
	pushKey(L, CK_MIN);
	lua_insert(L, -2);
	lua_rawset(L, -3);

	lua_newtable(L);
	for (int k = 0; k < 3; k++){
		lua_pushnumber(L, mx[k]);
		lua_rawseti(L, -2, k + 1);
	}
	pushKey(L, CK_MAX);
	lua_insert(L, -2);
	lua_rawset(L, -3);

	pushKey(L, CK_AABB);
	lua_pushvalue(L, -2);
	lua_settable(L, 1);

	return 1;
}

// AABB do modelo (posicao + escala), com min/max trocados se a escala for negativa
static void getAABBLocal(lua_State *L, int idx, V3 &amin, V3 &amax){
	pushKey(L, CK_AABB);
	lua_gettable(L, idx);
	if (!lua_istable(L, -1))
		luaL_error(L, "collisions: model has no aabb (call generateAABB first)");
	int ab = lua_gettop(L);

	pushKey(L, CK_MIN);
	lua_gettable(L, ab);
	amin = getVec3(L, lua_gettop(L));
	lua_pop(L, 1);
	pushKey(L, CK_MAX);
	lua_gettable(L, ab);
	amax = getVec3(L, lua_gettop(L));
	lua_pop(L, 2);
}

static void getModelAABB(lua_State *L, int idx, V3 &mn, V3 &mx){
	idx = abs_index(L, idx);
	luaL_checktype(L, idx, LUA_TTABLE);

	V3 amin, amax;
	getAABBLocal(L, idx, amin, amax);

	V3 s = getVec3Field(L, idx, CK_SCALE, V3{1, 1, 1});
	V3 p = getVec3Field(L, idx, CK_POSITION, V3{0, 0, 0});

	double x1 = amin.x * s.x + p.x, x2 = amax.x * s.x + p.x;
	double y1 = amin.y * s.y + p.y, y2 = amax.y * s.y + p.y;
	double z1 = amin.z * s.z + p.z, z2 = amax.z * s.z + p.z;

	mn = V3{std::min(x1, x2), std::min(y1, y2), std::min(z1, z2)};
	mx = V3{std::max(x1, x2), std::max(y1, y2), std::max(z1, z2)};
}

static int w_sphereIntersectsAABB(lua_State *L){
	V3 c = V3{luaL_checknumber(L, 2), luaL_checknumber(L, 3), luaL_checknumber(L, 4)};
	double radius = luaL_checknumber(L, 5);

	V3 mn, mx;
	getModelAABB(L, 1, mn, mx);

	bool hit = (c.x + radius >= mn.x && c.x - radius <= mx.x) &&
	           (c.y + radius >= mn.y && c.y - radius <= mx.y) &&
	           (c.z + radius >= mn.z && c.z - radius <= mx.z);
	lua_pushboolean(L, hit);
	return 1;
}

static int w_capsuleIntersectsAABB(lua_State *L){
	V3 p1 = V3{luaL_checknumber(L, 2), luaL_checknumber(L, 3), luaL_checknumber(L, 4)};
	V3 p2 = V3{luaL_checknumber(L, 5), luaL_checknumber(L, 6), luaL_checknumber(L, 7)};
	double radius = luaL_checknumber(L, 8);

	V3 mn, mx;
	getModelAABB(L, 1, mn, mx);

	double cminx = std::min(p1.x, p2.x) - radius, cmaxx = std::max(p1.x, p2.x) + radius;
	double cminy = std::min(p1.y, p2.y) - radius, cmaxy = std::max(p1.y, p2.y) + radius;
	double cminz = std::min(p1.z, p2.z) - radius, cmaxz = std::max(p1.z, p2.z) + radius;

	bool hit = (cminx <= mx.x && cmaxx >= mn.x) &&
	           (cminy <= mx.y && cmaxy >= mn.y) &&
	           (cminz <= mx.z && cmaxz >= mn.z);
	lua_pushboolean(L, hit);
	return 1;
}

static int w_isIntersectionAABB(lua_State *L){
	V3 amn, amx, bmn, bmx;
	getModelAABB(L, 1, amn, amx);
	getModelAABB(L, 2, bmn, bmx);

	bool hit = amn.x <= bmx.x && amx.x >= bmn.x &&
	           amn.y <= bmx.y && amx.y >= bmn.y &&
	           amn.z <= bmx.z && amx.z >= bmn.z;
	lua_pushboolean(L, hit);
	return 1;
}

static int w_isPointInsideAABB(lua_State *L){
	V3 p = V3{luaL_checknumber(L, 2), luaL_checknumber(L, 3), luaL_checknumber(L, 4)};

	V3 mn, mx;
	getModelAABB(L, 1, mn, mx);

	bool in = p.x >= mn.x && p.x <= mx.x &&
	          p.y >= mn.y && p.y <= mx.y &&
	          p.z >= mn.z && p.z <= mx.z;
	lua_pushboolean(L, in);
	return 1;
}

static int w_getDistanceFrom(lua_State *L){
	V3 p = V3{luaL_checknumber(L, 2), luaL_checknumber(L, 3), luaL_checknumber(L, 4)};
	luaL_checktype(L, 1, LUA_TTABLE);

	V3 pos = getVec3Field(L, 1, CK_POSITION, V3{0, 0, 0});
	lua_pushnumber(L, length(p - pos));
	return 1;
}

// AABB - ray (CPML)
static int w_rayIntersectionAABB(lua_State *L){
	V3 src = V3{luaL_checknumber(L, 2), luaL_checknumber(L, 3), luaL_checknumber(L, 4)};
	V3 dir = normalize(V3{luaL_checknumber(L, 5), luaL_checknumber(L, 6), luaL_checknumber(L, 7)});

	luaL_checktype(L, 1, LUA_TTABLE);

	// aqui o original NAO troca min/max por escala negativa, mantido igual
	V3 amin, amax;
	getAABBLocal(L, 1, amin, amax);

	V3 s = getVec3Field(L, 1, CK_SCALE, V3{1, 1, 1});
	V3 p = getVec3Field(L, 1, CK_POSITION, V3{0, 0, 0});

	double t1 = (amin.x * s.x + p.x - src.x) / dir.x;
	double t2 = (amax.x * s.x + p.x - src.x) / dir.x;
	double t3 = (amin.y * s.y + p.y - src.y) / dir.y;
	double t4 = (amax.y * s.y + p.y - src.y) / dir.y;
	double t5 = (amin.z * s.z + p.z - src.z) / dir.z;
	double t6 = (amax.z * s.z + p.z - src.z) / dir.z;

	double tmin = std::max(std::max(std::min(t1, t2), std::min(t3, t4)), std::min(t5, t6));
	double tmax = std::min(std::min(std::max(t1, t2), std::max(t3, t4)), std::max(t5, t6));

	// AABB inteira atras da origem, ou raio nao toca
	if (tmax < 0 || tmin > tmax){
		lua_pushboolean(L, 0);
		return 1;
	}

	lua_pushnumber(L, tmin);
	lua_pushnumber(L, src.x + dir.x * tmin);
	lua_pushnumber(L, src.y + dir.y * tmin);
	lua_pushnumber(L, src.z + dir.z * tmin);
	return 4;
}

// ---------------------------------------------------------------------------------------------
// registro
// ---------------------------------------------------------------------------------------------

void MGOLD_setCollisionsFunc(lua_State *L){
	// chaves internadas
	for (int i = 0; i < CK_COUNT; i++){
		lua_pushstring(L, CKEY_NAMES[i]);
		ckeyRef[i] = luaL_ref(L, LUA_REGISTRYINDEX);
	}

	// metatable do userdata de cache (libera o CollisionCache no gc)
	lua_newtable(L);
	lua_pushcfunction(L, w_cache_gc);
	lua_setfield(L, -2, "__gc");
	cacheMetaRef = luaL_ref(L, LUA_REGISTRYINDEX);

	lua_newtable(L); // collisions

	struct Entry{ const char *name; lua_CFunction fn; };
	static const Entry entries[] = {
		{"prepare", w_prepare},
		{"invalidate", w_invalidate},
		{"createCollisionZones", w_createCollisionZones},
		{"rayIntersection", w_rayIntersection},
		{"sphereIntersection", w_sphereIntersection},
		{"closestPoint", w_closestPoint},
		{"capsuleIntersection", w_capsuleIntersection},
		{"generateAABB", w_generateAABB},
		{"sphereIntersectsAABB", w_sphereIntersectsAABB},
		{"capsuleIntersectsAABB", w_capsuleIntersectsAABB},
		{"isIntersectionAABB", w_isIntersectionAABB},
		{"isPointInsideAABB", w_isPointInsideAABB},
		{"getDistanceFrom", w_getDistanceFrom},
		{"rayIntersectionAABB", w_rayIntersectionAABB},
	};

	for (const Entry &e : entries){
		lua_pushcfunction(L, e.fn);
		lua_setfield(L, -2, e.name);
	}
}

}
}