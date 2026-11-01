#include "modules/mgold/Object.h"
#include "modules/mgold/Light.h"

#include "common/runtime.h"
#include "modules/data/ByteData.h"

#include <cmath>
#include <cstring>
#include <initializer_list>
#include <new>

// ---------------------------------------------------------------------------------------------
// love.mgold.object  (antes mgi3/object.lua)
//
// Contrato Lua mantido: malhas e modelos continuam sendo TABELAS com os mesmos campos
// (mesh.mesh / mesh.visibleCount / mesh.shader lidos pelo Render.cpp, model.position / rotation /
// scale / color / matrix / uid / physicsId / collisionDirty lidos pelo Physics), entao o resto do
// codigo nao precisa mudar.
//
// O que ficou nativo (caminho quente):
//  * setPosition / setRotation / setScale / setColor / lookAt / setVisibility
//  * lista de sujos, calculo da matriz, escrita no buffer de instancias (float*) e upload
//    (um unico Mesh:setVertices por malha por frame, so quando algo mudou)
//  * swap / swapAndPop / grow
// Sem FFI: o buffer e um love.data ByteData escrito direto por ponteiro C.
// ---------------------------------------------------------------------------------------------

namespace love
{
namespace mgold
{

namespace
{

const int FLOATS_PER_INSTANCE = 20; // 16 matriz + 4 cor
const int BYTES_PER_INSTANCE = FLOATS_PER_INSTANCE * (int) sizeof(float);

struct MeshData{
	int count, maxModels, visibleCount, dirtyCount;
	bool dirty;
	float *ptr;
	int dataRef, instRef, modelsRef, dirtyRef;
};

int meshMT = LUA_NOREF;      // metatable das malhas
int modelMT = LUA_NOREF;     // metatable dos modelos
int dataMT = LUA_NOREF;      // metatable do userdata MeshData (__gc)
int meshsRef = LUA_NOREF;    // tabela nome -> malha
int mainFmtRef = LUA_NOREF;  // formato de vertice
int instFmtRef = LUA_NOREF;  // formato de instancia

const char *INST_NAMES[5] = {"InstanceMat1", "InstanceMat2", "InstanceMat3", "InstanceMat4", "InstanceColor"};

// ---------------------------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------------------------

inline int absidx(lua_State *L, int i){
	return (i < 0) ? lua_gettop(L) + i + 1 : i;
}

void pushPath(lua_State *L, std::initializer_list<const char *> path){
	lua_getglobal(L, "love");
	for (const char *p : path){
		lua_getfield(L, -1, p);
		lua_remove(L, -2);
	}
}

double num(lua_State *L, int t, const char *k){
	lua_getfield(L, t, k);
	double v = lua_tonumber(L, -1);
	lua_pop(L, 1);
	return v;
}

int intField(lua_State *L, int t, const char *k){
	lua_getfield(L, t, k);
	int v = (int) lua_tointeger(L, -1);
	lua_pop(L, 1);
	return v;
}

bool boolField(lua_State *L, int t, const char *k){
	lua_getfield(L, t, k);
	bool v = lua_toboolean(L, -1) != 0;
	lua_pop(L, 1);
	return v;
}

void setNum(lua_State *L, int t, const char *k, double v){
	lua_pushnumber(L, v);
	lua_setfield(L, t, k);
}

void setInt(lua_State *L, int t, const char *k, int v){
	lua_pushinteger(L, v);
	lua_setfield(L, t, k);
}

void setBool(lua_State *L, int t, const char *k, bool v){
	lua_pushboolean(L, v ? 1 : 0);
	lua_setfield(L, t, k);
}

MeshData *meshData(lua_State *L, int mesh){
	lua_getfield(L, mesh, "_d");
	MeshData *d = (MeshData *) lua_touserdata(L, -1);
	lua_pop(L, 1);
	return d;
}

void syncCounts(lua_State *L, int mesh, MeshData *d){
	setInt(L, mesh, "count", d->count);
	setInt(L, mesh, "visibleCount", d->visibleCount);
}

void release(lua_State *L, MeshData *d){
	int *refs[4] = {&d->dataRef, &d->instRef, &d->modelsRef, &d->dirtyRef};
	for (int i = 0; i < 4; i++){
		if (*refs[i] != LUA_NOREF){
			luaL_unref(L, LUA_REGISTRYINDEX, *refs[i]);
			*refs[i] = LUA_NOREF;
		}
	}
	d->ptr = nullptr;
}

void pushCopy(lua_State *L, int vec){
	lua_getfield(L, vec, "copy");
	lua_pushvalue(L, vec);
	lua_call(L, 1, 1);
}

// ---------------------------------------------------------------------------------------------
// buffer de instancias
// ---------------------------------------------------------------------------------------------

void allocInstance(lua_State *L, int mesh, MeshData *d, int newMax, float *old, int oldMax){
	pushPath(L, {"data", "newByteData"});
	lua_pushinteger(L, newMax * BYTES_PER_INSTANCE);
	lua_call(L, 1, 1);

	auto *bd = luax_checktype<love::data::ByteData>(L, -1);
	float *p = (float *) bd->getData();
	std::memset(p, 0, (size_t) newMax * BYTES_PER_INSTANCE);
	if (old)
		std::memcpy(p, old, (size_t) oldMax * BYTES_PER_INSTANCE);

	if (d->dataRef != LUA_NOREF)
		luaL_unref(L, LUA_REGISTRYINDEX, d->dataRef);
	d->dataRef = luaL_ref(L, LUA_REGISTRYINDEX);
	d->ptr = p;

	pushPath(L, {"graphics", "newMesh"});
	lua_rawgeti(L, LUA_REGISTRYINDEX, instFmtRef);
	lua_pushinteger(L, newMax);
	lua_pushstring(L, "triangles");
	lua_pushstring(L, "stream");
	lua_call(L, 4, 1);

	if (d->instRef != LUA_NOREF)
		luaL_unref(L, LUA_REGISTRYINDEX, d->instRef);
	d->instRef = luaL_ref(L, LUA_REGISTRYINDEX);

	lua_getfield(L, mesh, "mesh"); // mesh do LOVE
	for (int i = 0; i < 5; i++){
		lua_getfield(L, -1, "attachAttribute");
		lua_pushvalue(L, -2);
		lua_pushstring(L, INST_NAMES[i]);
		lua_rawgeti(L, LUA_REGISTRYINDEX, d->instRef);
		lua_pushstring(L, "perinstance");
		lua_call(L, 4, 0);
	}
	lua_pop(L, 1);

	d->maxModels = newMax;
}

void grow(lua_State *L, int mesh, MeshData *d){
	if (d->count <= d->maxModels)
		return;

	int oldMax = d->maxModels, newMax = oldMax;
	while (newMax < d->count)
		newMax *= 2;

	allocInstance(L, mesh, d, newMax, d->ptr, oldMax);
	d->dirty = true;
}

// ---------------------------------------------------------------------------------------------
// modelos
// ---------------------------------------------------------------------------------------------

void touch(lua_State *L, int m){
	setBool(L, m, "dirty", true);

	lua_getfield(L, m, "mesh");
	int mesh = lua_gettop(L);
	MeshData *d = meshData(L, mesh);
	d->dirty = true;

	if (!boolField(L, m, "queued")){
		setBool(L, m, "queued", true);
		d->dirtyCount++;
		lua_rawgeti(L, LUA_REGISTRYINDEX, d->dirtyRef);
		lua_pushvalue(L, m);
		lua_rawseti(L, -2, d->dirtyCount);
		lua_pop(L, 1);
	}
	lua_pop(L, 1);
}

// matriz de transformacao (mesmo calculo de Matrices.cpp). Sempre recalcula: sin/cos nativos
// custam menos do que decidir o que reaproveitar.
void computeMatrix(lua_State *L, int m, float *out){
	double t[3], r[3], s[3];

	lua_getfield(L, m, "position");
	t[0] = num(L, lua_gettop(L), "x"); t[1] = num(L, lua_gettop(L), "y"); t[2] = num(L, lua_gettop(L), "z");
	lua_pop(L, 1);

	lua_getfield(L, m, "rotation");
	r[0] = num(L, lua_gettop(L), "x"); r[1] = num(L, lua_gettop(L), "y"); r[2] = num(L, lua_gettop(L), "z");
	lua_pop(L, 1);

	lua_getfield(L, m, "scale");
	s[0] = num(L, lua_gettop(L), "x"); s[1] = num(L, lua_gettop(L), "y"); s[2] = num(L, lua_gettop(L), "z");
	lua_pop(L, 1);

	double ca = std::cos(r[2]), cb = std::cos(r[1]), cc = std::cos(r[0]);
	double sa = std::sin(r[2]), sb = std::sin(r[1]), sc = std::sin(r[0]);

	double M[16] = {
		ca * cb * s[0], (ca * sb * sc - sa * cc) * s[1], (ca * sb * cc + sa * sc) * s[2], t[0],
		sa * cb * s[0], (sa * sb * sc + ca * cc) * s[1], (sa * sb * cc - ca * sc) * s[2], t[1],
		-sb * s[0],     cb * sc * s[1],                  cb * cc * s[2],                  t[2],
		0, 0, 0, 1
	};

	// model.matrix continua atualizada (o Physics le)
	lua_getfield(L, m, "matrix");
	for (int i = 0; i < 16; i++){
		lua_pushnumber(L, M[i]);
		lua_rawseti(L, -2, i + 1);
	}
	lua_pop(L, 1);

	for (int i = 0; i < 16; i++)
		out[i] = (float) M[i];
}

void swapModels(lua_State *L, int mesh, MeshData *d, int a, int b){
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

	setInt(L, ma, "id", b);
	setInt(L, mb, "id", a);
	lua_pushfstring(L, "%d%p", b, lua_topointer(L, mesh)); lua_setfield(L, ma, "uid");
	lua_pushfstring(L, "%d%p", a, lua_topointer(L, mesh)); lua_setfield(L, mb, "uid");

	touch(L, ma);
	touch(L, mb);

	lua_pop(L, 3);
}

void swapAndPop(lua_State *L, int mesh, MeshData *d, int model){
	int id = intField(L, model, "id");

	if (boolField(L, model, "visible")){
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

void destroyModel(lua_State *L, int model){
	if (!boolField(L, model, "alive"))
		return;
	setBool(L, model, "alive", false);

	lua_getfield(L, model, "mesh");
	int mesh = lua_gettop(L);
	swapAndPop(L, mesh, meshData(L, mesh), model);
	lua_pop(L, 1);
}

// --- metodos de modelo ---

int w_model_setVisibility(lua_State *L){
	luaL_checktype(L, 1, LUA_TTABLE);
	bool flag = lua_toboolean(L, 2) != 0;

	if (!boolField(L, 1, "alive") || boolField(L, 1, "visible") == flag)
		return 0;

	lua_getfield(L, 1, "mesh");
	int mesh = lua_gettop(L);
	MeshData *d = meshData(L, mesh);
	int id = intField(L, 1, "id");

	setBool(L, 1, "visible", flag);

	if (flag){
		swapModels(L, mesh, d, id, d->visibleCount + 1);
		d->visibleCount++;
	}
	else{
		swapModels(L, mesh, d, id, d->visibleCount);
		d->visibleCount--;
	}

	d->dirty = true;
	syncCounts(L, mesh, d);
	touch(L, 1);
	lua_pop(L, 1);
	return 0;
}

int w_model_setColor(lua_State *L){
	lua_getfield(L, 1, "color");
	int c = lua_gettop(L);
	setNum(L, c, "r", luaL_optnumber(L, 2, 0));
	setNum(L, c, "g", luaL_optnumber(L, 3, 0));
	setNum(L, c, "b", luaL_optnumber(L, 4, 0));
	setNum(L, c, "a", luaL_optnumber(L, 5, 1));
	lua_pop(L, 1);
	touch(L, 1);
	return 0;
}

int w_model_setPosition(lua_State *L){
	lua_getfield(L, 1, "position");
	int p = lua_gettop(L);
	setNum(L, p, "x", luaL_checknumber(L, 2));
	setNum(L, p, "y", luaL_checknumber(L, 3));
	setNum(L, p, "z", luaL_checknumber(L, 4));
	lua_pop(L, 1);

	setBool(L, 1, "lightDirty", true);
	setBool(L, 1, "collisionDirty", true);
	touch(L, 1);
	return 0;
}

int w_model_setRotation(lua_State *L){
	lua_getfield(L, 1, "rotation");
	int r = lua_gettop(L);
	setNum(L, r, "x", luaL_optnumber(L, 2, 0));
	setNum(L, r, "y", luaL_optnumber(L, 3, 0));
	setNum(L, r, "z", luaL_optnumber(L, 4, 0));
	lua_pop(L, 1);

	setBool(L, 1, "rsDirty", true);
	setBool(L, 1, "collisionDirty", true);
	touch(L, 1);
	return 0;
}

int w_model_setScale(lua_State *L){
	double x = luaL_checknumber(L, 2);
	double y = luaL_optnumber(L, 3, x);
	double z = luaL_optnumber(L, 4, y);

	lua_getfield(L, 1, "scale");
	int s = lua_gettop(L);
	setNum(L, s, "x", x);
	setNum(L, s, "y", y);
	setNum(L, s, "z", z);
	lua_pop(L, 1);

	setBool(L, 1, "rsDirty", true);
	setBool(L, 1, "collisionDirty", true);
	touch(L, 1);
	return 0;
}

double indexNum(lua_State *L, int t, int i){
	lua_pushinteger(L, i);
	lua_gettable(L, t); // respeita __index (vec3[1])
	double v = lua_tonumber(L, -1);
	lua_pop(L, 1);
	return v;
}

int w_model_lookAt(lua_State *L){
	luaL_checktype(L, 2, LUA_TTABLE);

	lua_getfield(L, 1, "position");
	int p = lua_gettop(L);
	double px = num(L, p, "x"), py = num(L, p, "y"), pz = num(L, p, "z");
	lua_pop(L, 1);

	double dx = indexNum(L, 2, 1) - px;
	double dy = -indexNum(L, 2, 2) - py;
	double dz = indexNum(L, 2, 3) - pz;

	double yaw = std::atan2(dx, dz);
	double pitch = std::atan2(dy, std::sqrt(dx * dx + dz * dz));

	lua_getfield(L, 1, "rotation");
	int r = lua_gettop(L);
	setNum(L, r, "x", pitch);
	setNum(L, r, "y", yaw);
	setNum(L, r, "z", 0);
	lua_pop(L, 1);

	setBool(L, 1, "rsDirty", true);
	setBool(L, 1, "collisionDirty", true);
	touch(L, 1);
	return 0;
}

int w_model_updateMatrix(lua_State *L){
	float tmp[16];
	computeMatrix(L, 1, tmp);
	return 0;
}

int w_model_destroy(lua_State *L){
	destroyModel(L, 1);
	return 0;
}

// ---------------------------------------------------------------------------------------------
// malhas
// ---------------------------------------------------------------------------------------------

void pushVecFn(lua_State *L, const char *sub, const char *fn){
	pushPath(L, {"mgold", sub, fn});
}

int w_mesh_newModel(lua_State *L){
	luaL_checktype(L, 1, LUA_TTABLE);
	luaL_checktype(L, 2, LUA_TTABLE);
	luaL_checktype(L, 3, LUA_TTABLE);

	MeshData *d = meshData(L, 1);
	d->count++;
	grow(L, 1, d);

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

	if (lua_type(L, 4) == LUA_TNUMBER){
		pushVecFn(L, "vector", "vec3");
		lua_pushvalue(L, 4);
		lua_call(L, 1, 1);
	}
	else{
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

	// ocultos existem: o novo (no fim) precisa ir para a zona dos visiveis
	if (d->visibleCount < d->count - 1)
		swapModels(L, 1, d, d->count, d->visibleCount + 1);
	d->visibleCount++;
	syncCounts(L, 1, d);

	touch(L, m);

	lua_pushvalue(L, m);
	return 1;
}

int w_mesh_grow(lua_State *L){
	grow(L, 1, meshData(L, 1));
	return 0;
}

int w_mesh_swapModels(lua_State *L){
	swapModels(L, 1, meshData(L, 1), (int) luaL_checkinteger(L, 2), (int) luaL_checkinteger(L, 3));
	return 0;
}

int w_mesh_swapAndPop(lua_State *L){
	luaL_checktype(L, 2, LUA_TTABLE);
	swapAndPop(L, 1, meshData(L, 1), 2);
	return 0;
}

void updateMesh(lua_State *L, int mesh){
	MeshData *d = meshData(L, mesh);
	if (!d || !d->dirty)
		return;

	lua_rawgeti(L, LUA_REGISTRYINDEX, d->dirtyRef);
	int list = lua_gettop(L);

	for (int i = 1; i <= d->dirtyCount; i++){
		lua_rawgeti(L, list, i);
		int m = lua_gettop(L);
		lua_pushnil(L);
		lua_rawseti(L, list, i);

		setBool(L, m, "queued", false);

		if (boolField(L, m, "alive")){
			int id = intField(L, m, "id");
			float *o = d->ptr + (size_t) (id - 1) * FLOATS_PER_INSTANCE;

			computeMatrix(L, m, o);
			setBool(L, m, "dirty", false);

			lua_getfield(L, m, "color");
			int c = lua_gettop(L);
			o[16] = (float) num(L, c, "r");
			o[17] = (float) num(L, c, "g");
			o[18] = (float) num(L, c, "b");
			o[19] = (float) num(L, c, "a");
			lua_pop(L, 1);
		}
		lua_pop(L, 1);
	}
	lua_pop(L, 1);
	d->dirtyCount = 0;

	if (d->count > 0){
		lua_rawgeti(L, LUA_REGISTRYINDEX, d->instRef);
		lua_getfield(L, -1, "setVertices");
		lua_pushvalue(L, -2);
		lua_rawgeti(L, LUA_REGISTRYINDEX, d->dataRef);
		lua_pushinteger(L, 1);
		lua_pushinteger(L, d->count);
		lua_call(L, 4, 0);
		lua_pop(L, 1);
	}

	d->dirty = false;
}

int w_mesh_update(lua_State *L){
	updateMesh(L, 1);
	return 0;
}

int w_mesh_destroy(lua_State *L){
	MeshData *d = meshData(L, 1);

	lua_rawgeti(L, LUA_REGISTRYINDEX, d->modelsRef);
	int models = lua_gettop(L);
	for (int i = d->count; i >= 1; i--){
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

int w_dataGC(lua_State *L){
	MeshData *d = (MeshData *) lua_touserdata(L, 1);
	if (d)
		release(L, d);
	return 0;
}

// ---------------------------------------------------------------------------------------------
// modulo
// ---------------------------------------------------------------------------------------------

int w_newMesh(lua_State *L){
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

	// userdata nativo
	MeshData *d = (MeshData *) lua_newuserdata(L, sizeof(MeshData));
	new (d) MeshData();
	d->count = 0; d->maxModels = 64; d->visibleCount = 0; d->dirtyCount = 0;
	d->dirty = false; d->ptr = nullptr;
	d->dataRef = LUA_NOREF; d->instRef = LUA_NOREF;
	lua_newtable(L); d->modelsRef = luaL_ref(L, LUA_REGISTRYINDEX);
	lua_newtable(L); d->dirtyRef = luaL_ref(L, LUA_REGISTRYINDEX);
	lua_rawgeti(L, LUA_REGISTRYINDEX, dataMT);
	lua_setmetatable(L, -2);
	lua_setfield(L, mesh, "_d");

	lua_rawgeti(L, LUA_REGISTRYINDEX, d->modelsRef); lua_setfield(L, mesh, "models");

	// malha estatica do LOVE
	pushPath(L, {"graphics", "newMesh"});
	lua_rawgeti(L, LUA_REGISTRYINDEX, mainFmtRef);
	lua_pushvalue(L, 2);
	lua_pushstring(L, "triangles");
	lua_pushstring(L, "static");
	lua_call(L, 4, 1);
	lua_setfield(L, mesh, "mesh");

	lua_getfield(L, mesh, "mesh");
	lua_getfield(L, -1, "setTexture");
	lua_pushvalue(L, -2);
	lua_pushvalue(L, 3);
	lua_call(L, 2, 0);
	lua_pop(L, 1);

	allocInstance(L, mesh, d, d->maxModels, nullptr, 0);
	syncCounts(L, mesh, d);

	lua_rawgeti(L, LUA_REGISTRYINDEX, meshsRef);
	lua_pushstring(L, name);
	lua_pushvalue(L, mesh);
	lua_settable(L, -3);
	lua_pop(L, 1);

	lua_pushvalue(L, mesh);
	return 1;
}

int w_getMesh(lua_State *L){
	const char *name = luaL_checkstring(L, 1);
	lua_rawgeti(L, LUA_REGISTRYINDEX, meshsRef);
	lua_getfield(L, -1, name);
	return 1;
}

int w_getMeshs(lua_State *L){
	lua_rawgeti(L, LUA_REGISTRYINDEX, meshsRef);
	return 1;
}

int w_update(lua_State *L){
	lua_rawgeti(L, LUA_REGISTRYINDEX, meshsRef);
	int t = lua_gettop(L);
	lua_pushnil(L);
	while (lua_next(L, t)){
		updateMesh(L, lua_gettop(L));
		lua_pop(L, 1);
	}
	lua_pop(L, 1);

	MGOLD_updateLight(L);
	return 0;
}

void pushFormat(lua_State *L, int n, const char *const *names, const char *const *fmts, const int *locs){
	lua_createtable(L, n, 0);
	for (int i = 0; i < n; i++){
		lua_createtable(L, 0, 3);
		lua_pushstring(L, names[i]); lua_setfield(L, -2, "name");
		lua_pushstring(L, fmts[i]); lua_setfield(L, -2, "format");
		lua_pushinteger(L, locs[i]); lua_setfield(L, -2, "location");
		lua_rawseti(L, -2, i + 1);
	}
}

int newRefTable(lua_State *L, const luaL_Reg *funcs){
	lua_newtable(L);
	lua_newtable(L); // __index
	for (const luaL_Reg *r = funcs; r->name; r++){
		lua_pushcfunction(L, r->func);
		lua_setfield(L, -2, r->name);
	}
	lua_setfield(L, -2, "__index");
	return luaL_ref(L, LUA_REGISTRYINDEX);
}

} // namespace

void MGOLD_setObjectFunc(lua_State *L){
	static const luaL_Reg meshFuncs[] = {
		{"newModel", w_mesh_newModel},
		{"grow", w_mesh_grow},
		{"swapModels", w_mesh_swapModels},
		{"swapAndPop", w_mesh_swapAndPop},
		{"update", w_mesh_update},
		{"destroy", w_mesh_destroy},
		{nullptr, nullptr}
	};
	static const luaL_Reg modelFuncs[] = {
		{"setVisibility", w_model_setVisibility},
		{"setColor", w_model_setColor},
		{"setPosition", w_model_setPosition},
		{"setRotation", w_model_setRotation},
		{"setScale", w_model_setScale},
		{"lookAt", w_model_lookAt},
		{"updateMatrix", w_model_updateMatrix},
		{"destroy", w_model_destroy},
		{nullptr, nullptr}
	};

	meshMT = newRefTable(L, meshFuncs);
	modelMT = newRefTable(L, modelFuncs);

	lua_newtable(L);
	lua_pushcfunction(L, w_dataGC);
	lua_setfield(L, -2, "__gc");
	dataMT = luaL_ref(L, LUA_REGISTRYINDEX);

	lua_newtable(L);
	meshsRef = luaL_ref(L, LUA_REGISTRYINDEX);

	{
		static const char *const names[4] = {"VertexPosition", "vertNormal", "VertexTexCoord", "VertexColor"};
		static const char *const fmts[4] = {"floatvec3", "floatvec3", "floatvec2", "floatvec4"};
		static const int locs[4] = {0, 3, 1, 2};
		pushFormat(L, 4, names, fmts, locs);
		mainFmtRef = luaL_ref(L, LUA_REGISTRYINDEX);
	}
	{
		static const char *const fmts[5] = {"floatvec4", "floatvec4", "floatvec4", "floatvec4", "floatvec4"};
		static const int locs[5] = {4, 5, 6, 7, 8};
		pushFormat(L, 5, INST_NAMES, fmts, locs);
		instFmtRef = luaL_ref(L, LUA_REGISTRYINDEX);
	}

	lua_newtable(L); // love.mgold.object
	lua_pushcfunction(L, w_newMesh); lua_setfield(L, -2, "newMesh");
	lua_pushcfunction(L, w_getMesh); lua_setfield(L, -2, "getMesh");
	lua_pushcfunction(L, w_getMeshs); lua_setfield(L, -2, "getMeshs");
	lua_pushcfunction(L, w_update); lua_setfield(L, -2, "update");
}

}
}