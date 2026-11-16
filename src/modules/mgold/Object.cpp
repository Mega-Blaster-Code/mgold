#include "modules/mgold/Object.h"
#include "modules/mgold/Light.h"

#include "common/runtime.h"
#include "common/Module.h"
#include "modules/graphics/Graphics.h"
#include "modules/graphics/Mesh.h"
#include "modules/graphics/wrap_Texture.h"
#include "modules/graphics/wrap_Mesh.h"

#include <cmath>
#include <cstring>
#include <initializer_list>
#include <new>
#include <climits>
#include <vector>



















namespace love
{
namespace mgold
{

namespace
{

const int FLOATS_PER_INSTANCE = 20;
const int BYTES_PER_INSTANCE = FLOATS_PER_INSTANCE * (int)sizeof(float);

struct MeshData
{
	int count, maxModels, visibleCount, dirtyCount;
	bool dirty;
	float *ptr;
	StrongRef<graphics::Mesh> mesh;
	StrongRef<graphics::Mesh> inst;
	int modelsRef, dirtyRef;
};

int meshMT = LUA_NOREF;
int modelMT = LUA_NOREF;
int dataMT = LUA_NOREF;
int meshsRef = LUA_NOREF;

const char *INST_NAMES[5] = { "InstanceMat1", "InstanceMat2", "InstanceMat3", "InstanceMat4", "InstanceColor" };

std::vector<graphics::Buffer::DataDeclaration> mainFormat;
std::vector<graphics::Buffer::DataDeclaration> instFormat;

inline graphics::Graphics *gfx()
{
	return Module::getInstance<graphics::Graphics>(Module::M_GRAPHICS);
}






enum Key
{
	K_position, K_rotation, K_scale, K_matrix, K_color,
	K_x, K_y, K_z, K_r, K_g, K_b, K_a,
	K_alive, K_id, K_visible, K_queued, K_dirty, K_mesh, K__d,
	K_lightDirty, K_rsDirty, K_collisionDirty,
	K_COUNT
};

const char *KEY_NAMES[K_COUNT] = {
	"position", "rotation", "scale", "matrix", "color",
	"x", "y", "z", "r", "g", "b", "a",
	"alive", "id", "visible", "queued", "dirty", "mesh", "_d",
	"lightDirty", "rsDirty", "collisionDirty"
};

int keyRef[K_COUNT];

inline void pushK(lua_State *L, Key k) { lua_rawgeti(L, LUA_REGISTRYINDEX, keyRef[k]); }


inline void rgetK(lua_State *L, int t, Key k) { pushK(L, k); lua_rawget(L, t); }

inline double numK(lua_State *L, int t, Key k)
{
	rgetK(L, t, k);
	double v = lua_tonumber(L, -1);
	lua_pop(L, 1);
	return v;
}

inline bool boolK(lua_State *L, int t, Key k)
{
	rgetK(L, t, k);
	bool v = lua_toboolean(L, -1) != 0;
	lua_pop(L, 1);
	return v;
}

inline int intK(lua_State *L, int t, Key k)
{
	rgetK(L, t, k);
	int v = (int)lua_tointeger(L, -1);
	lua_pop(L, 1);
	return v;
}

inline void setNumK(lua_State *L, int t, Key k, double v)
{
	pushK(L, k);
	lua_pushnumber(L, v);
	lua_rawset(L, t);
}

inline void setBoolK(lua_State *L, int t, Key k, bool v)
{
	pushK(L, k);
	lua_pushboolean(L, v ? 1 : 0);
	lua_rawset(L, t);
}





inline int absidx(lua_State *L, int i)
{
	return (i < 0) ? lua_gettop(L) + i + 1 : i;
}

void pushPath(lua_State *L, std::initializer_list<const char *> path)
{
	lua_getglobal(L, "love");
	for (const char *p : path)
	{
		lua_getfield(L, -1, p);
		lua_remove(L, -2);
	}
}

double num(lua_State *L, int t, const char *k)
{
	lua_getfield(L, t, k);
	double v = lua_tonumber(L, -1);
	lua_pop(L, 1);
	return v;
}

int intField(lua_State *L, int t, const char *k)
{
	lua_getfield(L, t, k);
	int v = (int)lua_tointeger(L, -1);
	lua_pop(L, 1);
	return v;
}

bool boolField(lua_State *L, int t, const char *k)
{
	lua_getfield(L, t, k);
	bool v = lua_toboolean(L, -1) != 0;
	lua_pop(L, 1);
	return v;
}

void setNum(lua_State *L, int t, const char *k, double v)
{
	lua_pushnumber(L, v);
	lua_setfield(L, t, k);
}

void setInt(lua_State *L, int t, const char *k, int v)
{
	lua_pushinteger(L, v);
	lua_setfield(L, t, k);
}

void setBool(lua_State *L, int t, const char *k, bool v)
{
	lua_pushboolean(L, v ? 1 : 0);
	lua_setfield(L, t, k);
}

MeshData *meshData(lua_State *L, int mesh)
{
	rgetK(L, mesh, K__d);
	MeshData *d = (MeshData *)lua_touserdata(L, -1);
	lua_pop(L, 1);
	return d;
}

void syncCounts(lua_State *L, int mesh, MeshData *d)
{
	setInt(L, mesh, "count", d->count);
	setInt(L, mesh, "visibleCount", d->visibleCount);
}

void release(lua_State *L, MeshData *d)
{
	int *refs[2] = { &d->modelsRef, &d->dirtyRef };
	for (int i = 0; i < 2; i++)
	{
		if (*refs[i] != LUA_NOREF)
		{
			luaL_unref(L, LUA_REGISTRYINDEX, *refs[i]);
			*refs[i] = LUA_NOREF;
		}
	}
	d->mesh.set(nullptr);
	d->inst.set(nullptr);
	d->ptr = nullptr;
}

void pushCopy(lua_State *L, int vec)
{
	lua_getfield(L, vec, "copy");
	lua_pushvalue(L, vec);
	lua_call(L, 1, 1);
}







void allocInstance(lua_State *L, MeshData *d, int newMax)
{
	graphics::Mesh *inst = nullptr;
	luax_catchexcept(L, [&]() {
		inst = gfx()->newMesh(instFormat, newMax, graphics::PRIMITIVE_TRIANGLES, graphics::BUFFERDATAUSAGE_DYNAMIC);
		if (inst->getVertexStride() != (size_t)BYTES_PER_INSTANCE)
		{
			inst->release();
			throw love::Exception("mgold: unexpected instance vertex stride");
		}
	});

	float *p = (float *)inst->getVertexData();
	if (d->ptr)
		std::memcpy(p, d->ptr, (size_t)d->maxModels * BYTES_PER_INSTANCE);

	d->inst.set(inst, Acquire::NORETAIN);
	d->ptr = p;

	luax_catchexcept(L, [&]() {
		for (int i = 0; i < 5; i++)
			d->mesh->attachAttribute(INST_NAMES[i], d->inst->getVertexBuffer(), d->inst.get(), INST_NAMES[i], 0,
									 graphics::STEP_PER_INSTANCE);
	});

	d->maxModels = newMax;

	d->inst->setVertexDataModified(0, (size_t)newMax * BYTES_PER_INSTANCE);
}

void grow(lua_State *L, MeshData *d)
{
	if (d->count <= d->maxModels)
		return;

	int newMax = d->maxModels;
	while (newMax < d->count)
		newMax *= 2;

	allocInstance(L, d, newMax);
	d->dirty = true;
}





void touch(lua_State *L, int m)
{
	setBoolK(L, m, K_dirty, true);

	rgetK(L, m, K_mesh);
	int mesh = lua_gettop(L);
	MeshData *d = meshData(L, mesh);
	d->dirty = true;

	if (!boolK(L, m, K_queued))
	{
		setBoolK(L, m, K_queued, true);
		d->dirtyCount++;
		lua_rawgeti(L, LUA_REGISTRYINDEX, d->dirtyRef);
		lua_pushvalue(L, m);
		lua_rawseti(L, -2, d->dirtyCount);
		lua_pop(L, 1);
	}
	lua_pop(L, 1);
}



void computeMatrix(lua_State *L, int m, float *out)
{
	double t[3], r[3], s[3];

	rgetK(L, m, K_position);
	int v = lua_gettop(L);
	t[0] = numK(L, v, K_x); t[1] = numK(L, v, K_y); t[2] = numK(L, v, K_z);
	lua_pop(L, 1);

	rgetK(L, m, K_rotation);
	v = lua_gettop(L);
	r[0] = numK(L, v, K_x); r[1] = numK(L, v, K_y); r[2] = numK(L, v, K_z);
	lua_pop(L, 1);

	rgetK(L, m, K_scale);
	v = lua_gettop(L);
	s[0] = numK(L, v, K_x); s[1] = numK(L, v, K_y); s[2] = numK(L, v, K_z);
	lua_pop(L, 1);

	double ca = std::cos(r[2]), cb = std::cos(r[1]), cc = std::cos(r[0]);
	double sa = std::sin(r[2]), sb = std::sin(r[1]), sc = std::sin(r[0]);

	double M[16] = {
		ca * cb * s[0], (ca * sb * sc - sa * cc) * s[1], (ca * sb * cc + sa * sc) * s[2], t[0],
		sa * cb * s[0], (sa * sb * sc + ca * cc) * s[1], (sa * sb * cc - ca * sc) * s[2], t[1],
		-sb * s[0],     cb * sc * s[1],                  cb * cc * s[2],                  t[2],
		0, 0, 0, 1
	};


	rgetK(L, m, K_matrix);
	for (int i = 0; i < 16; i++)
	{
		lua_pushnumber(L, M[i]);
		lua_rawseti(L, -2, i + 1);
	}
	lua_pop(L, 1);

	for (int i = 0; i < 16; i++)
		out[i] = (float)M[i];
}

void swapModels(lua_State *L, int mesh, MeshData *d, int a, int b)
{
	if (a == b)
		return;

	lua_rawgeti(L, LUA_REGISTRYINDEX, d->modelsRef);
	int models = lua_gettop(L);
	lua_rawgeti(L, models, a);
	int ma = lua_gettop(L);
	lua_rawgeti(L, models, b);
	int mb = lua_gettop(L);

	lua_pushvalue(L, mb); lua_rawseti(L, models, a);
	lua_pushvalue(L, ma); lua_rawseti(L, models, b);

	pushK(L, K_id); lua_pushinteger(L, b); lua_rawset(L, ma);
	pushK(L, K_id); lua_pushinteger(L, a); lua_rawset(L, mb);
	lua_pushfstring(L, "%d%p", b, lua_topointer(L, mesh)); lua_setfield(L, ma, "uid");
	lua_pushfstring(L, "%d%p", a, lua_topointer(L, mesh)); lua_setfield(L, mb, "uid");

	touch(L, ma);
	touch(L, mb);

	lua_pop(L, 3);
}

void swapAndPop(lua_State *L, int mesh, MeshData *d, int model)
{
	int id = intK(L, model, K_id);

	if (boolK(L, model, K_visible))
	{
		swapModels(L, mesh, d, id, d->visibleCount);
		d->visibleCount--;
		id = d->visibleCount + 1;
	}

	swapModels(L, mesh, d, id, d->count);

	lua_rawgeti(L, LUA_REGISTRYINDEX, d->modelsRef);
	lua_pushnil(L);
	lua_rawseti(L, -2, d->count);
	lua_pop(L, 1);

	d->count--;
	d->dirty = true;
	syncCounts(L, mesh, d);
}

void destroyModel(lua_State *L, int model)
{
	if (!boolK(L, model, K_alive))
		return;
	setBoolK(L, model, K_alive, false);

	rgetK(L, model, K_mesh);
	int mesh = lua_gettop(L);
	swapAndPop(L, mesh, meshData(L, mesh), model);
	lua_pop(L, 1);
}



int w_model_setVisibility(lua_State *L)
{
	luaL_checktype(L, 1, LUA_TTABLE);
	bool flag = lua_toboolean(L, 2) != 0;

	if (!boolK(L, 1, K_alive) || boolK(L, 1, K_visible) == flag)
		return 0;

	rgetK(L, 1, K_mesh);
	int mesh = lua_gettop(L);
	MeshData *d = meshData(L, mesh);
	int id = intK(L, 1, K_id);

	setBoolK(L, 1, K_visible, flag);

	if (flag)
	{
		swapModels(L, mesh, d, id, d->visibleCount + 1);
		d->visibleCount++;
	}
	else
	{
		swapModels(L, mesh, d, id, d->visibleCount);
		d->visibleCount--;
	}

	d->dirty = true;
	syncCounts(L, mesh, d);
	touch(L, 1);
	lua_pop(L, 1);
	return 0;
}

int w_model_setColor(lua_State *L)
{
	double r = luaL_optnumber(L, 2, 0), g = luaL_optnumber(L, 3, 0);
	double b = luaL_optnumber(L, 4, 0), a = luaL_optnumber(L, 5, 1);

	rgetK(L, 1, K_color);
	int c = lua_gettop(L);
	setNumK(L, c, K_r, r);
	setNumK(L, c, K_g, g);
	setNumK(L, c, K_b, b);
	setNumK(L, c, K_a, a);
	lua_pop(L, 1);
	touch(L, 1);
	return 0;
}

int w_model_setPosition(lua_State *L)
{
	double x = luaL_checknumber(L, 2), y = luaL_checknumber(L, 3), z = luaL_checknumber(L, 4);

	rgetK(L, 1, K_position);
	int p = lua_gettop(L);
	setNumK(L, p, K_x, x);
	setNumK(L, p, K_y, y);
	setNumK(L, p, K_z, z);
	lua_pop(L, 1);

	setBoolK(L, 1, K_lightDirty, true);
	setBoolK(L, 1, K_collisionDirty, true);
	touch(L, 1);
	return 0;
}

int w_model_setRotation(lua_State *L)
{
	double x = luaL_optnumber(L, 2, 0), y = luaL_optnumber(L, 3, 0), z = luaL_optnumber(L, 4, 0);

	rgetK(L, 1, K_rotation);
	int r = lua_gettop(L);
	setNumK(L, r, K_x, x);
	setNumK(L, r, K_y, y);
	setNumK(L, r, K_z, z);
	lua_pop(L, 1);

	setBoolK(L, 1, K_rsDirty, true);
	setBoolK(L, 1, K_collisionDirty, true);
	touch(L, 1);
	return 0;
}

int w_model_setScale(lua_State *L)
{
	double x = luaL_checknumber(L, 2);
	double y = luaL_optnumber(L, 3, x);
	double z = luaL_optnumber(L, 4, y);

	rgetK(L, 1, K_scale);
	int s = lua_gettop(L);
	setNumK(L, s, K_x, x);
	setNumK(L, s, K_y, y);
	setNumK(L, s, K_z, z);
	lua_pop(L, 1);

	setBoolK(L, 1, K_rsDirty, true);
	setBoolK(L, 1, K_collisionDirty, true);
	touch(L, 1);
	return 0;
}

double indexNum(lua_State *L, int t, int i)
{
	lua_pushinteger(L, i);
	lua_gettable(L, t);
	double v = lua_tonumber(L, -1);
	lua_pop(L, 1);
	return v;
}

int w_model_lookAt(lua_State *L)
{
	luaL_checktype(L, 2, LUA_TTABLE);

	rgetK(L, 1, K_position);
	int p = lua_gettop(L);
	double px = numK(L, p, K_x), py = numK(L, p, K_y), pz = numK(L, p, K_z);
	lua_pop(L, 1);

	double dx = indexNum(L, 2, 1) - px;
	double dy = -indexNum(L, 2, 2) - py;
	double dz = indexNum(L, 2, 3) - pz;

	double yaw = std::atan2(dx, dz);
	double pitch = std::atan2(dy, std::sqrt(dx * dx + dz * dz));

	rgetK(L, 1, K_rotation);
	int r = lua_gettop(L);
	setNumK(L, r, K_x, pitch);
	setNumK(L, r, K_y, yaw);
	setNumK(L, r, K_z, 0);
	lua_pop(L, 1);

	setBoolK(L, 1, K_rsDirty, true);
	setBoolK(L, 1, K_collisionDirty, true);
	touch(L, 1);
	return 0;
}

int w_model_updateMatrix(lua_State *L)
{
	float tmp[16];
	computeMatrix(L, 1, tmp);
	return 0;
}

int w_model_destroy(lua_State *L)
{
	destroyModel(L, 1);
	return 0;
}





void pushVecFn(lua_State *L, const char *sub, const char *fn)
{
	pushPath(L, { "mgold", sub, fn });
}

int w_mesh_newModel(lua_State *L)
{
	luaL_checktype(L, 1, LUA_TTABLE);
	luaL_checktype(L, 2, LUA_TTABLE);
	luaL_checktype(L, 3, LUA_TTABLE);

	MeshData *d = meshData(L, 1);
	d->count++;
	grow(L, d);

	lua_newtable(L);
	int m = lua_gettop(L);
	lua_rawgeti(L, LUA_REGISTRYINDEX, modelMT);
	lua_setmetatable(L, m);

	lua_pushfstring(L, "%d%p", d->count, lua_topointer(L, 1));
	lua_setfield(L, m, "uid");
	setInt(L, m, "id", d->count);
	lua_pushvalue(L, 1); lua_setfield(L, m, "mesh");
	setBool(L, m, "alive", true);
	setBool(L, m, "queued", false);
	setBool(L, m, "visible", true);
	setInt(L, m, "physicsId", 0);

	pushVecFn(L, "vector", "rgba");
	lua_pushnumber(L, 1); lua_pushnumber(L, 1); lua_pushnumber(L, 1); lua_pushnumber(L, 1);
	lua_call(L, 4, 1);
	lua_setfield(L, m, "color");

	pushCopy(L, 2); lua_setfield(L, m, "position");
	pushCopy(L, 3); lua_setfield(L, m, "rotation");

	if (lua_type(L, 4) == LUA_TNUMBER)
	{
		pushVecFn(L, "vector", "vec3");
		lua_pushvalue(L, 4);
		lua_call(L, 1, 1);
	}
	else
	{
		luaL_checktype(L, 4, LUA_TTABLE);
		pushCopy(L, 4);
	}
	lua_setfield(L, m, "scale");

	pushVecFn(L, "matrices", "newMatrix");
	lua_call(L, 0, 1);
	lua_setfield(L, m, "matrix");

	setBool(L, m, "rsDirty", true);
	setBool(L, m, "collisionDirty", true);

	lua_rawgeti(L, LUA_REGISTRYINDEX, d->modelsRef);
	lua_pushvalue(L, m);
	lua_rawseti(L, -2, d->count);
	lua_pop(L, 1);


	if (d->visibleCount < d->count - 1)
		swapModels(L, 1, d, d->count, d->visibleCount + 1);
	d->visibleCount++;
	syncCounts(L, 1, d);

	touch(L, m);

	lua_pushvalue(L, m);
	return 1;
}

int w_mesh_grow(lua_State *L)
{
	grow(L, meshData(L, 1));
	return 0;
}

int w_mesh_swapModels(lua_State *L)
{
	swapModels(L, 1, meshData(L, 1), (int)luaL_checkinteger(L, 2), (int)luaL_checkinteger(L, 3));
	return 0;
}

int w_mesh_swapAndPop(lua_State *L)
{
	luaL_checktype(L, 2, LUA_TTABLE);
	swapAndPop(L, 1, meshData(L, 1), 2);
	return 0;
}

void updateMesh(lua_State *L, int mesh)
{
	MeshData *d = meshData(L, mesh);
	if (!d || !d->dirty || !d->ptr)
		return;

	lua_rawgeti(L, LUA_REGISTRYINDEX, d->dirtyRef);
	int list = lua_gettop(L);


	int lo = INT_MAX, hi = 0;

	for (int i = 1; i <= d->dirtyCount; i++)
	{
		lua_rawgeti(L, list, i);
		int m = lua_gettop(L);
		lua_pushnil(L);
		lua_rawseti(L, list, i);

		setBoolK(L, m, K_queued, false);

		if (boolK(L, m, K_alive))
		{
			int id = intK(L, m, K_id);
			float *o = d->ptr + (size_t)(id - 1) * FLOATS_PER_INSTANCE;

			computeMatrix(L, m, o);
			setBoolK(L, m, K_dirty, false);

			rgetK(L, m, K_color);
			int c = lua_gettop(L);
			o[16] = (float)numK(L, c, K_r);
			o[17] = (float)numK(L, c, K_g);
			o[18] = (float)numK(L, c, K_b);
			o[19] = (float)numK(L, c, K_a);
			lua_pop(L, 1);

			if (id < lo) lo = id;
			if (id > hi) hi = id;
		}
		lua_pop(L, 1);
	}
	lua_pop(L, 1);
	d->dirtyCount = 0;


	if (hi >= lo)
		d->inst->setVertexDataModified((size_t)(lo - 1) * BYTES_PER_INSTANCE, (size_t)(hi - lo + 1) * BYTES_PER_INSTANCE);

	d->dirty = false;
}

int w_mesh_update(lua_State *L)
{
	updateMesh(L, 1);
	return 0;
}

int w_mesh_destroy(lua_State *L)
{
	MeshData *d = meshData(L, 1);

	lua_rawgeti(L, LUA_REGISTRYINDEX, d->modelsRef);
	int models = lua_gettop(L);
	for (int i = d->count; i >= 1; i--)
	{
		lua_rawgeti(L, models, i);
		destroyModel(L, lua_gettop(L));
		lua_pop(L, 1);
	}
	lua_pop(L, 1);

	lua_getfield(L, 1, "name");
	lua_rawgeti(L, LUA_REGISTRYINDEX, meshsRef);
	lua_pushvalue(L, -2);
	lua_pushnil(L);
	lua_settable(L, -3);
	lua_pop(L, 2);

	release(L, d);
	return 0;
}

int w_dataGC(lua_State *L)
{
	MeshData *d = (MeshData *)lua_touserdata(L, 1);
	if (d)
	{
		release(L, d);
		d->~MeshData();
	}
	return 0;
}








void pushFormatTable(lua_State *L, const std::vector<graphics::Buffer::DataDeclaration> &fmt);

int buildMainMesh(lua_State *L, int verts, graphics::Mesh **out)
{
	if (!lua_istable(L, verts))
		return 0;

	size_t n = luax_objlen(L, verts);
	int bad = 0;
	{
		std::vector<float> data(n * 12);
		for (size_t i = 0; i < n && !bad; i++)
		{
			lua_rawgeti(L, verts, (int)i + 1);
			if (!lua_istable(L, -1))
			{
				bad = (int)i + 1;
				lua_pop(L, 1);
				break;
			}
			float *o = &data[i * 12];
			for (int j = 0; j < 12; j++)
			{
				lua_rawgeti(L, -1, j + 1);
				o[j] = lua_isnumber(L, -1) ? (float)lua_tonumber(L, -1) : (j >= 8 ? 1.0f : 0.0f);
				lua_pop(L, 1);
			}
			lua_pop(L, 1);
		}

		if (!bad)
		{
			*out = nullptr;
			try
			{
				*out = gfx()->newMesh(mainFormat, data.data(), data.size() * sizeof(float),
									  graphics::PRIMITIVE_TRIANGLES, graphics::BUFFERDATAUSAGE_STATIC);
			}
			catch (love::Exception &e)
			{
				lua_pushstring(L, e.what());
				bad = -1;
			}
		}
	}

	if (bad == -1)
		return lua_error(L);
	if (bad > 0)
		return luaL_error(L, "mgold: vertex #%d is not a table", bad);
	return 1;
}

int w_newMesh(lua_State *L)
{
	const char *name = luaL_checkstring(L, 1);
	luaL_checkany(L, 2);
	luaL_checkany(L, 3);

	lua_newtable(L);
	int mesh = lua_gettop(L);
	lua_rawgeti(L, LUA_REGISTRYINDEX, meshMT);
	lua_setmetatable(L, mesh);

	lua_pushstring(L, name); lua_setfield(L, mesh, "name");
	lua_pushvalue(L, 2); lua_setfield(L, mesh, "verts");
	setBool(L, mesh, "noLight", false);
	lua_newtable(L); lua_setfield(L, mesh, "lightIndices");


	MeshData *d = (MeshData *)lua_newuserdata(L, sizeof(MeshData));
	new (d) MeshData();
	d->count = 0; d->maxModels = 64; d->visibleCount = 0; d->dirtyCount = 0;
	d->dirty = false; d->ptr = nullptr;
	lua_newtable(L); d->modelsRef = luaL_ref(L, LUA_REGISTRYINDEX);
	lua_newtable(L); d->dirtyRef = luaL_ref(L, LUA_REGISTRYINDEX);
	lua_rawgeti(L, LUA_REGISTRYINDEX, dataMT);
	lua_setmetatable(L, -2);
	lua_setfield(L, mesh, "_d");

	lua_rawgeti(L, LUA_REGISTRYINDEX, d->modelsRef); lua_setfield(L, mesh, "models");


	graphics::Mesh *main = nullptr;
	if (!buildMainMesh(L, 2, &main))
	{

		pushPath(L, { "graphics", "newMesh" });
		pushFormatTable(L, mainFormat);
		lua_pushvalue(L, 2);
		lua_pushstring(L, "triangles");
		lua_pushstring(L, "static");
		lua_call(L, 4, 1);
		main = graphics::luax_checkmesh(L, -1);
		main->retain();
		lua_pop(L, 1);
	}
	d->mesh.set(main, Acquire::NORETAIN);

	if (lua_isnoneornil(L, 3))
		d->mesh->setTexture();
	else
	{
		graphics::Texture *tex = graphics::luax_checktexture(L, 3);
		luax_catchexcept(L, [&]() { d->mesh->setTexture(tex); });
	}

	luax_pushtype(L, d->mesh.get());
	lua_setfield(L, mesh, "mesh");

	allocInstance(L, d, d->maxModels);
	syncCounts(L, mesh, d);

	lua_rawgeti(L, LUA_REGISTRYINDEX, meshsRef);
	lua_pushstring(L, name);
	lua_pushvalue(L, mesh);
	lua_settable(L, -3);
	lua_pop(L, 1);

	lua_pushvalue(L, mesh);
	return 1;
}

int w_getMesh(lua_State *L)
{
	const char *name = luaL_checkstring(L, 1);
	lua_rawgeti(L, LUA_REGISTRYINDEX, meshsRef);
	lua_getfield(L, -1, name);
	return 1;
}

int w_getMeshs(lua_State *L)
{
	lua_rawgeti(L, LUA_REGISTRYINDEX, meshsRef);
	return 1;
}

int w_update(lua_State *L)
{
	lua_rawgeti(L, LUA_REGISTRYINDEX, meshsRef);
	int t = lua_gettop(L);
	lua_pushnil(L);
	while (lua_next(L, t))
	{
		updateMesh(L, lua_gettop(L));
		lua_pop(L, 1);
	}
	lua_pop(L, 1);

	MGOLD_updateLight(L);
	return 0;
}


void pushFormatTable(lua_State *L, const std::vector<graphics::Buffer::DataDeclaration> &fmt)
{
	static const char *const names[] = { "floatvec2", "floatvec3", "floatvec4" };
	lua_createtable(L, (int)fmt.size(), 0);
	for (size_t i = 0; i < fmt.size(); i++)
	{
		lua_createtable(L, 0, 3);
		lua_pushstring(L, fmt[i].name.c_str()); lua_setfield(L, -2, "name");
		int comps = fmt[i].format == graphics::DATAFORMAT_FLOAT_VEC2 ? 0 : fmt[i].format == graphics::DATAFORMAT_FLOAT_VEC3 ? 1 : 2;
		lua_pushstring(L, names[comps]); lua_setfield(L, -2, "format");
		lua_pushinteger(L, fmt[i].bindingLocation); lua_setfield(L, -2, "location");
		lua_rawseti(L, -2, (int)i + 1);
	}
}

int newRefTable(lua_State *L, const luaL_Reg *funcs)
{
	lua_newtable(L);
	lua_newtable(L);
	for (const luaL_Reg *r = funcs; r->name; r++)
	{
		lua_pushcfunction(L, r->func);
		lua_setfield(L, -2, r->name);
	}
	lua_setfield(L, -2, "__index");
	return luaL_ref(L, LUA_REGISTRYINDEX);
}

}

bool MGOLD_getMeshDrawInfo(lua_State *L, int idx, graphics::Mesh *&mesh, int &visibleCount)
{
	idx = (idx < 0) ? lua_gettop(L) + idx + 1 : idx;
	if (!lua_istable(L, idx))
		return false;

	MeshData *d = meshData(L, idx);
	if (!d || !d->mesh.get())
		return false;

	mesh = d->mesh.get();
	visibleCount = d->visibleCount;
	return true;
}

void MGOLD_setObjectFunc(lua_State *L)
{
	static const luaL_Reg meshFuncs[] = {
		{ "newModel", w_mesh_newModel },
		{ "grow", w_mesh_grow },
		{ "swapModels", w_mesh_swapModels },
		{ "swapAndPop", w_mesh_swapAndPop },
		{ "update", w_mesh_update },
		{ "destroy", w_mesh_destroy },
		{ nullptr, nullptr }
	};
	static const luaL_Reg modelFuncs[] = {
		{ "setVisibility", w_model_setVisibility },
		{ "setColor", w_model_setColor },
		{ "setPosition", w_model_setPosition },
		{ "setRotation", w_model_setRotation },
		{ "setScale", w_model_setScale },
		{ "lookAt", w_model_lookAt },
		{ "updateMatrix", w_model_updateMatrix },
		{ "destroy", w_model_destroy },
		{ nullptr, nullptr }
	};

	meshMT = newRefTable(L, meshFuncs);
	modelMT = newRefTable(L, modelFuncs);

	lua_newtable(L);
	lua_pushcfunction(L, w_dataGC);
	lua_setfield(L, -2, "__gc");
	dataMT = luaL_ref(L, LUA_REGISTRYINDEX);

	lua_newtable(L);
	meshsRef = luaL_ref(L, LUA_REGISTRYINDEX);


	for (int k = 0; k < K_COUNT; k++)
	{
		lua_pushstring(L, KEY_NAMES[k]);
		keyRef[k] = luaL_ref(L, LUA_REGISTRYINDEX);
	}

	{
		using graphics::Buffer;
		mainFormat.clear();
		mainFormat.push_back(Buffer::DataDeclaration("VertexPosition", graphics::DATAFORMAT_FLOAT_VEC3, 0, 0));
		mainFormat.push_back(Buffer::DataDeclaration("vertNormal", graphics::DATAFORMAT_FLOAT_VEC3, 0, 3));
		mainFormat.push_back(Buffer::DataDeclaration("VertexTexCoord", graphics::DATAFORMAT_FLOAT_VEC2, 0, 1));
		mainFormat.push_back(Buffer::DataDeclaration("VertexColor", graphics::DATAFORMAT_FLOAT_VEC4, 0, 2));

		instFormat.clear();
		for (int i = 0; i < 5; i++)
			instFormat.push_back(Buffer::DataDeclaration(INST_NAMES[i], graphics::DATAFORMAT_FLOAT_VEC4, 0, 4 + i));
	}

	lua_newtable(L);
	lua_pushcfunction(L, w_newMesh); lua_setfield(L, -2, "newMesh");
	lua_pushcfunction(L, w_getMesh); lua_setfield(L, -2, "getMesh");
	lua_pushcfunction(L, w_getMeshs); lua_setfield(L, -2, "getMeshs");
	lua_pushcfunction(L, w_update); lua_setfield(L, -2, "update");
}

}
}
