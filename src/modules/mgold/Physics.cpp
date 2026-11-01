#include "modules/mgold/Physics.h"
#include "modules/mgold/CollisionCore.h"
#include "modules/mgold/Vectors.h"

#include <cmath>
#include <vector>
#include <algorithm>

namespace love
{

namespace mgold
{

enum Key
{
	K_X, K_Y, K_Z,
	K_POSITION, K_SPEED, K_LASTSPEED, K_NORMAL,
	K_SHAPE, K_RADIUS,
	K_ONGROUND, K_WASONGROUND, K_STEPDOWN,
	K_STATIC, K_STATICTIME, K_CANBESTATIC, K_MANUALUPDATE,
	K_BODYID, K_MODEL, K_ALIVE, K_SCALE,
	K_PHYSICS, K_PHYSICSID, K_COLLISIONDIRTY, K_SETPOSITION,
	K_ONCOLLIDE, K_ONWALLHIT, K_ONCEILINGHIT,
	K_ONLAND, K_ONLEAVEGROUND, K_ONSLEEP, K_ONWAKE,
	K_COUNT
};

static const char *KEY_NAMES[K_COUNT] = {
	"x", "y", "z",
	"position", "speed", "lastSpeed", "normal",
	"shape", "radius",
	"onGround", "wasOnGround", "stepDownSize",
	"static", "staticTime", "canBeStatic", "manualUpdate",
	"bodyId", "model", "alive", "scale",
	"physics", "physicsId", "collisionDirty", "setPosition",
	"onCollide", "onWallHit", "onCeilingHit",
	"onLand", "onLeaveGround", "onSleep", "onWake"
};

static int keyRef[K_COUNT];
static int callbacksRef = LUA_NOREF;
static int bodyMetaRef = LUA_NOREF;

static const int SHAPE_BOX = 1;
static const int SHAPE_SPHERE = 2;
static const int SHAPE_CAPSULE = 4;
static const int SHAPE_PLANE = 5;

static const double CAPSULE_TOP = 0.15;
static const double CAPSULE_BOTTOM = 0.5;
static const double GROUND_NY = 0.7;

struct StaticRec
{
	CollisionCache *cache;
	int ref;
	double zoneSize;
	double bmin[3];
	double bmax[3];
};

struct BodyRec
{
	int ref;
};

static std::vector<StaticRec> staticModels;
static std::vector<BodyRec> bodies;

static inline void pushKey(lua_State *L, Key k)
{
	lua_rawgeti(L, LUA_REGISTRYINDEX, keyRef[k]);
}

static inline void rget(lua_State *L, int idx, Key k)
{
	pushKey(L, k);
	lua_rawget(L, idx);
}

static inline void gget(lua_State *L, int idx, Key k)
{
	pushKey(L, k);
	lua_gettable(L, idx);
}

static inline double getNum(lua_State *L, int idx, Key k)
{
	rget(L, idx, k);
	double v = lua_tonumber(L, -1);
	lua_pop(L, 1);
	return v;
}

static inline bool getBool(lua_State *L, int idx, Key k)
{
	rget(L, idx, k);
	bool v = lua_toboolean(L, -1) != 0;
	lua_pop(L, 1);
	return v;
}

static inline void setNum(lua_State *L, int idx, Key k, double v)
{
	pushKey(L, k);
	lua_pushnumber(L, v);
	lua_rawset(L, idx);
}

static inline void setBool(lua_State *L, int idx, Key k, bool v)
{
	pushKey(L, k);
	lua_pushboolean(L, v ? 1 : 0);
	lua_rawset(L, idx);
}

static inline void setFromTop(lua_State *L, int idx, Key k)
{
	pushKey(L, k);
	lua_insert(L, -2);
	lua_rawset(L, idx);
}

static void pushVecField(lua_State *L, int b, Key k)
{
	rget(L, b, k);
	if (!lua_istable(L, -1))
		luaL_error(L, "physics: body.%s must be a vec3", KEY_NAMES[k]);
}

static inline void readVec3(lua_State *L, int idx, double *o)
{
	rget(L, idx, K_X);
	rget(L, idx, K_Y);
	rget(L, idx, K_Z);
	o[0] = lua_tonumber(L, -3);
	o[1] = lua_tonumber(L, -2);
	o[2] = lua_tonumber(L, -1);
	lua_pop(L, 3);
}

struct BodyState
{
	double p[3], s[3];
	double radius, stepDown, staticTime;
	int shape;
	bool onGround, wasOnGround, isStatic, canBeStatic;

	double bp[3], bs[3];
	double bStaticTime;
	bool bOnGround, bWasOnGround, bIsStatic;
};

static void loadHead(lua_State *L, int b, BodyState &st)
{
	st.isStatic = st.bIsStatic = getBool(L, b, K_STATIC);
	st.canBeStatic = getBool(L, b, K_CANBESTATIC);

	pushVecField(L, b, K_SPEED);
	readVec3(L, lua_gettop(L), st.s);
	lua_pop(L, 1);
	st.bs[0] = st.s[0]; st.bs[1] = st.s[1]; st.bs[2] = st.s[2];
}

static void loadRest(lua_State *L, int b, BodyState &st)
{
	pushVecField(L, b, K_POSITION);
	readVec3(L, lua_gettop(L), st.p);
	lua_pop(L, 1);
	st.bp[0] = st.p[0]; st.bp[1] = st.p[1]; st.bp[2] = st.p[2];

	st.radius = getNum(L, b, K_RADIUS);
	st.shape = (int)getNum(L, b, K_SHAPE);
	st.stepDown = getNum(L, b, K_STEPDOWN);
	st.onGround = st.bOnGround = getBool(L, b, K_ONGROUND);
	st.wasOnGround = st.bWasOnGround = getBool(L, b, K_WASONGROUND);
	st.staticTime = st.bStaticTime = getNum(L, b, K_STATICTIME);
}

static void loadBody(lua_State *L, int b, BodyState &st)
{
	loadHead(L, b, st);
	loadRest(L, b, st);
}

static void flush(lua_State *L, int b, BodyState &st)
{
	if (st.p[0] != st.bp[0] || st.p[1] != st.bp[1] || st.p[2] != st.bp[2])
	{
		pushVecField(L, b, K_POSITION);
		int t = lua_gettop(L);
		for (int k = 0; k < 3; k++)
		{
			if (st.p[k] != st.bp[k])
			{
				setNum(L, t, (Key)(K_X + k), st.p[k]);
				st.bp[k] = st.p[k];
			}
		}
		lua_pop(L, 1);
	}

	if (st.s[0] != st.bs[0] || st.s[1] != st.bs[1] || st.s[2] != st.bs[2])
	{
		pushVecField(L, b, K_SPEED);
		int t = lua_gettop(L);
		for (int k = 0; k < 3; k++)
		{
			if (st.s[k] != st.bs[k])
			{
				setNum(L, t, (Key)(K_X + k), st.s[k]);
				st.bs[k] = st.s[k];
			}
		}
		lua_pop(L, 1);
	}

	if (st.onGround != st.bOnGround)
	{
		setBool(L, b, K_ONGROUND, st.onGround);
		st.bOnGround = st.onGround;
	}
	if (st.wasOnGround != st.bWasOnGround)
	{
		setBool(L, b, K_WASONGROUND, st.wasOnGround);
		st.bWasOnGround = st.wasOnGround;
	}
	if (st.isStatic != st.bIsStatic)
	{
		setBool(L, b, K_STATIC, st.isStatic);
		st.bIsStatic = st.isStatic;
	}
	if (st.staticTime != st.bStaticTime)
	{
		setNum(L, b, K_STATICTIME, st.staticTime);
		st.bStaticTime = st.staticTime;
	}
}


static void reload(lua_State *L, int b, BodyState &st)
{
	pushVecField(L, b, K_POSITION);
	readVec3(L, lua_gettop(L), st.p);
	lua_pop(L, 1);
	st.bp[0] = st.p[0]; st.bp[1] = st.p[1]; st.bp[2] = st.p[2];

	pushVecField(L, b, K_SPEED);
	readVec3(L, lua_gettop(L), st.s);
	lua_pop(L, 1);
	st.bs[0] = st.s[0]; st.bs[1] = st.s[1]; st.bs[2] = st.s[2];

	st.isStatic = st.bIsStatic = getBool(L, b, K_STATIC);
	st.onGround = st.bOnGround = getBool(L, b, K_ONGROUND);
	st.wasOnGround = st.bWasOnGround = getBool(L, b, K_WASONGROUND);
	st.staticTime = st.bStaticTime = getNum(L, b, K_STATICTIME);
}

static bool collideQuery(const ShapeQuery &q, Hit &best, int &bestModel)
{
	bool found = false;
	size_t n = staticModels.size();

	for (size_t i = 0; i < n; i++)
	{
		const StaticRec &r = staticModels[i];

		if (!MGOLD_boxOverlap(r.bmin, r.bmax, q.qmin, q.qmax))
			continue;

		Hit h;
		if (MGOLD_queryCache(r.cache, q, h, false) && (!found || h.len < best.len))
		{
			best = h;
			bestModel = (int)i;
			found = true;
		}
	}

	return found;
}

static bool collideAll(int shape, const double *p, double radius, double mx, double my, double mz, Hit &best, int &bestModel)
{
	double px = p[0] + mx, py = p[1] + my, pz = p[2] + mz;

	ShapeQuery q;
	if (shape == SHAPE_CAPSULE)
		MGOLD_makeCapsuleQuery(q, V3{ px, py + CAPSULE_TOP, pz }, V3{ px, py - CAPSULE_BOTTOM, pz }, radius);
	else if (shape == SHAPE_SPHERE)
		MGOLD_makeSphereQuery(q, V3{ px, py, pz }, radius);
	else
		return false;

	return collideQuery(q, best, bestModel);
}

static void refreshBounds(StaticRec &r)
{
	for (int k = 0; k < 3; k++)
	{
		r.bmin[k] = r.cache->aabbMin[k];
		r.bmax[k] = r.cache->aabbMax[k];
	}
}

static void syncStatic(lua_State *L)
{
	for (size_t i = 0; i < staticModels.size(); i++)
	{
		StaticRec &r = staticModels[i];

		lua_rawgeti(L, LUA_REGISTRYINDEX, r.ref);
		int m = lua_gettop(L);

		rget(L, m, K_COLLISIONDIRTY);
		bool clean = lua_isboolean(L, -1) && !lua_toboolean(L, -1);
		lua_pop(L, 1);

		if (!clean)
		{
			MGOLD_refreshCollisionCache(L, m, r.cache);
			if (r.zoneSize > 0)
				MGOLD_buildZones(r.cache, r.zoneSize);
			refreshBounds(r);
		}

		lua_pop(L, 1);
	}
}

static void fireEvent(lua_State *L, int b, BodyState *st, Key k, bool hasArg, double a)
{
	bool flushed = false;

	for (int src = 0; src < 2; src++)
	{
		int top = lua_gettop(L);

		if (src == 0)
			gget(L, b, k);
		else
		{
			lua_rawgeti(L, LUA_REGISTRYINDEX, callbacksRef);
			rget(L, lua_gettop(L), k);
			lua_remove(L, -2);
		}

		if (!lua_isfunction(L, -1))
		{
			lua_settop(L, top);
			continue;
		}

		if (st && !flushed)
		{
			flush(L, b, *st);
			flushed = true;
		}

		lua_pushvalue(L, b);
		if (hasArg)
			lua_pushnumber(L, a);
		lua_call(L, hasArg ? 2 : 1, 0);
	}

	if (st && flushed)
		reload(L, b, *st);
}

static void tryCollisionCb(lua_State *L, int b, BodyState &st, bool &flushed, int src, Key k, int modelRef, const V3 &n, const V3 &pt)
{
	int top = lua_gettop(L);

	if (src == 0)
		gget(L, b, k);
	else if (src == 1)
	{
		lua_rawgeti(L, LUA_REGISTRYINDEX, callbacksRef);
		rget(L, lua_gettop(L), k);
		lua_remove(L, -2);
	}
	else
	{
		lua_rawgeti(L, LUA_REGISTRYINDEX, modelRef);
		if (!lua_istable(L, -1))
		{
			lua_settop(L, top);
			return;
		}
		gget(L, lua_gettop(L), k);
		lua_remove(L, -2);
	}

	if (!lua_isfunction(L, -1))
	{
		lua_settop(L, top);
		return;
	}

	if (!flushed)
	{
		flush(L, b, st);
		flushed = true;
	}

	if (src == 2)
	{
		lua_rawgeti(L, LUA_REGISTRYINDEX, modelRef);
		lua_pushvalue(L, b);
	}
	else
	{
		lua_pushvalue(L, b);
		lua_rawgeti(L, LUA_REGISTRYINDEX, modelRef);
	}
	lua_pushnumber(L, n.x);
	lua_pushnumber(L, n.y);
	lua_pushnumber(L, n.z);
	lua_pushnumber(L, pt.x);
	lua_pushnumber(L, pt.y);
	lua_pushnumber(L, pt.z);
	lua_call(L, 8, 0);
}

static void fireCollision(lua_State *L, int b, BodyState &st, int modelIndex, const V3 &n, const V3 &pt)
{
	int modelRef = staticModels[modelIndex].ref;
	bool flushed = false;

	tryCollisionCb(L, b, st, flushed, 0, K_ONCOLLIDE, modelRef, n, pt);
	tryCollisionCb(L, b, st, flushed, 1, K_ONCOLLIDE, modelRef, n, pt);
	tryCollisionCb(L, b, st, flushed, 2, K_ONCOLLIDE, modelRef, n, pt);

	Key kind = K_COUNT;
	if (n.y < -0.7)
		kind = K_ONCEILINGHIT;
	else if (n.y <= 0.7)
		kind = K_ONWALLHIT;

	if (kind != K_COUNT)
	{
		tryCollisionCb(L, b, st, flushed, 0, kind, modelRef, n, pt);
		tryCollisionCb(L, b, st, flushed, 1, kind, modelRef, n, pt);
	}

	if (flushed)
		reload(L, b, st);
}

static void slide(lua_State *L, int b, BodyState &st, double mx, double my, double mz, double *r)
{
	Hit h;
	int mi = -1;
	bool hit = collideAll(st.shape, st.p, st.radius, mx, my, mz, h, mi);

	st.p[0] += mx;
	st.p[1] += my;
	st.p[2] += mz;

	if (!hit)
	{
		r[0] = mx; r[1] = my; r[2] = mz;
		r[3] = r[4] = r[5] = 0;
		return;
	}

	double nx = h.n.x, ny = h.n.y, nz = h.n.z;
	double rx = mx, ry = my, rz = mz;
	bool ignoreSlopes = ny > GROUND_NY;

	double speedLength = std::sqrt(mx * mx + my * my + mz * mz);
	if (speedLength > 0)
	{
		double ix = mx / speedLength, iy = my / speedLength, iz = mz / speedLength;
		double d = nx * ix + ny * iy + nz * iz;

		if (ignoreSlopes)
			ry = 0;
		else
		{
			ry = (iy - ny * d) * speedLength;
			rx = (ix - nx * d) * speedLength;
			rz = (iz - nz * d) * speedLength;
		}
	}

	double pen = h.len - st.radius;
	st.p[1] -= ny * pen;
	if (!ignoreSlopes)
	{
		st.p[0] -= nx * pen;
		st.p[2] -= nz * pen;
	}

	fireCollision(L, b, st, mi, h.n, h.p);

	r[0] = rx; r[1] = ry; r[2] = rz;
	r[3] = nx; r[4] = ny; r[5] = nz;
}

static void doUnsetStatic(lua_State *L, int b)
{
	bool was = getBool(L, b, K_STATIC);
	setBool(L, b, K_STATIC, false);
	setNum(L, b, K_STATICTIME, 5);
	if (was)
		fireEvent(L, b, nullptr, K_ONWAKE, false, 0);
}

static void updateBody(lua_State *L, int b, double dt, double gravity, double maxFall)
{
	BodyState st;
	loadHead(L, b, st);

	if (st.isStatic && st.canBeStatic)
	{
		if (st.s[0] * st.s[0] + st.s[1] * st.s[1] + st.s[2] * st.s[2] > 0.0001)
			doUnsetStatic(L, b);
		return;
	}

	loadRest(L, b, st);

	double vy = st.s[1] - gravity * dt;
	if (vy < -maxFall)
		vy = -maxFall;
	double impactSpeed = -vy;

	double r[6];
	slide(L, b, st, 0, vy * dt, 0, r);
	vy = r[1] / dt;
	st.s[1] = vy;

	bool wasOnGround = st.onGround;
	st.wasOnGround = wasOnGround;
	st.onGround = r[4] > GROUND_NY;

	if (!st.onGround && wasOnGround && vy < 0)
	{
		Hit h;
		int mi = -1;
		if (collideAll(st.shape, st.p, st.radius, 0, -st.stepDown, 0, h, mi))
		{
			st.p[1] = st.p[1] - st.stepDown - h.n.y * (h.len - st.radius);
			st.s[1] = 0;
			st.onGround = true;
		}
	}

	if (st.onGround && !wasOnGround)
		fireEvent(L, b, &st, K_ONLAND, true, impactSpeed > 0 ? impactSpeed : 0);
	else if (!st.onGround && wasOnGround)
		fireEvent(L, b, &st, K_ONLEAVEGROUND, false, 0);

	double dx = st.s[0] * dt, dz = st.s[2] * dt;
	if (dx * dx + dz * dz > 1e-12)
	{
		slide(L, b, st, dx, 0, dz, r);
		st.s[0] = r[0] / dt;
		st.s[2] = r[2] / dt;
	}

	pushVecField(L, b, K_LASTSPEED);
	int t = lua_gettop(L);
	setNum(L, t, K_X, st.s[0]);
	setNum(L, t, K_Y, st.s[1]);
	setNum(L, t, K_Z, st.s[2]);
	lua_pop(L, 1);

	if (st.canBeStatic)
	{
		if (st.s[0] * st.s[0] + st.s[1] * st.s[1] + st.s[2] * st.s[2] <= 0.0001)
		{
			st.staticTime -= dt;
			if (st.staticTime <= 0)
			{
				st.isStatic = true;
				st.staticTime = 1;
				fireEvent(L, b, &st, K_ONSLEEP, false, 0);
			}
		}
	}

	flush(L, b, st);
}

static void createBody(lua_State *L, double x, double y, double z, double radius, int shape)
{
	lua_createtable(L, 0, 18);
	int b = lua_gettop(L);

	MGOLD_pushVec3(L, x, y, z);
	setFromTop(L, b, K_POSITION);
	MGOLD_pushVec3(L, 0, 0, 0);
	setFromTop(L, b, K_SPEED);
	MGOLD_pushVec3(L, 0, 0, 0);
	setFromTop(L, b, K_LASTSPEED);
	MGOLD_pushVec3(L, 0, 0, 0);
	setFromTop(L, b, K_NORMAL);

	setNum(L, b, K_SHAPE, shape);
	setNum(L, b, K_RADIUS, radius);
	setBool(L, b, K_ONGROUND, false);
	setBool(L, b, K_WASONGROUND, false);
	setNum(L, b, K_STEPDOWN, 0.075);

	setBool(L, b, K_STATIC, false);
	setNum(L, b, K_STATICTIME, 1);
	setBool(L, b, K_CANBESTATIC, true);
	setBool(L, b, K_MANUALUPDATE, false);

	lua_rawgeti(L, LUA_REGISTRYINDEX, bodyMetaRef);
	lua_setmetatable(L, b);

	lua_pushvalue(L, b);
	BodyRec rec = { luaL_ref(L, LUA_REGISTRYINDEX) };
	bodies.push_back(rec);
	setNum(L, b, K_BODYID, (double)bodies.size());
}

static void destroyAt(lua_State *L, size_t idx)
{
	size_t n = bodies.size();
	if (idx >= n)
	{
		return;

	}

	int ref = bodies[idx].ref;

	if (idx != n - 1)
	{
		BodyRec last = bodies[n - 1];
		bodies[idx] = last;

		lua_rawgeti(L, LUA_REGISTRYINDEX, last.ref);
		setNum(L, lua_gettop(L), K_BODYID, (double)(idx + 1));
		lua_pop(L, 1);
	}
	bodies.pop_back();

	lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
	int t = lua_gettop(L);
	pushKey(L, K_BODYID);
	lua_pushnil(L);
	lua_rawset(L, t);
	lua_pop(L, 1);

	luaL_unref(L, LUA_REGISTRYINDEX, ref);
}

static int w_newBody(lua_State *L)
{
	double x = luaL_optnumber(L, 1, 0);
	double y = luaL_optnumber(L, 2, 0);
	double z = luaL_optnumber(L, 3, 0);
	double radius = luaL_optnumber(L, 4, 0.2);
	int shape = (int)luaL_optnumber(L, 5, SHAPE_SPHERE);

	createBody(L, x, y, z, radius, shape);
	return 1;
}

static int w_destroy(lua_State *L)
{
	luaL_checktype(L, 1, LUA_TTABLE);

	rget(L, 1, K_BODYID);
	if (!lua_isnumber(L, -1))
	{
		return 0;

	}
	size_t idx = (size_t)lua_tonumber(L, -1) - 1;
	lua_pop(L, 1);

	if (idx >= bodies.size())
	{
		return 0;

	}
	lua_rawgeti(L, LUA_REGISTRYINDEX, bodies[idx].ref);
	bool same = lua_rawequal(L, -1, 1) != 0;
	lua_pop(L, 1);
	if (!same)
	{
		return 0;

	}

	destroyAt(L, idx);
	return 0;
}

static int w_getBodies(lua_State *L)
{
	lua_createtable(L, (int)bodies.size(), 0);
	for (size_t i = 0; i < bodies.size(); i++)
	{
		lua_rawgeti(L, LUA_REGISTRYINDEX, bodies[i].ref);
		lua_rawseti(L, -2, (int)i + 1);
	}
	return 1;
}

static int w_attachModel(lua_State *L)
{
	luaL_checktype(L, 1, LUA_TTABLE);
	int shape = (int)luaL_optnumber(L, 2, SHAPE_SPHERE);

	gget(L, 1, K_POSITION);
	double p[3];
	readVec3(L, lua_gettop(L), p);
	lua_pop(L, 1);

	gget(L, 1, K_SCALE);
	double s[3];
	readVec3(L, lua_gettop(L), s);
	lua_pop(L, 1);

	createBody(L, p[0], p[1], p[2], (s[0] + s[1] + s[2]) / 3.0, shape);
	int b = lua_gettop(L);

	pushKey(L, K_PHYSICS);
	lua_pushvalue(L, b);
	lua_settable(L, 1);

	lua_pushvalue(L, 1);
	setFromTop(L, b, K_MODEL);

	return 0;
}

static int w_addStaticModel(lua_State *L)
{
	luaL_checktype(L, 1, LUA_TTABLE);
	double zoneSize = luaL_optnumber(L, 2, 0);

	CollisionCache *cache = MGOLD_getCollisionCache(L, 1);
	if (zoneSize > 0)
	{
		MGOLD_buildZones(cache, zoneSize);

	}

	lua_pushvalue(L, 1);
	StaticRec rec;
	rec.cache = cache;
	rec.ref = luaL_ref(L, LUA_REGISTRYINDEX);
	rec.zoneSize = zoneSize > 0 ? zoneSize : 0;
	refreshBounds(rec);
	staticModels.push_back(rec);

	setNum(L, 1, K_PHYSICSID, (double)staticModels.size());

	lua_pushvalue(L, 1);
	return 1;
}

static int w_removeStaticModel(lua_State *L)
{
	luaL_checktype(L, 1, LUA_TTABLE);

	rget(L, 1, K_PHYSICSID);
	if (!lua_isnumber(L, -1))
	{
		return 0;

	}
	double id = lua_tonumber(L, -1);
	lua_pop(L, 1);

	if (id < 1 || (size_t)id > staticModels.size())
	{
		return 0;

	}
	size_t idx = (size_t)id - 1;

	lua_rawgeti(L, LUA_REGISTRYINDEX, staticModels[idx].ref);
	bool same = lua_rawequal(L, -1, 1) != 0;
	lua_pop(L, 1);
	if (!same)
	{
		return 0;

	}

	int ref = staticModels[idx].ref;
	size_t n = staticModels.size();

	if (idx != n - 1)
	{
		StaticRec last = staticModels[n - 1];
		staticModels[idx] = last;

		lua_rawgeti(L, LUA_REGISTRYINDEX, last.ref);
		setNum(L, lua_gettop(L), K_PHYSICSID, (double)(idx + 1));
		lua_pop(L, 1);
	}
	staticModels.pop_back();

	pushKey(L, K_PHYSICSID);
	lua_pushnil(L);
	lua_rawset(L, 1);

	luaL_unref(L, LUA_REGISTRYINDEX, ref);
	return 0;
}

static int w_getStaticModels(lua_State *L)
{
	lua_createtable(L, (int)staticModels.size(), 0);
	for (size_t i = 0; i < staticModels.size(); i++)
	{
		lua_rawgeti(L, LUA_REGISTRYINDEX, staticModels[i].ref);
		lua_rawseti(L, -2, (int)i + 1);
	}
	return 1;
}

static int w_syncStaticModels(lua_State *L)
{
	syncStatic(L);
	return 0;
}

static int pushVec3Pair(lua_State *L, double a, double b, double c, double d, double e, double f)
{
	MGOLD_pushVec3(L, a, b, c);
	MGOLD_pushVec3(L, d, e, f);
	return 2;
}

static int w_collisionTest(lua_State *L)
{
	luaL_checktype(L, 1, LUA_TTABLE);
	luaL_checktype(L, 2, LUA_TTABLE);

	syncStatic(L);

	BodyState st;
	loadBody(L, 1, st);

	double m[3];
	readVec3(L, 2, m);

	Hit h;
	int mi = -1;
	if (!collideAll(st.shape, st.p, st.radius, m[0], m[1], m[2], h, mi))
	{
		lua_pushnil(L);
		return 1 + pushVec3Pair(L, 0, 0, 0, 0, 0, 0);
	}

	lua_pushnumber(L, h.len);
	pushVec3Pair(L, h.p.x, h.p.y, h.p.z, h.n.x, h.n.y, h.n.z);
	lua_rawgeti(L, LUA_REGISTRYINDEX, staticModels[mi].ref);
	return 4;
}

static int w_moveAndSlide(lua_State *L)
{
	luaL_checktype(L, 1, LUA_TTABLE);
	luaL_checktype(L, 2, LUA_TTABLE);

	syncStatic(L);

	BodyState st;
	loadBody(L, 1, st);

	double m[3];
	readVec3(L, 2, m);

	double r[6];
	slide(L, 1, st, m[0], m[1], m[2], r);
	flush(L, 1, st);

	return pushVec3Pair(L, r[0], r[1], r[2], r[3], r[4], r[5]);
}

static int w_updatePhysics(lua_State *L)
{
	luaL_checktype(L, 1, LUA_TTABLE);
	double dt = luaL_checknumber(L, 2);
	double gravity = luaL_optnumber(L, 3, 1);
	double maxFall = luaL_optnumber(L, 4, 19);

	syncStatic(L);
	updateBody(L, 1, dt, gravity, maxFall);
	return 0;
}

static int w_setStatic(lua_State *L)
{
	luaL_checktype(L, 1, LUA_TTABLE);

	bool was = getBool(L, 1, K_STATIC);
	setBool(L, 1, K_STATIC, true);

	pushVecField(L, 1, K_SPEED);
	int t = lua_gettop(L);
	setNum(L, t, K_X, 0);
	setNum(L, t, K_Y, 0);
	setNum(L, t, K_Z, 0);
	lua_pop(L, 1);

	if (!was)
	{
		fireEvent(L, 1, nullptr, K_ONSLEEP, false, 0);

	}
	return 0;
}

static int w_unsetStatic(lua_State *L)
{
	luaL_checktype(L, 1, LUA_TTABLE);
	doUnsetStatic(L, 1);
	return 0;
}


static int w_updateAll(lua_State *L)
{
	double dt = luaL_checknumber(L, 1);

	syncStatic(L);

	std::vector<int> toDel;

	for (size_t i = bodies.size(); i-- > 0;)
	{
		if (i >= bodies.size())
		{
			continue;

		}

		int top = lua_gettop(L);
		int ref = bodies[i].ref;

		lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
		int b = lua_gettop(L);

		if (!getBool(L, b, K_MANUALUPDATE))
		{
			updateBody(L, b, dt, 20, 19);

		}

		rget(L, b, K_MODEL);
		if (lua_istable(L, -1))
		{
			int m = lua_gettop(L);

			gget(L, m, K_ALIVE);
			bool alive = lua_toboolean(L, -1) != 0;
			lua_pop(L, 1);

			if (alive)
			{
				pushVecField(L, b, K_POSITION);
				double bp[3];
				readVec3(L, lua_gettop(L), bp);
				lua_pop(L, 1);

				gget(L, m, K_POSITION);
				double mp[3] = { 0, 0, 0 };
				if (lua_istable(L, -1))
				{
					readVec3(L, lua_gettop(L), mp);

				}
				lua_pop(L, 1);

				if (mp[0] != bp[0] || mp[1] != bp[1] || mp[2] != bp[2])
				{
					gget(L, m, K_SETPOSITION);
					if (lua_isfunction(L, -1))
					{
						lua_pushvalue(L, m);
						lua_pushnumber(L, bp[0]);
						lua_pushnumber(L, bp[1]);
						lua_pushnumber(L, bp[2]);
						lua_call(L, 4, 0);
					}
					else
					{
						lua_pop(L, 1);

					}
				}
			}
			else
			{
				toDel.push_back(ref);

			}
		}

		lua_settop(L, top);
	}

	for (size_t d = 0; d < toDel.size(); d++)
	{
		for (size_t i = 0; i < bodies.size(); i++)
		{
			if (bodies[i].ref == toDel[d])
			{
				destroyAt(L, i);
				break;
			}
		}
	}

	return 0;
}
static int pushResult(lua_State *L, const Hit &h, int mi)
{
	lua_pushnumber(L, h.len);
	lua_pushnumber(L, h.p.x);
	lua_pushnumber(L, h.p.y);
	lua_pushnumber(L, h.p.z);
	lua_pushnumber(L, h.n.x);
	lua_pushnumber(L, h.n.y);
	lua_pushnumber(L, h.n.z);
	lua_rawgeti(L, LUA_REGISTRYINDEX, staticModels[mi].ref);
	return 8;
}

static int w_raycast(lua_State *L)
{
	V3 src = V3{ luaL_checknumber(L, 1), luaL_checknumber(L, 2), luaL_checknumber(L, 3) };
	V3 dir = V3{ luaL_checknumber(L, 4), luaL_checknumber(L, 5), luaL_checknumber(L, 6) };
	double maxDist = luaL_optnumber(L, 7, -1);

	double len = length(dir);
	if (len == 0)
	{
		return luaL_argerror(L, 4, "direction must not be zero");

	}
	dir = dir * (1.0 / len);

	syncStatic(L);

	bool found = false;
	Hit best = {};
	int bestModel = -1;

	for (size_t i = 0; i < staticModels.size(); i++)
	{
		Hit h;
		if (MGOLD_cacheRay(staticModels[i].cache, src, dir, h) && (!found || h.len < best.len))
		{
			best = h;
			bestModel = (int)i;
			found = true;
		}
	}

	if (!found || (maxDist >= 0 && best.len > maxDist))
	{
		return 0;

	}

	return pushResult(L, best, bestModel);
}

static int w_sphereTest(lua_State *L)
{
	V3 c = V3{ luaL_checknumber(L, 1), luaL_checknumber(L, 2), luaL_checknumber(L, 3) };
	double radius = luaL_checknumber(L, 4);

	syncStatic(L);

	ShapeQuery q;
	MGOLD_makeSphereQuery(q, c, radius);

	Hit h;
	int mi = -1;
	return collideQuery(q, h, mi) ? pushResult(L, h, mi) : 0;
}

static int w_capsuleTest(lua_State *L)
{
	V3 tip = V3{ luaL_checknumber(L, 1), luaL_checknumber(L, 2), luaL_checknumber(L, 3) };
	V3 base = V3{ luaL_checknumber(L, 4), luaL_checknumber(L, 5), luaL_checknumber(L, 6) };
	double radius = luaL_checknumber(L, 7);

	syncStatic(L);

	ShapeQuery q;
	MGOLD_makeCapsuleQuery(q, tip, base, radius);

	Hit h;
	int mi = -1;
	return collideQuery(q, h, mi) ? pushResult(L, h, mi) : 0;
}

static int w_closestPoint(lua_State *L)
{
	V3 p = V3{ luaL_checknumber(L, 1), luaL_checknumber(L, 2), luaL_checknumber(L, 3) };

	syncStatic(L);

	bool found = false;
	Hit best = {};
	int bestModel = -1;

	for (size_t i = 0; i < staticModels.size(); i++)
	{
		Hit h;
		if (MGOLD_cachePoint(staticModels[i].cache, p, h) && (!found || h.len < best.len))
		{
			best = h;
			bestModel = (int)i;
			found = true;
		}
	}

	return found ? pushResult(L, best, bestModel) : 0;
}

void MGOLD_setPhysicsFunc(lua_State *L)
{
	staticModels.clear();
	bodies.clear();

	for (int i = 0; i < K_COUNT; i++)
	{
		lua_pushstring(L, KEY_NAMES[i]);
		keyRef[i] = luaL_ref(L, LUA_REGISTRYINDEX);
	}

	lua_newtable(L);
	int module = lua_gettop(L);

	lua_newtable(L);
	lua_pushnumber(L, SHAPE_BOX);
	lua_setfield(L, -2, "box");
	lua_pushnumber(L, SHAPE_SPHERE);
	lua_setfield(L, -2, "sphere");
	lua_pushnumber(L, SHAPE_CAPSULE);
	lua_setfield(L, -2, "capsule");
	lua_pushnumber(L, SHAPE_PLANE);
	lua_setfield(L, -2, "plane");
	lua_setfield(L, module, "shapes");

	lua_newtable(L);
	lua_pushvalue(L, -1);
	callbacksRef = luaL_ref(L, LUA_REGISTRYINDEX);
	lua_setfield(L, module, "callbacks");

	lua_newtable(L);
	int cls = lua_gettop(L);
	lua_pushvalue(L, cls);
	lua_setfield(L, cls, "__index");

	struct Entry { const char *name; lua_CFunction fn; };
	static const Entry bodyFuncs[] = {
		{ "destroy", w_destroy },
		{ "collisionTest", w_collisionTest },
		{ "moveAndSlide", w_moveAndSlide },
		{ "updatePhysics", w_updatePhysics },
		{ "setStatic", w_setStatic },
		{ "unsetStatic", w_unsetStatic },
	};
	for (const Entry &e : bodyFuncs)
	{
		lua_pushcfunction(L, e.fn);
		lua_setfield(L, cls, e.name);
	}

	lua_pushvalue(L, cls);
	bodyMetaRef = luaL_ref(L, LUA_REGISTRYINDEX);
	lua_setfield(L, module, "Body");

	static const Entry moduleFuncs[] = {
		{ "addStaticModel", w_addStaticModel },
		{ "removeStaticModel", w_removeStaticModel },
		{ "getStaticModels", w_getStaticModels },
		{ "syncStaticModels", w_syncStaticModels },
		{ "newBody", w_newBody },
		{ "getBodies", w_getBodies },
		{ "attachModel", w_attachModel },
		{ "updateAll", w_updateAll },
		{ "raycast", w_raycast },
		{ "sphereTest", w_sphereTest },
		{ "capsuleTest", w_capsuleTest },
		{ "closestPoint", w_closestPoint },
	};
	for (const Entry &e : moduleFuncs)
	{
		lua_pushcfunction(L, e.fn);
		lua_setfield(L, module, e.name);
	}
}

}
}
