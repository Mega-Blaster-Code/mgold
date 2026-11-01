#include "modules/mgold/Player.h"
#include "modules/mgold/Camera.h"

#include "common/Module.h"
#include "modules/keyboard/Keyboard.h"

#include <cmath>
#include <cstring>
#include <new>
#include <vector>

// ---------------------------------------------------------------------------------------------
// love.mgold.player  --  controlador de jogador PADRAO (opcional), 100% nativo.
//
// Customizacao (nada e obrigatorio):
//   * config    : todos os numeros/flags (tabela passada em new / setConfig)
//   * bindings  : teclas (string, lista de strings ou funcao(player) -> bool)
//   * hooks     : getInput, getYaw, getWish, applyFriction, accelerate (funcoes Lua que substituem
//                 o passo nativo correspondente; via config ou player:setHook)
//   * eventos   : player:on("jump", fn)  -- o player NUNCA toca audio
//   * user data : player.qualquerCoisa = x  (guardado numa tabela propria, nao precisa de heranca)
//
// Eventos: jump, land(impacto), crouch, uncrouch, fallDamage(dano, impacto), noclip(on),
//          startMoving, stopMoving, update(dt), teleport(x,y,z)
//
// Custo por frame: leitura de teclas nativa; 2 leituras de tabela (position/speed), 2 escritas e
// 2 chamadas de metodo do body (unsetStatic / updatePhysics), que e a fronteira com o Physics.cpp.
// ---------------------------------------------------------------------------------------------

namespace love
{
namespace mgold
{

namespace
{

using love::keyboard::Keyboard;

const double TAU = 6.28318530717958647692;
const char *META = "mgold.player";

enum Ev{ EV_JUMP, EV_LAND, EV_CROUCH, EV_UNCROUCH, EV_FALLDAMAGE, EV_NOCLIP, EV_START, EV_STOP, EV_UPDATE,
         EV_TELEPORT, EV_COUNT };
const char *EV_NAMES[EV_COUNT] = {"jump", "land", "crouch", "uncrouch", "fallDamage", "noclip", "startMoving",
                                  "stopMoving", "update", "teleport"};

enum Bind{ B_FORWARD, B_BACK, B_LEFT, B_RIGHT, B_JUMP, B_CROUCH, B_WALK, B_NOCLIP, B_COUNT };
const char *B_NAMES[B_COUNT] = {"forward", "back", "left", "right", "jump", "crouch", "walk", "noclip"};

enum Hook{ H_GETINPUT, H_GETYAW, H_GETWISH, H_FRICTION, H_ACCELERATE, H_COUNT };
const char *H_NAMES[H_COUNT] = {"getInput", "getYaw", "getWish", "applyFriction", "accelerate"};

struct Opt{ bool has; double v; };

struct Binding{
	std::vector<Keyboard::Key> keys;
	int fnRef;
};

struct Config{
	double radius = 0.2;

	double groundSpeed = 3, walkFactor = 0.5, crouchFactor = 0.33;
	Opt maxGroundSpeed = {false, 0}, maxAirSpeed = {false, 0};

	double groundAccel = 10, airAccel = 10;
	Opt airCap = {true, 0.4};
	double friction = 6, stopSpeed = 1;

	double gravity = 13, maxFall = 20;

	bool canJump = true;
	double jumpSpeed = 4, jumpCooldown = 0.1;
	bool autoHop = false;
	Opt bhopSpeedCap = {false, 0};

	bool canCrouch = true;
	double crouchDelay = 0.24, uncrouchLift = 0.25;

	bool canWalk = true;

	bool canNoclip = false;
	double noclipSpeed = 8, noclipSlowFactor = 0.3;

	bool fdEnabled = false;
	double fdSafe = 10, fdFatal = 20, fdMax = 100;

	bool useCamera = true;
	double eyeStand = 0, eyeCrouch = 0, eyeSmooth = 15;

	bool bobEnabled = false;
	double bobAmp = 0.03, bobFreq = 1.4;

	bool dipEnabled = false;
	double dipScale = 0.01, dipMax = 0.25, dipRecover = 8;

	bool controls = true;
};

struct Input{
	double forward = 0, side = 0;
	bool jump = false, crouch = false, walk = false, noclip = false;
};

struct Pending{ int ev; double a, b; };

struct Player{
	Config c;
	Binding bind[B_COUNT];

	int bodyRef, shapeStandRef, shapeCrouchRef;
	int listenersRef, userRef, inputRef;
	int hookRef[H_COUNT];
	int listenerCount[EV_COUNT];

	Input in;
	bool noclip, noclipHeld, crouching, jumpHeld, moving;
	double crouchTimer, jumpTimer, timeOnAir;
	double eyeOffset, bobPhase, bobAmount, dip;

	Pending pending[16];
	int pendingCount;
};

int methodsRef = LUA_NOREF;
int metaRef = LUA_NOREF;

struct V3{ double x, y, z; };

// ---------------------------------------------------------------------------------------------
// helpers de tabela
// ---------------------------------------------------------------------------------------------

double numField(lua_State *L, int t, const char *k){
	lua_getfield(L, t, k);
	double v = lua_tonumber(L, -1);
	lua_pop(L, 1);
	return v;
}

void setNumField(lua_State *L, int t, const char *k, double v){
	lua_pushnumber(L, v);
	lua_setfield(L, t, k);
}

void readV3(lua_State *L, int body, const char *key, V3 &out){
	lua_getfield(L, body, key);
	int t = lua_gettop(L);
	out.x = numField(L, t, "x");
	out.y = numField(L, t, "y");
	out.z = numField(L, t, "z");
	lua_pop(L, 1);
}

void writeV3(lua_State *L, int body, const char *key, const V3 &v){
	lua_getfield(L, body, key);
	int t = lua_gettop(L);
	setNumField(L, t, "x", v.x);
	setNumField(L, t, "y", v.y);
	setNumField(L, t, "z", v.z);
	lua_pop(L, 1);
}

void optNum(lua_State *L, int t, const char *k, double &out){
	lua_getfield(L, t, k);
	if (lua_isnumber(L, -1))
		out = lua_tonumber(L, -1);
	lua_pop(L, 1);
}

void optBool(lua_State *L, int t, const char *k, bool &out){
	lua_getfield(L, t, k);
	if (lua_type(L, -1) == LUA_TBOOLEAN)
		out = lua_toboolean(L, -1) != 0;
	lua_pop(L, 1);
}

// numero liga, false/nil explicito desliga (nil = "nao mexer")
void optOpt(lua_State *L, int t, const char *k, Opt &out){
	lua_getfield(L, t, k);
	if (lua_isnumber(L, -1)){
		out.has = true;
		out.v = lua_tonumber(L, -1);
	}
	else if (lua_type(L, -1) == LUA_TBOOLEAN && !lua_toboolean(L, -1))
		out.has = false;
	lua_pop(L, 1);
}

void setRef(lua_State *L, int &ref, int idx){
	if (ref != LUA_NOREF)
		luaL_unref(L, LUA_REGISTRYINDEX, ref);
	lua_pushvalue(L, idx);
	ref = luaL_ref(L, LUA_REGISTRYINDEX);
}

void clearRef(lua_State *L, int &ref){
	if (ref != LUA_NOREF)
		luaL_unref(L, LUA_REGISTRYINDEX, ref);
	ref = LUA_NOREF;
}

void parseBinding(lua_State *L, int idx, Binding &b){
	b.keys.clear();
	clearRef(L, b.fnRef);

	auto add = [&](const char *name){
		Keyboard::Key k;
		if (Keyboard::getConstant(name, k))
			b.keys.push_back(k);
	};

	int t = lua_type(L, idx);
	if (t == LUA_TSTRING)
		add(lua_tostring(L, idx));
	else if (t == LUA_TFUNCTION)
		setRef(L, b.fnRef, idx);
	else if (t == LUA_TTABLE){
		int n = (int) luax_objlen(L, idx);
		for (int i = 1; i <= n; i++){
			lua_rawgeti(L, idx, i);
			if (lua_type(L, -1) == LUA_TSTRING)
				add(lua_tostring(L, -1));
			lua_pop(L, 1);
		}
	}
}

int hookIndex(const char *name){
	for (int i = 0; i < H_COUNT; i++)
		if (std::strcmp(name, H_NAMES[i]) == 0)
			return i;
	return -1;
}

int eventIndex(const char *name){
	for (int i = 0; i < EV_COUNT; i++)
		if (std::strcmp(name, EV_NAMES[i]) == 0)
			return i;
	return -1;
}

void addListener(lua_State *L, Player *P, const char *event, int fn);

void applyConfig(lua_State *L, Player *P, int t, int self){
	Config &c = P->c;

	optNum(L, t, "radius", c.radius);
	optNum(L, t, "groundSpeed", c.groundSpeed);
	optNum(L, t, "walkFactor", c.walkFactor);
	optNum(L, t, "crouchFactor", c.crouchFactor);
	optOpt(L, t, "maxGroundSpeed", c.maxGroundSpeed);
	optOpt(L, t, "maxAirSpeed", c.maxAirSpeed);
	optNum(L, t, "groundAccel", c.groundAccel);
	optNum(L, t, "airAccel", c.airAccel);
	optOpt(L, t, "airCap", c.airCap);
	optNum(L, t, "friction", c.friction);
	optNum(L, t, "stopSpeed", c.stopSpeed);
	optNum(L, t, "gravity", c.gravity);
	optNum(L, t, "maxFall", c.maxFall);
	optBool(L, t, "canJump", c.canJump);
	optNum(L, t, "jumpSpeed", c.jumpSpeed);
	optNum(L, t, "jumpCooldown", c.jumpCooldown);
	optBool(L, t, "autoHop", c.autoHop);
	optOpt(L, t, "bhopSpeedCap", c.bhopSpeedCap);
	optBool(L, t, "canCrouch", c.canCrouch);
	optNum(L, t, "crouchDelay", c.crouchDelay);
	optNum(L, t, "uncrouchLift", c.uncrouchLift);
	optBool(L, t, "canWalk", c.canWalk);
	optBool(L, t, "canNoclip", c.canNoclip);
	optNum(L, t, "noclipSpeed", c.noclipSpeed);
	optNum(L, t, "noclipSlowFactor", c.noclipSlowFactor);
	optBool(L, t, "useCamera", c.useCamera);
	optBool(L, t, "controls", c.controls);

	lua_getfield(L, t, "fallDamage");
	if (lua_istable(L, -1)){
		int s = lua_gettop(L);
		optBool(L, s, "enabled", c.fdEnabled);
		optNum(L, s, "safeSpeed", c.fdSafe);
		optNum(L, s, "fatalSpeed", c.fdFatal);
		optNum(L, s, "maxDamage", c.fdMax);
	}
	lua_pop(L, 1);

	lua_getfield(L, t, "eye");
	if (lua_istable(L, -1)){
		int s = lua_gettop(L);
		optNum(L, s, "stand", c.eyeStand);
		optNum(L, s, "crouch", c.eyeCrouch);
		optNum(L, s, "smooth", c.eyeSmooth);
	}
	lua_pop(L, 1);

	lua_getfield(L, t, "bob");
	if (lua_istable(L, -1)){
		int s = lua_gettop(L);
		optBool(L, s, "enabled", c.bobEnabled);
		optNum(L, s, "amplitude", c.bobAmp);
		optNum(L, s, "frequency", c.bobFreq);
	}
	lua_pop(L, 1);

	lua_getfield(L, t, "landDip");
	if (lua_istable(L, -1)){
		int s = lua_gettop(L);
		optBool(L, s, "enabled", c.dipEnabled);
		optNum(L, s, "scale", c.dipScale);
		optNum(L, s, "max", c.dipMax);
		optNum(L, s, "recover", c.dipRecover);
	}
	lua_pop(L, 1);

	lua_getfield(L, t, "bindings");
	if (lua_istable(L, -1)){
		int s = lua_gettop(L);
		for (int i = 0; i < B_COUNT; i++){
			lua_getfield(L, s, B_NAMES[i]);
			if (!lua_isnil(L, -1))
				parseBinding(L, lua_gettop(L), P->bind[i]);
			lua_pop(L, 1);
		}
	}
	lua_pop(L, 1);

	for (int i = 0; i < H_COUNT; i++){
		lua_getfield(L, t, H_NAMES[i]);
		if (lua_isfunction(L, -1))
			setRef(L, P->hookRef[i], lua_gettop(L));
		lua_pop(L, 1);
	}

	lua_getfield(L, t, "callbacks");
	if (lua_istable(L, -1)){
		int s = lua_gettop(L);
		lua_pushnil(L);
		while (lua_next(L, s)){
			if (lua_type(L, -2) == LUA_TSTRING && lua_isfunction(L, -1))
				addListener(L, P, lua_tostring(L, -2), lua_gettop(L));
			lua_pop(L, 1);
		}
	}
	lua_pop(L, 1);

	(void) self;
}

Player *checkPlayer(lua_State *L, int i){
	return (Player *) luaL_checkudata(L, i, META);
}

// ---------------------------------------------------------------------------------------------
// eventos
// ---------------------------------------------------------------------------------------------

void addListener(lua_State *L, Player *P, const char *event, int fn){
	fn = (fn < 0) ? lua_gettop(L) + fn + 1 : fn;

	lua_rawgeti(L, LUA_REGISTRYINDEX, P->listenersRef);
	lua_getfield(L, -1, event);
	if (!lua_istable(L, -1)){
		lua_pop(L, 1);
		lua_newtable(L);
		lua_pushvalue(L, -1);
		lua_setfield(L, -3, event);
	}
	lua_pushvalue(L, fn);
	lua_rawseti(L, -2, (int) luax_objlen(L, -2) + 1);
	lua_pop(L, 2);

	int ev = eventIndex(event);
	if (ev >= 0)
		P->listenerCount[ev]++;
}

// chama os listeners de um evento conhecido com ate 2 numeros
void emit(lua_State *L, Player *P, int self, int ev, int nargs, double a, double b){
	if (P->listenerCount[ev] <= 0)
		return;

	lua_rawgeti(L, LUA_REGISTRYINDEX, P->listenersRef);
	lua_getfield(L, -1, EV_NAMES[ev]);
	if (lua_istable(L, -1)){
		int list = lua_gettop(L);
		int n = (int) luax_objlen(L, list);
		for (int i = 1; i <= n; i++){
			lua_rawgeti(L, list, i);
			if (!lua_isfunction(L, -1)){
				lua_pop(L, 1);
				continue;
			}
			lua_pushvalue(L, self);
			if (nargs > 0) lua_pushnumber(L, a);
			if (nargs > 1) lua_pushnumber(L, b);
			lua_call(L, 1 + nargs, 0);
		}
	}
	lua_pop(L, 2);
}

void queue(Player *P, int ev, double a = 0, double b = 0){
	if (P->pendingCount < 16)
		P->pending[P->pendingCount++] = {ev, a, b};
}

void flush(lua_State *L, Player *P, int self){
	for (int i = 0; i < P->pendingCount; i++){
		Pending &p = P->pending[i];
		int nargs = 0;
		if (p.ev == EV_LAND) nargs = 1;
		else if (p.ev == EV_FALLDAMAGE) nargs = 2;
		else if (p.ev == EV_NOCLIP) nargs = 1;
		emit(L, P, self, p.ev, nargs, p.a, p.b);
	}
	P->pendingCount = 0;
}

// ---------------------------------------------------------------------------------------------
// input
// ---------------------------------------------------------------------------------------------

bool pressed(lua_State *L, Player *P, int self, int b){
	Binding &bd = P->bind[b];

	if (!bd.keys.empty()){
		auto *kb = Module::getInstance<Keyboard>(Module::M_KEYBOARD);
		if (kb && kb->isDown(bd.keys))
			return true;
	}
	if (bd.fnRef != LUA_NOREF){
		lua_rawgeti(L, LUA_REGISTRYINDEX, bd.fnRef);
		lua_pushvalue(L, self);
		lua_call(L, 1, 1);
		bool r = lua_toboolean(L, -1) != 0;
		lua_pop(L, 1);
		return r;
	}
	return false;
}

void readInput(lua_State *L, Player *P, int self){
	Input &in = P->in;

	if (!P->c.controls){
		in = Input();
		return;
	}

	if (P->hookRef[H_GETINPUT] != LUA_NOREF){
		lua_rawgeti(L, LUA_REGISTRYINDEX, P->inputRef);
		int t = lua_gettop(L);
		setNumField(L, t, "forward", in.forward);
		setNumField(L, t, "side", in.side);
		lua_pushboolean(L, in.jump); lua_setfield(L, t, "jump");
		lua_pushboolean(L, in.crouch); lua_setfield(L, t, "crouch");
		lua_pushboolean(L, in.walk); lua_setfield(L, t, "walk");
		lua_pushboolean(L, in.noclip); lua_setfield(L, t, "noclip");

		lua_rawgeti(L, LUA_REGISTRYINDEX, P->hookRef[H_GETINPUT]);
		lua_pushvalue(L, self);
		lua_pushvalue(L, t);
		lua_call(L, 2, 1);
		int r = lua_istable(L, -1) ? lua_gettop(L) : t;

		in.forward = numField(L, r, "forward");
		in.side = numField(L, r, "side");
		lua_getfield(L, r, "jump"); in.jump = lua_toboolean(L, -1) != 0; lua_pop(L, 1);
		lua_getfield(L, r, "crouch"); in.crouch = lua_toboolean(L, -1) != 0; lua_pop(L, 1);
		lua_getfield(L, r, "walk"); in.walk = lua_toboolean(L, -1) != 0; lua_pop(L, 1);
		lua_getfield(L, r, "noclip"); in.noclip = lua_toboolean(L, -1) != 0; lua_pop(L, 1);

		lua_settop(L, t - 1);
		return;
	}

	in.forward = (pressed(L, P, self, B_FORWARD) ? 1 : 0) - (pressed(L, P, self, B_BACK) ? 1 : 0);
	in.side = (pressed(L, P, self, B_RIGHT) ? 1 : 0) - (pressed(L, P, self, B_LEFT) ? 1 : 0);
	in.jump = pressed(L, P, self, B_JUMP);
	in.crouch = pressed(L, P, self, B_CROUCH);
	in.walk = pressed(L, P, self, B_WALK);
	in.noclip = pressed(L, P, self, B_NOCLIP);
}

double getYaw(lua_State *L, Player *P, int self){
	if (P->hookRef[H_GETYAW] != LUA_NOREF){
		lua_rawgeti(L, LUA_REGISTRYINDEX, P->hookRef[H_GETYAW]);
		lua_pushvalue(L, self);
		lua_call(L, 1, 1);
		double y = lua_tonumber(L, -1);
		lua_pop(L, 1);
		return y;
	}
	return MGOLD_cameraYaw();
}

// ---------------------------------------------------------------------------------------------
// passos do movimento
// ---------------------------------------------------------------------------------------------

void getWish(lua_State *L, Player *P, int self, double &wx, double &wz, double &mag){
	double fwd = P->in.forward, side = P->in.side;

	if (P->hookRef[H_GETWISH] != LUA_NOREF){
		lua_rawgeti(L, LUA_REGISTRYINDEX, P->hookRef[H_GETWISH]);
		lua_pushvalue(L, self);
		lua_pushnumber(L, fwd);
		lua_pushnumber(L, side);
		lua_call(L, 3, 3);
		wx = lua_tonumber(L, -3);
		wz = lua_tonumber(L, -2);
		mag = lua_tonumber(L, -1);
		lua_pop(L, 3);
		return;
	}

	double len = std::sqrt(fwd * fwd + side * side);
	if (len == 0){
		wx = wz = mag = 0;
		return;
	}
	if (len > 1){
		fwd /= len;
		side /= len;
		len = 1;
	}
	double yaw = getYaw(L, P, self);
	double s = std::sin(yaw), c = std::cos(yaw);
	wx = s * fwd - c * side;
	wz = c * fwd + s * side;
	mag = len;
}

void applyFriction(lua_State *L, Player *P, int self, int body, V3 &v, double dt){
	if (P->hookRef[H_FRICTION] != LUA_NOREF){
		writeV3(L, body, "speed", v);
		lua_rawgeti(L, LUA_REGISTRYINDEX, P->hookRef[H_FRICTION]);
		lua_pushvalue(L, self);
		lua_pushnumber(L, dt);
		lua_call(L, 2, 0);
		readV3(L, body, "speed", v);
		return;
	}

	double speed = std::sqrt(v.x * v.x + v.z * v.z);
	if (speed < 0.001){
		v.x = v.z = 0;
		return;
	}
	double control = std::fmax(speed, P->c.stopSpeed);
	double newSpeed = std::fmax(0.0, speed - control * P->c.friction * dt);
	double k = newSpeed / speed;
	v.x *= k;
	v.z *= k;
}

void accelerate(lua_State *L, Player *P, int self, int body, V3 &v, double wx, double wz, double wishSpeed,
                double accel, double dt, const Opt &cap){
	if (P->hookRef[H_ACCELERATE] != LUA_NOREF){
		writeV3(L, body, "speed", v);
		lua_rawgeti(L, LUA_REGISTRYINDEX, P->hookRef[H_ACCELERATE]);
		lua_pushvalue(L, self);
		lua_pushnumber(L, wx);
		lua_pushnumber(L, wz);
		lua_pushnumber(L, wishSpeed);
		lua_pushnumber(L, accel);
		lua_pushnumber(L, dt);
		if (cap.has) lua_pushnumber(L, cap.v); else lua_pushnil(L);
		lua_call(L, 7, 0);
		readV3(L, body, "speed", v);
		return;
	}

	double current = v.x * wx + v.z * wz;
	double target = cap.has ? std::fmin(wishSpeed, cap.v) : wishSpeed;
	double add = target - current;
	if (add <= 0)
		return;
	double amount = std::fmin(accel * dt * wishSpeed, add);
	v.x += wx * amount;
	v.z += wz * amount;
}

void setBodyShape(lua_State *L, int body, int shapeRef){
	lua_rawgeti(L, LUA_REGISTRYINDEX, shapeRef);
	lua_setfield(L, body, "shape");
}

void updateCrouch(lua_State *L, Player *P, int body, V3 &pos, double dt){
	Config &c = P->c;
	P->crouchTimer -= dt;
	bool want = P->in.crouch && c.canCrouch;

	if (want && !P->crouching){
		if (P->crouchTimer <= 0){
			P->crouching = true;
			P->crouchTimer = c.crouchDelay;
			setBodyShape(L, body, P->shapeCrouchRef);
			queue(P, EV_CROUCH);
		}
	}
	else if (!want && P->crouching){
		P->crouching = false;
		setBodyShape(L, body, P->shapeStandRef);
		pos.y += c.uncrouchLift;
		queue(P, EV_UNCROUCH);
	}
}

void updateNoclip(lua_State *L, Player *P, int self, V3 &pos, V3 &vel, double dt){
	Config &c = P->c;
	Input &in = P->in;

	double lx, ly, lz;
	MGOLD_cameraLookVector(lx, ly, lz);
	double yaw = getYaw(L, P, self);
	double rx = -std::cos(yaw), rz = std::sin(yaw);

	double speed = c.noclipSpeed * ((in.walk && c.canWalk) ? c.noclipSlowFactor : 1.0);
	double up = (in.jump ? 1 : 0) - (in.crouch ? 1 : 0);

	double mx = lx * in.forward + rx * in.side;
	double my = ly * in.forward + up;
	double mz = lz * in.forward + rz * in.side;

	double len = std::sqrt(mx * mx + my * my + mz * mz);
	if (len > 1){
		mx /= len; my /= len; mz /= len;
	}

	pos.x += mx * speed * dt;
	pos.y += my * speed * dt;
	pos.z += mz * speed * dt;
	vel = {0, 0, 0};

	P->moving = len > 0;
}

void updateGround(lua_State *L, Player *P, int self, int body, V3 &vel, bool onGround, double dt){
	Config &c = P->c;
	Input &in = P->in;

	double wx, wz, mag;
	getWish(L, P, self, wx, wz, mag);

	double factor = 1;
	if (P->crouching)
		factor = c.crouchFactor;
	else if (in.walk && c.canWalk)
		factor = c.walkFactor;
	double wishSpeed = c.groundSpeed * factor * mag;

	bool grounded = onGround;

	P->jumpTimer -= dt;
	bool wantJump = in.jump && c.canJump;

	if (wantJump && grounded && P->jumpTimer <= 0 && (c.autoHop || !P->jumpHeld)){
		if (c.bhopSpeedCap.has){
			double hs = std::sqrt(vel.x * vel.x + vel.z * vel.z);
			if (hs > c.bhopSpeedCap.v){
				double k = c.bhopSpeedCap.v / hs;
				vel.x *= k;
				vel.z *= k;
			}
		}
		vel.y = c.jumpSpeed;
		P->jumpTimer = c.jumpCooldown;
		grounded = false;
		queue(P, EV_JUMP);
	}
	P->jumpHeld = wantJump;

	// no frame do pulo a friccao e pulada (permite bhop)
	if (grounded){
		applyFriction(L, P, self, body, vel, dt);
		if (wishSpeed > 0)
			accelerate(L, P, self, body, vel, wx, wz, wishSpeed, c.groundAccel, dt, Opt{false, 0});
	}
	else if (wishSpeed > 0)
		accelerate(L, P, self, body, vel, wx, wz, wishSpeed, c.airAccel, dt, c.airCap);

	const Opt &limit = grounded ? c.maxGroundSpeed : c.maxAirSpeed;
	if (limit.has){
		double hs = std::sqrt(vel.x * vel.x + vel.z * vel.z);
		if (hs > limit.v){
			double k = limit.v / hs;
			vel.x *= k;
			vel.z *= k;
		}
	}

	P->moving = wishSpeed > 0;
}

void handleLanding(Player *P, double impact){
	Config &c = P->c;
	queue(P, EV_LAND, impact);

	if (c.fdEnabled && impact > c.fdSafe){
		double t = std::fmin(1.0, (impact - c.fdSafe) / std::fmax(0.0001, c.fdFatal - c.fdSafe));
		queue(P, EV_FALLDAMAGE, t * c.fdMax, impact);
	}

	if (c.dipEnabled)
		P->dip = std::fmin(c.dipMax, P->dip + impact * c.dipScale);
}

// ---------------------------------------------------------------------------------------------
// update principal
// ---------------------------------------------------------------------------------------------

int w_update(lua_State *L){
	Player *P = checkPlayer(L, 1);
	double dt = luaL_checknumber(L, 2);
	Config &c = P->c;

	lua_rawgeti(L, LUA_REGISTRYINDEX, P->bodyRef);
	int body = lua_gettop(L);

	lua_getfield(L, body, "unsetStatic");
	lua_pushvalue(L, body);
	lua_call(L, 1, 0);

	readInput(L, P, 1);

	if (c.canNoclip && P->in.noclip && !P->noclipHeld){
		P->noclip = !P->noclip;
		queue(P, EV_NOCLIP, P->noclip ? 1 : 0);
		V3 zero = {0, 0, 0};
		writeV3(L, body, "speed", zero);
	}
	P->noclipHeld = P->in.noclip;

	bool wasMoving = P->moving;

	V3 pos, vel;
	readV3(L, body, "position", pos);
	readV3(L, body, "speed", vel);

	lua_getfield(L, body, "onGround");
	bool wasGround = lua_toboolean(L, -1) != 0;
	lua_pop(L, 1);

	double preFall = vel.y;
	bool onGround = wasGround;

	if (P->noclip){
		updateNoclip(L, P, 1, pos, vel, dt);
		writeV3(L, body, "position", pos);
		writeV3(L, body, "speed", vel);
	}
	else{
		updateCrouch(L, P, body, pos, dt);
		updateGround(L, P, 1, body, vel, wasGround, dt);
		preFall = vel.y;

		writeV3(L, body, "position", pos);
		writeV3(L, body, "speed", vel);

		lua_getfield(L, body, "updatePhysics");
		lua_pushvalue(L, body);
		lua_pushnumber(L, dt);
		lua_pushnumber(L, c.gravity);
		lua_pushnumber(L, c.maxFall);
		lua_call(L, 4, 0);

		lua_getfield(L, body, "onGround");
		onGround = lua_toboolean(L, -1) != 0;
		lua_pop(L, 1);

		if (onGround){
			P->timeOnAir = 0;
			if (!wasGround && preFall < 0)
				handleLanding(P, -preFall);
		}
		else
			P->timeOnAir += dt;
	}

	if (P->moving != wasMoving)
		queue(P, P->moving ? EV_START : EV_STOP);

	flush(L, P, 1); // listeners podem mexer no body (teleport...): rele depois

	// ---- camera ----
	readV3(L, body, "position", pos);
	readV3(L, body, "speed", vel);

	double target = P->crouching ? c.eyeCrouch : c.eyeStand;
	P->eyeOffset += (target - P->eyeOffset) * std::fmin(1.0, c.eyeSmooth * dt);

	double bobY = 0;
	if (c.bobEnabled){
		double hs = std::sqrt(vel.x * vel.x + vel.z * vel.z);
		bool active = onGround && !P->noclip && hs > 0.1;
		double goal = active ? std::fmin(1.0, hs / c.groundSpeed) : 0.0;
		P->bobAmount += (goal - P->bobAmount) * std::fmin(1.0, 10 * dt);
		if (active)
			P->bobPhase = std::fmod(P->bobPhase + hs * c.bobFreq * dt, 1.0);
		bobY = std::sin(P->bobPhase * TAU) * c.bobAmp * P->bobAmount;
	}

	if (P->dip > 0)
		P->dip = std::fmax(0.0, P->dip - P->dip * std::fmin(1.0, c.dipRecover * dt) - 0.0005);

	if (c.useCamera)
		MGOLD_cameraSetPosition(pos.x, pos.y + P->eyeOffset + bobY - P->dip, pos.z);

	emit(L, P, 1, EV_UPDATE, 1, dt, 0);

	lua_settop(L, 2);
	return 0;
}

// ---------------------------------------------------------------------------------------------
// criacao / gc / index
// ---------------------------------------------------------------------------------------------

void pushPhysics(lua_State *L, const char *name){
	lua_getglobal(L, "love");
	lua_getfield(L, -1, "mgold");
	lua_getfield(L, -1, "physics");
	lua_getfield(L, -1, name);
	lua_remove(L, -2);
	lua_remove(L, -2);
	lua_remove(L, -2);
}

// referencia para config[key] (se for tabela de config e tiver valor) ou physics.shapes[fallback]
int makeShapeRef(lua_State *L, int cfg, const char *key, const char *fallback){
	if (lua_istable(L, cfg)){
		lua_getfield(L, cfg, key);
		if (!lua_isnil(L, -1))
			return luaL_ref(L, LUA_REGISTRYINDEX);
		lua_pop(L, 1);
	}
	lua_getglobal(L, "love");
	lua_getfield(L, -1, "mgold");
	lua_getfield(L, -1, "physics");
	lua_getfield(L, -1, "shapes");
	lua_getfield(L, -1, fallback);
	lua_replace(L, -5);
	lua_pop(L, 3);
	return luaL_ref(L, LUA_REGISTRYINDEX);
}

// Player.new(x, y, z, config)  /  Player:new(x, y, z, config)
int w_new(lua_State *L){
	// "Player:new(...)" passa a propria tabela primeiro: se arg1 e tabela, desloca
	int a = lua_istable(L, 1) ? 2 : 1;
	double x = luaL_checknumber(L, a), y = luaL_checknumber(L, a + 1), z = luaL_checknumber(L, a + 2);
	int cfg = a + 3;

	Player *P = (Player *) lua_newuserdata(L, sizeof(Player));
	new (P) Player();
	int self = lua_gettop(L);

	P->bodyRef = P->shapeStandRef = P->shapeCrouchRef = LUA_NOREF;
	P->listenersRef = P->userRef = P->inputRef = LUA_NOREF;
	for (int i = 0; i < H_COUNT; i++) P->hookRef[i] = LUA_NOREF;
	for (int i = 0; i < EV_COUNT; i++) P->listenerCount[i] = 0;
	for (int i = 0; i < B_COUNT; i++) P->bind[i].fnRef = LUA_NOREF;

	P->noclip = P->noclipHeld = P->crouching = P->jumpHeld = P->moving = false;
	P->crouchTimer = P->jumpTimer = P->timeOnAir = 0;
	P->bobPhase = P->bobAmount = P->dip = 0;
	P->pendingCount = 0;

	lua_rawgeti(L, LUA_REGISTRYINDEX, metaRef);
	lua_setmetatable(L, self);

	lua_newtable(L); P->listenersRef = luaL_ref(L, LUA_REGISTRYINDEX);
	lua_newtable(L); P->userRef = luaL_ref(L, LUA_REGISTRYINDEX);
	lua_newtable(L); P->inputRef = luaL_ref(L, LUA_REGISTRYINDEX);

	// bindings padrao
	const char *defaults[B_COUNT] = {"w", "s", "a", "d", "space", "lctrl", "lshift", "v"};
	for (int i = 0; i < B_COUNT; i++){
		lua_pushstring(L, defaults[i]);
		parseBinding(L, lua_gettop(L), P->bind[i]);
		lua_pop(L, 1);
	}

	if (lua_istable(L, cfg))
		applyConfig(L, P, cfg, self);

	// formas (config.shape / config.crouchShape; nil = capsula / esfera)
	P->shapeStandRef = makeShapeRef(L, cfg, "shape", "capsule");
	P->shapeCrouchRef = makeShapeRef(L, cfg, "crouchShape", "sphere");

	// corpo
	pushPhysics(L, "newBody");
	lua_pushnumber(L, x);
	lua_pushnumber(L, y);
	lua_pushnumber(L, z);
	lua_pushnumber(L, P->c.radius);
	lua_rawgeti(L, LUA_REGISTRYINDEX, P->shapeStandRef);
	lua_call(L, 5, 1);
	lua_pushboolean(L, 1);
	lua_setfield(L, -2, "manualUpdate");
	P->bodyRef = luaL_ref(L, LUA_REGISTRYINDEX);

	P->eyeOffset = P->c.eyeStand;

	lua_pushvalue(L, self);
	return 1;
}

int w_gc(lua_State *L){
	Player *P = (Player *) lua_touserdata(L, 1);
	if (!P)
		return 0;
	clearRef(L, P->bodyRef);
	clearRef(L, P->shapeStandRef);
	clearRef(L, P->shapeCrouchRef);
	clearRef(L, P->listenersRef);
	clearRef(L, P->userRef);
	clearRef(L, P->inputRef);
	for (int i = 0; i < H_COUNT; i++) clearRef(L, P->hookRef[i]);
	for (int i = 0; i < B_COUNT; i++) clearRef(L, P->bind[i].fnRef);
	P->~Player();
	return 0;
}

int w_index(lua_State *L){
	Player *P = checkPlayer(L, 1);

	if (lua_type(L, 2) == LUA_TSTRING){
		const char *k = lua_tostring(L, 2);

		lua_rawgeti(L, LUA_REGISTRYINDEX, methodsRef);
		lua_pushvalue(L, 2);
		lua_rawget(L, -2);
		if (!lua_isnil(L, -1))
			return 1;
		lua_pop(L, 2);

		if (std::strcmp(k, "body") == 0){
			lua_rawgeti(L, LUA_REGISTRYINDEX, P->bodyRef);
			return 1;
		}
		if (std::strcmp(k, "timeOnAir") == 0){
			lua_pushnumber(L, P->timeOnAir);
			return 1;
		}
	}

	lua_rawgeti(L, LUA_REGISTRYINDEX, P->userRef);
	lua_pushvalue(L, 2);
	lua_rawget(L, -2);
	return 1;
}

int w_newindex(lua_State *L){
	Player *P = checkPlayer(L, 1);
	lua_rawgeti(L, LUA_REGISTRYINDEX, P->userRef);
	lua_pushvalue(L, 2);
	lua_pushvalue(L, 3);
	lua_rawset(L, -3);
	return 0;
}

// ---------------------------------------------------------------------------------------------
// metodos
// ---------------------------------------------------------------------------------------------

int w_on(lua_State *L){
	Player *P = checkPlayer(L, 1);
	const char *ev = luaL_checkstring(L, 2);
	luaL_checktype(L, 3, LUA_TFUNCTION);
	addListener(L, P, ev, 3);
	lua_pushvalue(L, 3);
	return 1;
}

int w_off(lua_State *L){
	Player *P = checkPlayer(L, 1);
	const char *ev = luaL_checkstring(L, 2);
	luaL_checktype(L, 3, LUA_TFUNCTION);

	lua_rawgeti(L, LUA_REGISTRYINDEX, P->listenersRef);
	lua_getfield(L, -1, ev);
	if (lua_istable(L, -1)){
		int list = lua_gettop(L);
		int n = (int) luax_objlen(L, list);
		int evi = eventIndex(ev);
		for (int i = n; i >= 1; i--){
			lua_rawgeti(L, list, i);
			bool same = lua_rawequal(L, -1, 3) != 0;
			lua_pop(L, 1);
			if (same){
				for (int j = i; j < n; j++){
					lua_rawgeti(L, list, j + 1);
					lua_rawseti(L, list, j);
				}
				lua_pushnil(L);
				lua_rawseti(L, list, n);
				n--;
				if (evi >= 0)
					P->listenerCount[evi]--;
			}
		}
	}
	return 0;
}

// player:emit("meuEvento", ...)  -- dispara evento (inclusive customizado)
int w_emit(lua_State *L){
	Player *P = checkPlayer(L, 1);
	const char *ev = luaL_checkstring(L, 2);
	int nargs = lua_gettop(L) - 2;

	lua_rawgeti(L, LUA_REGISTRYINDEX, P->listenersRef);
	lua_getfield(L, -1, ev);
	if (lua_istable(L, -1)){
		int list = lua_gettop(L);
		int n = (int) luax_objlen(L, list);
		for (int i = 1; i <= n; i++){
			lua_rawgeti(L, list, i);
			lua_pushvalue(L, 1);
			for (int k = 0; k < nargs; k++)
				lua_pushvalue(L, 3 + k);
			lua_call(L, 1 + nargs, 0);
		}
	}
	return 0;
}

int w_setConfig(lua_State *L){
	Player *P = checkPlayer(L, 1);
	luaL_checktype(L, 2, LUA_TTABLE);
	applyConfig(L, P, 2, 1);

	lua_getfield(L, 2, "shape");
	if (!lua_isnil(L, -1)){
		clearRef(L, P->shapeStandRef);
		P->shapeStandRef = luaL_ref(L, LUA_REGISTRYINDEX);
	}
	else
		lua_pop(L, 1);

	lua_getfield(L, 2, "crouchShape");
	if (!lua_isnil(L, -1)){
		clearRef(L, P->shapeCrouchRef);
		P->shapeCrouchRef = luaL_ref(L, LUA_REGISTRYINDEX);
	}
	else
		lua_pop(L, 1);
	return 0;
}

int w_setBinding(lua_State *L){
	Player *P = checkPlayer(L, 1);
	const char *name = luaL_checkstring(L, 2);
	for (int i = 0; i < B_COUNT; i++){
		if (std::strcmp(name, B_NAMES[i]) == 0){
			parseBinding(L, 3, P->bind[i]);
			return 0;
		}
	}
	return luaL_error(L, "unknown binding: %s", name);
}

int w_setHook(lua_State *L){
	Player *P = checkPlayer(L, 1);
	const char *name = luaL_checkstring(L, 2);
	int h = hookIndex(name);
	if (h < 0)
		return luaL_error(L, "unknown hook: %s", name);
	if (lua_isfunction(L, 3))
		setRef(L, P->hookRef[h], 3);
	else
		clearRef(L, P->hookRef[h]);
	return 0;
}

int w_setControlsEnabled(lua_State *L){
	checkPlayer(L, 1)->c.controls = lua_toboolean(L, 2) != 0;
	return 0;
}

int pushBodyV3(lua_State *L, Player *P, const char *key, double dy){
	lua_rawgeti(L, LUA_REGISTRYINDEX, P->bodyRef);
	int body = lua_gettop(L);
	V3 v;
	readV3(L, body, key, v);
	lua_pushnumber(L, v.x);
	lua_pushnumber(L, v.y + dy);
	lua_pushnumber(L, v.z);
	return 3;
}

int w_getPosition(lua_State *L){ return pushBodyV3(L, checkPlayer(L, 1), "position", 0); }
int w_getEyePosition(lua_State *L){
	Player *P = checkPlayer(L, 1);
	return pushBodyV3(L, P, "position", P->eyeOffset - P->dip);
}
int w_getVelocity(lua_State *L){ return pushBodyV3(L, checkPlayer(L, 1), "speed", 0); }

int w_getHorizontalSpeed(lua_State *L){
	Player *P = checkPlayer(L, 1);
	lua_rawgeti(L, LUA_REGISTRYINDEX, P->bodyRef);
	V3 v;
	readV3(L, lua_gettop(L), "speed", v);
	lua_pushnumber(L, std::sqrt(v.x * v.x + v.z * v.z));
	return 1;
}

int w_isOnGround(lua_State *L){
	Player *P = checkPlayer(L, 1);
	lua_rawgeti(L, LUA_REGISTRYINDEX, P->bodyRef);
	lua_getfield(L, -1, "onGround");
	lua_pushboolean(L, lua_toboolean(L, -1));
	return 1;
}

int w_isCrouching(lua_State *L){ lua_pushboolean(L, checkPlayer(L, 1)->crouching); return 1; }
int w_isNoclip(lua_State *L){ lua_pushboolean(L, checkPlayer(L, 1)->noclip); return 1; }
int w_isMoving(lua_State *L){ lua_pushboolean(L, checkPlayer(L, 1)->moving); return 1; }

int w_setPosition(lua_State *L){
	Player *P = checkPlayer(L, 1);
	lua_rawgeti(L, LUA_REGISTRYINDEX, P->bodyRef);
	V3 v = {luaL_checknumber(L, 2), luaL_checknumber(L, 3), luaL_checknumber(L, 4)};
	writeV3(L, lua_gettop(L), "position", v);
	return 0;
}

int w_setVelocity(lua_State *L){
	Player *P = checkPlayer(L, 1);
	lua_rawgeti(L, LUA_REGISTRYINDEX, P->bodyRef);
	V3 v = {luaL_checknumber(L, 2), luaL_checknumber(L, 3), luaL_checknumber(L, 4)};
	writeV3(L, lua_gettop(L), "speed", v);
	return 0;
}

int w_addVelocity(lua_State *L){
	Player *P = checkPlayer(L, 1);
	lua_rawgeti(L, LUA_REGISTRYINDEX, P->bodyRef);
	int body = lua_gettop(L);
	V3 v;
	readV3(L, body, "speed", v);
	v.x += luaL_checknumber(L, 2);
	v.y += luaL_checknumber(L, 3);
	v.z += luaL_checknumber(L, 4);
	writeV3(L, body, "speed", v);
	return 0;
}

int w_teleport(lua_State *L){
	Player *P = checkPlayer(L, 1);
	double x = luaL_checknumber(L, 2), y = luaL_checknumber(L, 3), z = luaL_checknumber(L, 4);
	lua_rawgeti(L, LUA_REGISTRYINDEX, P->bodyRef);
	int body = lua_gettop(L);
	V3 p = {x, y, z}, zero = {0, 0, 0};
	writeV3(L, body, "position", p);
	writeV3(L, body, "speed", zero);
	lua_pop(L, 1);
	emit(L, P, 1, EV_TELEPORT, 0, 0, 0); // sem args extras; leia a posicao com getPosition
	return 0;
}

int w_setNoclip(lua_State *L){
	Player *P = checkPlayer(L, 1);
	bool flag = lua_toboolean(L, 2) != 0;
	if (P->noclip == flag)
		return 0;
	P->noclip = flag;

	lua_rawgeti(L, LUA_REGISTRYINDEX, P->bodyRef);
	V3 zero = {0, 0, 0};
	writeV3(L, lua_gettop(L), "speed", zero);
	lua_pop(L, 1);

	emit(L, P, 1, EV_NOCLIP, 1, flag ? 1 : 0, 0);
	return 0;
}

int w_toggleNoclip(lua_State *L){
	Player *P = checkPlayer(L, 1);
	lua_pushboolean(L, !P->noclip);
	lua_replace(L, 2);
	lua_settop(L, 2);
	return w_setNoclip(L);
}

} // namespace

void MGOLD_setPlayerFunc(lua_State *L){
	static const luaL_Reg methods[] = {
		{"update", w_update},
		{"on", w_on},
		{"off", w_off},
		{"emit", w_emit},
		{"setConfig", w_setConfig},
		{"setBinding", w_setBinding},
		{"setHook", w_setHook},
		{"setControlsEnabled", w_setControlsEnabled},
		{"getPosition", w_getPosition},
		{"getEyePosition", w_getEyePosition},
		{"getVelocity", w_getVelocity},
		{"getHorizontalSpeed", w_getHorizontalSpeed},
		{"isOnGround", w_isOnGround},
		{"isCrouching", w_isCrouching},
		{"isNoclip", w_isNoclip},
		{"isMoving", w_isMoving},
		{"setPosition", w_setPosition},
		{"setVelocity", w_setVelocity},
		{"addVelocity", w_addVelocity},
		{"teleport", w_teleport},
		{"setNoclip", w_setNoclip},
		{"toggleNoclip", w_toggleNoclip},
		{nullptr, nullptr}
	};

	lua_newtable(L);
	for (const luaL_Reg *r = methods; r->name; r++){
		lua_pushcfunction(L, r->func);
		lua_setfield(L, -2, r->name);
	}
	methodsRef = luaL_ref(L, LUA_REGISTRYINDEX);

	luaL_newmetatable(L, META);
	lua_pushcfunction(L, w_index); lua_setfield(L, -2, "__index");
	lua_pushcfunction(L, w_newindex); lua_setfield(L, -2, "__newindex");
	lua_pushcfunction(L, w_gc); lua_setfield(L, -2, "__gc");
	lua_pushvalue(L, -1);
	metaRef = luaL_ref(L, LUA_REGISTRYINDEX);
	lua_pop(L, 1);

	lua_newtable(L); // love.mgold.player
	lua_pushcfunction(L, w_new);
	lua_setfield(L, -2, "new");
}

}
}