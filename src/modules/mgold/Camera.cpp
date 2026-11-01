#include "modules/mgold/Camera.h"
#include "modules/mgold/ShaderBridge.h"

#include "common/Module.h"
#include "modules/graphics/Graphics.h"
#include "modules/keyboard/Keyboard.h"
#include "modules/mouse/Mouse.h"

#include <cmath>
#include <string>
#include <unordered_map>
#include <vector>

// Se o seu LOVE for o 12.x, defina MGOLD_LOVE12 (Graphics::getRenderTargets em vez de getCanvas).

namespace love
{
namespace mgold
{

namespace
{

// ---------------------------------------------------------------------------------------------
// Estado da camera: tudo nativo, o Lua so e tocado ao enviar matrizes (quando mudaram).
// ---------------------------------------------------------------------------------------------

struct Vec3{ double x, y, z; };

struct CamState{
	double fov, nearClip, farClip, aspect;
	Vec3 pos, target, up;
};

const double PI = 3.14159265358979323846;

CamState cam = {PI / 2, 0.01, 100.0, 1.0, {0, 0, 0}, {0, 0, 1}, {0, 1, 0}};

double direction = 0.0; // fpsController.direction
double pitch = 0.0;     // fpsController.pitch

double viewM[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
double projM[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};

bool viewDirty = true;
bool projDirty = true;
bool matrixSent = false;
bool aspectReady = false;

std::unordered_map<std::string, CamState> saves;

// tabelas Lua (criadas por matrices.newMatrix) so para enviar ao shader
int viewRef = LUA_NOREF;
int projRef = LUA_NOREF;

// ---------------------------------------------------------------------------------------------
// utilidades
// ---------------------------------------------------------------------------------------------

inline void normalize(double &x, double &y, double &z){
	double len = std::sqrt(x * x + y * y + z * z);
	if (len == 0.0){ x = y = z = 0.0; return; }
	x /= len; y /= len; z /= len;
}

inline void cross(double ax, double ay, double az, double bx, double by, double bz,
                  double &rx, double &ry, double &rz){
	rx = ay * bz - az * by;
	ry = az * bx - ax * bz;
	rz = ax * by - ay * bx;
}

inline double dot(double ax, double ay, double az, double bx, double by, double bz){
	return ax * bx + ay * by + az * bz;
}

double windowAspect(){
	auto *gfx = Module::getInstance<love::graphics::Graphics>(Module::M_GRAPHICS);
	if (!gfx)
		return cam.aspect;
	double w = gfx->getWidth(), h = gfx->getHeight();
	return (w > 0 && h > 0) ? w / h : cam.aspect;
}

// o Graphics ainda nao existe quando o modulo abre, entao o aspect e resolvido na primeira vez
inline void ensureAspect(){
	if (!aspectReady){
		cam.aspect = windowAspect();
		aspectReady = true;
	}
}

// ---------------------------------------------------------------------------------------------
// matrizes (mesmo layout de Matrices.cpp: indices 0-based, translacao em 3, 7, 11)
// ---------------------------------------------------------------------------------------------

void buildView(){
	double z1 = cam.pos.x - cam.target.x, z2 = cam.pos.y - cam.target.y, z3 = cam.pos.z - cam.target.z;
	normalize(z1, z2, z3);

	double x1, x2, x3;
	cross(cam.up.x, cam.up.y, cam.up.z, z1, z2, z3, x1, x2, x3);
	normalize(x1, x2, x3);

	double y1, y2, y3;
	cross(z1, z2, z3, x1, x2, x3, y1, y2, y3);

	double ex = cam.pos.x, ey = cam.pos.y, ez = cam.pos.z;

	viewM[0] = x1; viewM[1] = x2; viewM[2]  = x3; viewM[3]  = -dot(x1, x2, x3, ex, ey, ez);
	viewM[4] = y1; viewM[5] = y2; viewM[6]  = y3; viewM[7]  = -dot(y1, y2, y3, ex, ey, ez);
	viewM[8] = z1; viewM[9] = z2; viewM[10] = z3; viewM[11] = -dot(z1, z2, z3, ex, ey, ez);
	viewM[12] = 0; viewM[13] = 0; viewM[14] = 0;  viewM[15] = 1;
}

void buildProjection(){
	double top = cam.nearClip * std::tan(cam.fov / 2);
	double bottom = -top;
	double right = top * cam.aspect;
	double left = -right;
	double n = cam.nearClip, f = cam.farClip;

	projM[0] = 2 * n / (right - left); projM[1] = 0; projM[2] = (right + left) / (right - left); projM[3] = 0;
	projM[4] = 0; projM[5] = 2 * n / (top - bottom); projM[6] = (top + bottom) / (top - bottom); projM[7] = 0;
	projM[8] = 0; projM[9] = 0; projM[10] = -(f + n) / (f - n); projM[11] = -2 * f * n / (f - n);
	projM[12] = 0; projM[13] = 0; projM[14] = -1; projM[15] = 0;
}

void buildOrthographic(double size){
	double top = size * std::tan(cam.fov / 2);
	double bottom = -top;
	double right = top * cam.aspect;
	double left = -right;
	double n = cam.nearClip, f = cam.farClip;

	projM[0] = 2 / (right - left); projM[1] = 0; projM[2] = 0; projM[3] = -(right + left) / (right - left);
	projM[4] = 0; projM[5] = 2 / (top - bottom); projM[6] = 0; projM[7] = -(top + bottom) / (top - bottom);
	projM[8] = 0; projM[9] = 0; projM[10] = -2 / (f - n); projM[11] = -(f + n) / (f - n);
	projM[12] = 0; projM[13] = 0; projM[14] = 0; projM[15] = 1;
}

void updateProjection(){
	ensureAspect();
	buildProjection();
	projDirty = true;
	matrixSent = false;
}

void updateView(){
	buildView();
	viewDirty = true;
	matrixSent = false;
}

// ---------------------------------------------------------------------------------------------
// logica de camera
// ---------------------------------------------------------------------------------------------

void getLook(double &vx, double &vy, double &vz){
	vx = cam.target.x - cam.pos.x;
	vy = cam.target.y - cam.pos.y;
	vz = cam.target.z - cam.pos.z;
	double len = std::sqrt(vx * vx + vy * vy + vz * vz);
	if (len > 0){ vx /= len; vy /= len; vz /= len; }
}

void lookAt(double x, double y, double z, double ax, double ay, double az){
	cam.pos = {x, y, z};
	cam.target = {ax, ay, az};

	double dx, dy, dz;
	getLook(dx, dy, dz);
	direction = std::atan2(dx, dz);
	pitch = std::atan2(-dy, std::sqrt(dx * dx + dz * dz));

	updateProjection();
	updateView();
}

// ponteiros nulos = "manter o valor atual" (equivale aos argumentos nil do Lua)
void lookInDirection(const double *x, const double *y, const double *z, const double *dir, const double *pit){
	if (x) cam.pos.x = *x;
	if (y) cam.pos.y = *y;
	if (z) cam.pos.z = *z;
	if (dir) direction = *dir;
	if (pit) pitch = *pit;

	double cosPitch = std::cos(pitch);
	cam.target.x = cam.pos.x + std::sin(direction) * cosPitch;
	cam.target.y = cam.pos.y - std::sin(pitch);
	cam.target.z = cam.pos.z + std::cos(direction) * cosPitch;

	updateProjection();
	updateView();
}

// ---------------------------------------------------------------------------------------------
// input (LOVE nativo, sem passar pelo Lua)
// ---------------------------------------------------------------------------------------------

struct Keys{
	std::vector<love::keyboard::Keyboard::Key> w, a, s, d, space, lshift;
	bool ready = false;
};

Keys keys;

void initKeys(){
	using love::keyboard::Keyboard;
	Keyboard::Key k;
	if (Keyboard::getConstant("w", k)) keys.w.push_back(k);
	if (Keyboard::getConstant("a", k)) keys.a.push_back(k);
	if (Keyboard::getConstant("s", k)) keys.s.push_back(k);
	if (Keyboard::getConstant("d", k)) keys.d.push_back(k);
	if (Keyboard::getConstant("space", k)) keys.space.push_back(k);
	if (Keyboard::getConstant("lshift", k)) keys.lshift.push_back(k);
	keys.ready = true;
}

// ---------------------------------------------------------------------------------------------
// envio ao shader
// ---------------------------------------------------------------------------------------------

void pushMatrixTable(lua_State *L, int &ref, const double *m){
	if (ref == LUA_NOREF){
		lua_getglobal(L, "love");
		lua_getfield(L, -1, "mgold");
		lua_getfield(L, -1, "matrices");
		lua_getfield(L, -1, "newMatrix");
		lua_call(L, 0, 1);
		ref = luaL_ref(L, LUA_REGISTRYINDEX);
		lua_pop(L, 3);
	}
	lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
	for (int i = 0; i < 16; i++){
		lua_pushnumber(L, m[i]);
		lua_rawseti(L, -2, i + 1);
	}
}

bool renderingToCanvas(){
	auto *gfx = Module::getInstance<love::graphics::Graphics>(Module::M_GRAPHICS);
	if (!gfx)
		return false;
	return !gfx->getRenderTargets().colors.empty();
}

// ---------------------------------------------------------------------------------------------
// bindings
// ---------------------------------------------------------------------------------------------

int w_save(lua_State *L){
	const char *name = luaL_checkstring(L, 1);
	ensureAspect();
	saves[name] = cam;

	cam.fov = PI / 2;
	cam.nearClip = 0.01;
	cam.farClip = 100.0;
	cam.aspect = windowAspect();
	cam.pos = {0, 0, 0};
	cam.target = {0, 0, 1};
	cam.up = {0, 1, 0};
	return 0;
}

int w_restore(lua_State *L){
	const char *name = luaL_checkstring(L, 1);
	auto it = saves.find(name);
	if (it == saves.end()){
		lua_pushboolean(L, 0);
		lua_pushfstring(L, "Camera save not found: %s", name);
		return 2;
	}
	cam = it->second;
	aspectReady = true;
	lookAt(cam.pos.x, cam.pos.y, cam.pos.z, cam.target.x, cam.target.y, cam.target.z);
	lua_pushboolean(L, 1);
	return 1;
}

int w_isRenderingToCanvas(lua_State *L){
	lua_pushboolean(L, renderingToCanvas());
	return 1;
}

int w_sendCameraMatrix(lua_State *L){
	if (matrixSent)
		return 0;
	matrixSent = true;

	// uniforms persistem no shader: so reenvia as matrizes que mudaram
	if (viewDirty){
		pushMatrixTable(L, viewRef, viewM);
		bridge::sendTop(L, "viewMatrix");
		viewDirty = false;
	}
	if (projDirty){
		pushMatrixTable(L, projRef, projM);
		bridge::sendTop(L, "projectionMatrix");
		projDirty = false;
	}
	bridge::sendBool(L, "isCanvasEnabled", renderingToCanvas());
	return 0;
}

int w_getDirectionPitch(lua_State *L){
	lua_pushnumber(L, direction);
	lua_pushnumber(L, pitch);
	return 2;
}

int w_getLookVector(lua_State *L){
	double x, y, z;
	getLook(x, y, z);
	lua_pushnumber(L, x);
	lua_pushnumber(L, y);
	lua_pushnumber(L, z);
	return 3;
}

int w_lookAt(lua_State *L){
	lookAt(luaL_checknumber(L, 1), luaL_checknumber(L, 2), luaL_checknumber(L, 3),
	       luaL_checknumber(L, 4), luaL_checknumber(L, 5), luaL_checknumber(L, 6));
	return 0;
}

int w_lookInDirection(lua_State *L){
	double v[5];
	const double *p[5];
	for (int i = 0; i < 5; i++){
		if (lua_isnoneornil(L, i + 1))
			p[i] = nullptr;
		else{
			v[i] = luaL_checknumber(L, i + 1);
			p[i] = &v[i];
		}
	}
	lookInDirection(p[0], p[1], p[2], p[3], p[4]);
	return 0;
}

int w_updateViewMatrix(lua_State *){ updateView(); return 0; }
int w_updateProjectionMatrix(lua_State *){ updateProjection(); return 0; }

int w_resize(lua_State *L){
	double w = luaL_optnumber(L, 1, 0), h = luaL_optnumber(L, 2, 0);
	if (w > 0 && h > 0)
		cam.aspect = w / h;
	else
		cam.aspect = windowAspect();
	aspectReady = true;
	updateProjection();
	return 0;
}

int w_updateOrthographicMatrix(lua_State *L){
	ensureAspect();
	buildOrthographic(luaL_optnumber(L, 1, 5));
	projDirty = true;
	matrixSent = false;
	return 0;
}

int w_firstPersonMovement(lua_State *L){
	double dt = luaL_checknumber(L, 1);

	if (!keys.ready)
		initKeys();
	auto *kb = Module::getInstance<love::keyboard::Keyboard>(Module::M_KEYBOARD);
	if (!kb)
		return 0;

	const double speed = 12.0;
	double moveX = 0, moveY = 0;
	bool moved = false;

	if (kb->isDown(keys.w)) moveX += 1;
	if (kb->isDown(keys.d)) moveY += 1;
	if (kb->isDown(keys.s)) moveX -= 1;
	if (kb->isDown(keys.a)) moveY -= 1;

	if (kb->isDown(keys.space)){ cam.pos.y += speed * dt; moved = true; }
	if (kb->isDown(keys.lshift)){ cam.pos.y -= speed * dt; moved = true; }

	if (moveX != 0 || moveY != 0){
		double angle = std::atan2(-moveY, moveX);
		cam.pos.x += std::sin(direction + angle) * speed * dt;
		cam.pos.z += std::cos(direction + angle) * speed * dt;
		moved = true;
	}

	if (moved)
		lookInDirection(nullptr, nullptr, nullptr, nullptr, nullptr);
	return 0;
}

int w_firstPersonLook(lua_State *L){
	double dx = luaL_checknumber(L, 1), dy = luaL_checknumber(L, 2);

	auto *ms = Module::getInstance<love::mouse::Mouse>(Module::M_MOUSE);
	if (ms && !ms->getRelativeMode())
		ms->setRelativeMode(true);

	const double sensitivity = 1.0 / 300.0;
	const double limit = PI * 0.49;

	direction -= dx * sensitivity;
	pitch += dy * sensitivity;
	if (pitch > limit) pitch = limit;
	if (pitch < -limit) pitch = -limit;

	lookInDirection(nullptr, nullptr, nullptr, &direction, &pitch);
	return 0;
}

int w_update(lua_State *){
	matrixSent = false;
	return 0;
}

// ---- acesso ao estado que antes era campo da tabela camera (camera.position[1], camera.fov...) ----

int w_getPosition(lua_State *L){
	lua_pushnumber(L, cam.pos.x); lua_pushnumber(L, cam.pos.y); lua_pushnumber(L, cam.pos.z);
	return 3;
}
int w_setPosition(lua_State *L){
	cam.pos = {luaL_checknumber(L, 1), luaL_checknumber(L, 2), luaL_checknumber(L, 3)};
	return 0;
}
int w_getTarget(lua_State *L){
	lua_pushnumber(L, cam.target.x); lua_pushnumber(L, cam.target.y); lua_pushnumber(L, cam.target.z);
	return 3;
}
int w_setTarget(lua_State *L){
	cam.target = {luaL_checknumber(L, 1), luaL_checknumber(L, 2), luaL_checknumber(L, 3)};
	return 0;
}
int w_getUp(lua_State *L){
	lua_pushnumber(L, cam.up.x); lua_pushnumber(L, cam.up.y); lua_pushnumber(L, cam.up.z);
	return 3;
}
int w_setUp(lua_State *L){
	cam.up = {luaL_checknumber(L, 1), luaL_checknumber(L, 2), luaL_checknumber(L, 3)};
	return 0;
}
int w_getFov(lua_State *L){ lua_pushnumber(L, cam.fov); return 1; }
int w_setFov(lua_State *L){ cam.fov = luaL_checknumber(L, 1); return 0; }
int w_getClip(lua_State *L){
	lua_pushnumber(L, cam.nearClip); lua_pushnumber(L, cam.farClip);
	return 2;
}
int w_setClip(lua_State *L){
	cam.nearClip = luaL_checknumber(L, 1);
	cam.farClip = luaL_checknumber(L, 2);
	return 0;
}
int w_getAspectRatio(lua_State *L){ ensureAspect(); lua_pushnumber(L, cam.aspect); return 1; }

int w_getViewMatrix(lua_State *L){ pushMatrixTable(L, viewRef, viewM); return 1; }
int w_getProjectionMatrix(lua_State *L){ pushMatrixTable(L, projRef, projM); return 1; }

} // namespace

// ---- acesso nativo para outros modulos (Player.cpp) ----
void MGOLD_cameraSetPosition(double x, double y, double z){
	lookInDirection(&x, &y, &z, nullptr, nullptr);
}

double MGOLD_cameraYaw(){
	return direction;
}

void MGOLD_cameraLookVector(double &x, double &y, double &z){
	getLook(x, y, z);
}

// chamada direta de C++ (Render.cpp), sem passar pelo Lua
void MGOLD_sendCameraMatrix(lua_State *L){
	w_sendCameraMatrix(L);
}

void MGOLD_setCameraFunc(lua_State *L){
	static const luaL_Reg funcs[] = {
		{"save", w_save},
		{"restore", w_restore},
		{"isRenderingToCanvas", w_isRenderingToCanvas},
		{"sendCameraMatrix", w_sendCameraMatrix},
		{"getDirectionPitch", w_getDirectionPitch},
		{"getLookVector", w_getLookVector},
		{"lookAt", w_lookAt},
		{"lookInDirection", w_lookInDirection},
		{"updateViewMatrix", w_updateViewMatrix},
		{"updateProjectionMatrix", w_updateProjectionMatrix},
		{"resize", w_resize},
		{"updateOrthographicMatrix", w_updateOrthographicMatrix},
		{"firstPersonMovement", w_firstPersonMovement},
		{"firstPersonLook", w_firstPersonLook},
		{"update", w_update},

		{"getPosition", w_getPosition},
		{"setPosition", w_setPosition},
		{"getTarget", w_getTarget},
		{"setTarget", w_setTarget},
		{"getUp", w_getUp},
		{"setUp", w_setUp},
		{"getFov", w_getFov},
		{"setFov", w_setFov},
		{"getClip", w_getClip},
		{"setClip", w_setClip},
		{"getAspectRatio", w_getAspectRatio},
		{"getViewMatrix", w_getViewMatrix},
		{"getProjectionMatrix", w_getProjectionMatrix},
		{nullptr, nullptr}
	};

	lua_newtable(L);
	for (const luaL_Reg *r = funcs; r->name; r++){
		lua_pushcfunction(L, r->func);
		lua_setfield(L, -2, r->name);
	}
}

}
}