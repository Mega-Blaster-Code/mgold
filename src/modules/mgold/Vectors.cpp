#include "modules/mgold/Vectors.h"

#include <cmath>
#include <cstdio>
#include <string>

namespace love
{

namespace mgold
{

// ---------------------------------------------------------------------------------------------
// Os vetores continuam sendo TABELAS com campos crus (x, y, z, w / r, g, b, a):
//   v.x            -> acesso direto da VM (nenhum codigo C roda)
//   v[1]..v[n]     -> __index / __newindex (so quando a chave nao existe na tabela)
//
// Otimizacoes desta versao:
//  * as funcoes sao templates em N (2, 3, 4): os loops sao desenrolados em tempo de compilacao
//  * os nomes dos campos ("x", "y"...) sao strings internadas UMA vez e guardadas como UPVALUES
//    dos closures. Antes cada lua_getfield/lua_setfield fazia strlen + hash + busca no intern
//    table; agora e so lua_pushvalue + lua_rawget/lua_rawset
//  * a classe (metatable) tambem e upvalue: nada de lua_getfield(REGISTRY, "nome") por vetor criado
//  * lua_rawget/lua_rawset no lugar de getfield/setfield (nunca disparam metamethods)
//  * operadores nao consultam metatable para numeros e nao fazem nenhuma alocacao alem do
//    resultado; as versoes in-place (add/sub/mul/div/set) nao alocam nada
//  * o pool de temporarios e acessado por referencia inteira (luaL_ref), sem string
// ---------------------------------------------------------------------------------------------

static const int POOL_SIZE = 1024;

struct VecType{
	const char *name;      // usado no __tostring
	const char *newName;   // vec2, vec3...
	const char *isName;    // isVec2, isVec3...
	const char *tempName;  // tvec2, tvec3...
	const char *fields[4];
	int n;
	bool alphaLast;        // rgba: o ultimo componente tem padrao 1 em vez de copiar o anterior
	int poolRef;
	int classRef;
	int tempIndex;
};

static VecType TYPES[4] = {
	{"vec2", "vec2", "isVec2", "tvec2", {"x", "y", "", ""}, 2, false, LUA_NOREF, LUA_NOREF, 0},
	{"vec3", "vec3", "isVec3", "tvec3", {"x", "y", "z", ""}, 3, false, LUA_NOREF, LUA_NOREF, 0},
	{"vec4", "vec4", "isVec4", "tvec4", {"x", "y", "z", "w"}, 4, false, LUA_NOREF, LUA_NOREF, 0},
	{"rgba", "rgba", "isRgba", "trgba", {"r", "g", "b", "a"}, 4, true, LUA_NOREF, LUA_NOREF, 0},
};

// layout dos upvalues de TODO closure de vetor:
//   1 = VecType*, 2 = classe (metatable), 3.. = chaves dos campos (strings internadas)
#define UP_TYPE lua_upvalueindex(1)
#define UP_CLASS lua_upvalueindex(2)
#define UP_KEY(i) lua_upvalueindex(3 + (i))

enum Op{ OP_ADD, OP_SUB, OP_MUL, OP_DIV };

template<int OP> static inline double apply(double a, double b){
	switch (OP){
		case OP_ADD: return a + b;
		case OP_SUB: return a - b;
		case OP_MUL: return a * b;
		default: return a / b;
	}
}

// idx precisa ser absoluto (positivo)
template<int N> static inline void readComps(lua_State *L, int idx, double *out){
	for (int i = 0; i < N; i++){
		lua_pushvalue(L, UP_KEY(i));
		lua_rawget(L, idx);
	}
	for (int i = 0; i < N; i++)
		out[i] = lua_tonumber(L, -N + i);
	lua_pop(L, N);
}

template<int N> static inline void writeComps(lua_State *L, int idx, const double *v){
	for (int i = 0; i < N; i++){
		lua_pushvalue(L, UP_KEY(i));
		lua_pushnumber(L, v[i]);
		lua_rawset(L, idx);
	}
}

template<int N> static inline void pushVec(lua_State *L, const double *v){
	lua_createtable(L, 0, N);
	for (int i = 0; i < N; i++){
		lua_pushvalue(L, UP_KEY(i));
		lua_pushnumber(L, v[i]);
		lua_rawset(L, -3);
	}
	lua_pushvalue(L, UP_CLASS);
	lua_setmetatable(L, -2);
}

static inline bool isVec(lua_State *L, int idx){
	if (lua_type(L, idx) == LUA_TNONE || !lua_getmetatable(L, idx))
		return false;
	bool same = lua_rawequal(L, -1, UP_CLASS) != 0;
	lua_pop(L, 1);
	return same;
}

// regras de preenchimento do original: y = y or x, z = z or y, w = w or z (rgba: a = a or 1)
template<int N> static inline void readCtorArgs(lua_State *L, const VecType *T, int first, double *out){
	for (int i = 0; i < N; i++){
		int idx = first + i;
		if (!lua_isnoneornil(L, idx))
			out[i] = luaL_checknumber(L, idx);
		else if (T->alphaLast && i == N - 1)
			out[i] = 1.0;
		else
			out[i] = (i == 0) ? 0.0 : out[i - 1];
	}
}

// ---------------------------------------------------------------------------------------------
// construtores / temporarios
// ---------------------------------------------------------------------------------------------

template<int N> static int w_new(lua_State *L){
	const VecType *T = (const VecType *) lua_touserdata(L, UP_TYPE);
	double v[N];
	readCtorArgs<N>(L, T, 1, v);
	pushVec<N>(L, v);
	return 1;
}

template<int N> static int w_newTemp(lua_State *L){
	VecType *T = (VecType *) lua_touserdata(L, UP_TYPE);
	double v[N];
	readCtorArgs<N>(L, T, 1, v);

	if (T->tempIndex >= POOL_SIZE)
		return luaL_error(L, "No temporary %s left", T->name);

	lua_rawgeti(L, LUA_REGISTRYINDEX, T->poolRef);
	lua_rawgeti(L, -1, ++T->tempIndex);
	lua_remove(L, -2);

	writeComps<N>(L, lua_gettop(L), v);
	return 1;
}

template<int N> static int w_is(lua_State *L){
	lua_pushboolean(L, isVec(L, 1));
	return 1;
}

static int w_clean(lua_State *L){
	for (int i = 0; i < 4; i++)
		TYPES[i].tempIndex = 0;
	return 0;
}

// ---------------------------------------------------------------------------------------------
// metamethods
// ---------------------------------------------------------------------------------------------

// so roda quando a chave NAO existe na tabela (metodos e v[1..n])
template<int N> static int w_index(lua_State *L){
	int kt = lua_type(L, 2);

	if (kt == LUA_TSTRING){
		lua_pushvalue(L, 2);
		lua_rawget(L, UP_CLASS);
		return 1;
	}

	if (kt == LUA_TNUMBER){
		double d = lua_tonumber(L, 2);
		int i = (int) d;
		if (i >= 1 && i <= N && (double) i == d){
			lua_pushvalue(L, UP_KEY(i - 1));
			lua_rawget(L, 1);
			return 1;
		}
	}

	lua_pushnil(L);
	return 1;
}

template<int N> static int w_newindex(lua_State *L){
	if (lua_type(L, 2) == LUA_TNUMBER){
		double d = lua_tonumber(L, 2);
		int i = (int) d;
		if (i >= 1 && i <= N && (double) i == d){
			lua_pushvalue(L, UP_KEY(i - 1));
			lua_pushvalue(L, 3);
			lua_rawset(L, 1);
			return 0;
		}
	}

	lua_pushvalue(L, 2);
	lua_pushvalue(L, 3);
	lua_rawset(L, 1);
	return 0;
}

// carrega um operando: numero vira escalar espalhado, tabela e lida como vetor
template<int N> static inline void loadOperand(lua_State *L, int idx, double *out){
	int t = lua_type(L, idx);
	if (t == LUA_TNUMBER){
		double s = lua_tonumber(L, idx);
		for (int i = 0; i < N; i++)
			out[i] = s;
	}
	else if (t == LUA_TTABLE)
		readComps<N>(L, idx, out);
	else
		luaL_argerror(L, idx, "vector or number expected");
}

template<int N, int OP> static int binop(lua_State *L){
	double a[N], b[N], r[N];
	loadOperand<N>(L, 1, a);
	loadOperand<N>(L, 2, b);
	for (int i = 0; i < N; i++)
		r[i] = apply<OP>(a[i], b[i]);
	pushVec<N>(L, r);
	return 1;
}

template<int N> static int w_add(lua_State *L){ return binop<N, OP_ADD>(L); }
template<int N> static int w_sub(lua_State *L){ return binop<N, OP_SUB>(L); }
template<int N> static int w_mul(lua_State *L){ return binop<N, OP_MUL>(L); }
template<int N> static int w_div(lua_State *L){ return binop<N, OP_DIV>(L); }

template<int N> static int w_unm(lua_State *L){
	luaL_checktype(L, 1, LUA_TTABLE);
	double v[N];
	readComps<N>(L, 1, v);
	for (int i = 0; i < N; i++)
		v[i] = -v[i];
	pushVec<N>(L, v);
	return 1;
}

template<int N> static int w_eq(lua_State *L){
	if (!lua_istable(L, 1) || !lua_istable(L, 2)){
		lua_pushboolean(L, 0);
		return 1;
	}
	double a[N], b[N];
	readComps<N>(L, 1, a);
	readComps<N>(L, 2, b);
	bool eq = true;
	for (int i = 0; i < N; i++)
		eq = eq && a[i] == b[i];
	lua_pushboolean(L, eq);
	return 1;
}

template<int N> static int w_tostring(lua_State *L){
	const VecType *T = (const VecType *) lua_touserdata(L, UP_TYPE);
	luaL_checktype(L, 1, LUA_TTABLE);
	double v[N];
	readComps<N>(L, 1, v);

	std::string s = T->name;
	s += "(";
	for (int i = 0; i < N; i++){
		char buf[400];
		snprintf(buf, sizeof(buf), "%s: %.3f", T->fields[i], v[i]);
		if (i > 0)
			s += ", ";
		s += buf;
	}
	s += ")";

	lua_pushlstring(L, s.c_str(), s.size());
	return 1;
}

static int w_concat(lua_State *L){
	// tostring(a) .. tostring(b), igual ao original
	lua_getglobal(L, "tostring");
	lua_pushvalue(L, 1);
	lua_call(L, 1, 1);

	lua_getglobal(L, "tostring");
	lua_pushvalue(L, 2);
	lua_call(L, 1, 1);

	lua_concat(L, 2);
	return 1;
}

// ---------------------------------------------------------------------------------------------
// metodos
// ---------------------------------------------------------------------------------------------

// os proprios rawget viram os valores de retorno, sem passar por double
template<int N> static int w_unpack(lua_State *L){
	luaL_checktype(L, 1, LUA_TTABLE);
	for (int i = 0; i < N; i++){
		lua_pushvalue(L, UP_KEY(i));
		lua_rawget(L, 1);
	}
	return N;
}

template<int N> static int w_toTable(lua_State *L){
	luaL_checktype(L, 1, LUA_TTABLE);
	double v[N];
	readComps<N>(L, 1, v);

	lua_createtable(L, N, N);
	for (int i = 0; i < N; i++){
		lua_pushnumber(L, v[i]);
		lua_rawseti(L, -2, i + 1);
		lua_pushvalue(L, UP_KEY(i));
		lua_pushnumber(L, v[i]);
		lua_rawset(L, -3);
	}
	return 1;
}

template<int N> static int w_length2(lua_State *L){
	luaL_checktype(L, 1, LUA_TTABLE);
	double v[N], sum = 0;
	readComps<N>(L, 1, v);
	for (int i = 0; i < N; i++)
		sum += v[i] * v[i];
	lua_pushnumber(L, sum);
	return 1;
}

template<int N> static int w_length(lua_State *L){
	luaL_checktype(L, 1, LUA_TTABLE);
	double v[N], sum = 0;
	readComps<N>(L, 1, v);
	for (int i = 0; i < N; i++)
		sum += v[i] * v[i];
	lua_pushnumber(L, std::sqrt(sum));
	return 1;
}

template<int N> static int w_dot(lua_State *L){
	luaL_checktype(L, 1, LUA_TTABLE);
	luaL_checktype(L, 2, LUA_TTABLE);
	double a[N], b[N], sum = 0;
	readComps<N>(L, 1, a);
	readComps<N>(L, 2, b);
	for (int i = 0; i < N; i++)
		sum += a[i] * b[i];
	lua_pushnumber(L, sum);
	return 1;
}

// so registrado para vec3
static int w_cross(lua_State *L){
	luaL_checktype(L, 1, LUA_TTABLE);
	luaL_checktype(L, 2, LUA_TTABLE);
	double a[3], b[3];
	readComps<3>(L, 1, a);
	readComps<3>(L, 2, b);

	double r[3] = {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
	pushVec<3>(L, r);
	return 1;
}

template<int N> static int w_normalize(lua_State *L){
	luaL_checktype(L, 1, LUA_TTABLE);
	double v[N], sum = 0;
	readComps<N>(L, 1, v);
	for (int i = 0; i < N; i++)
		sum += v[i] * v[i];

	double len = std::sqrt(sum);
	double inv = (len == 0.0) ? 0.0 : 1.0 / len;
	for (int i = 0; i < N; i++)
		v[i] *= inv;

	pushVec<N>(L, v);
	return 1;
}

template<int N> static int w_set(lua_State *L){
	const VecType *T = (const VecType *) lua_touserdata(L, UP_TYPE);
	luaL_checktype(L, 1, LUA_TTABLE);

	double v[N];
	for (int i = 0; i < N; i++){
		double def = (T->alphaLast && i == N - 1) ? 1.0 : 0.0;
		v[i] = luaL_optnumber(L, 2 + i, def);
	}
	writeComps<N>(L, 1, v);

	lua_pushvalue(L, 1);
	return 1;
}

template<int N> static int w_copy(lua_State *L){
	luaL_checktype(L, 1, LUA_TTABLE);
	double v[N];
	readComps<N>(L, 1, v);
	pushVec<N>(L, v);
	return 1;
}

// versoes que modificam o proprio vetor (add/sub/mul/div) e devolvem self: zero alocacao
template<int N, int OP> static int inplace(lua_State *L){
	luaL_checktype(L, 1, LUA_TTABLE);

	double a[N], b[N];
	readComps<N>(L, 1, a);
	loadOperand<N>(L, 2, b);
	for (int i = 0; i < N; i++)
		a[i] = apply<OP>(a[i], b[i]);
	writeComps<N>(L, 1, a);

	lua_pushvalue(L, 1);
	return 1;
}

template<int N> static int w_addTo(lua_State *L){ return inplace<N, OP_ADD>(L); }
template<int N> static int w_subTo(lua_State *L){ return inplace<N, OP_SUB>(L); }
template<int N> static int w_mulTo(lua_State *L){ return inplace<N, OP_MUL>(L); }
template<int N> static int w_divTo(lua_State *L){ return inplace<N, OP_DIV>(L); }

// ---------------------------------------------------------------------------------------------
// registro
// ---------------------------------------------------------------------------------------------

// cria um closure com os upvalues padrao (tipo, classe, chaves) e guarda em target[name]
static void setTyped(lua_State *L, VecType *T, int cls, int target, const char *name, lua_CFunction fn){
	lua_pushlightuserdata(L, T);
	lua_pushvalue(L, cls);
	for (int i = 0; i < T->n; i++)
		lua_pushstring(L, T->fields[i]); // internada aqui, uma unica vez
	lua_pushcclosure(L, fn, 2 + T->n);
	lua_setfield(L, target, name);
}

template<int N> static void registerFuncs(lua_State *L, VecType *T, int cls, int module){
	setTyped(L, T, cls, cls, "__index", w_index<N>);
	setTyped(L, T, cls, cls, "__newindex", w_newindex<N>);
	setTyped(L, T, cls, cls, "__tostring", w_tostring<N>);
	setTyped(L, T, cls, cls, "__add", w_add<N>);
	setTyped(L, T, cls, cls, "__sub", w_sub<N>);
	setTyped(L, T, cls, cls, "__mul", w_mul<N>);
	setTyped(L, T, cls, cls, "__div", w_div<N>);
	setTyped(L, T, cls, cls, "__unm", w_unm<N>);
	setTyped(L, T, cls, cls, "__eq", w_eq<N>);
	setTyped(L, T, cls, cls, "__concat", w_concat);

	setTyped(L, T, cls, cls, "unpack", w_unpack<N>);
	setTyped(L, T, cls, cls, "toTable", w_toTable<N>);
	setTyped(L, T, cls, cls, "length", w_length<N>);
	setTyped(L, T, cls, cls, "length2", w_length2<N>);
	setTyped(L, T, cls, cls, "dot", w_dot<N>);
	setTyped(L, T, cls, cls, "normalize", w_normalize<N>);
	setTyped(L, T, cls, cls, "set", w_set<N>);
	setTyped(L, T, cls, cls, "copy", w_copy<N>);
	setTyped(L, T, cls, cls, "add", w_addTo<N>);
	setTyped(L, T, cls, cls, "sub", w_subTo<N>);
	setTyped(L, T, cls, cls, "mul", w_mulTo<N>);
	setTyped(L, T, cls, cls, "div", w_divTo<N>);
	if (N == 3)
		setTyped(L, T, cls, cls, "cross", w_cross);

	setTyped(L, T, cls, module, T->newName, w_new<N>);
	setTyped(L, T, cls, module, T->isName, w_is<N>);
	setTyped(L, T, cls, module, T->tempName, w_newTemp<N>);
}

static void registerType(lua_State *L, VecType *T, int module){
	lua_newtable(L); // classe (metatable + metodos)
	int cls = lua_gettop(L);

	switch (T->n){
		case 2: registerFuncs<2>(L, T, cls, module); break;
		case 3: registerFuncs<3>(L, T, cls, module); break;
		default: registerFuncs<4>(L, T, cls, module); break;
	}

	lua_pushvalue(L, cls);
	T->classRef = luaL_ref(L, LUA_REGISTRYINDEX);

	// pool de temporarios pre-alocado (so na inicializacao, entao setfield e ok aqui)
	lua_createtable(L, POOL_SIZE, 0);
	int pool = lua_gettop(L);
	for (int i = 1; i <= POOL_SIZE; i++){
		lua_createtable(L, 0, T->n);
		for (int k = 0; k < T->n; k++){
			lua_pushnumber(L, (T->alphaLast && k == T->n - 1) ? 1.0 : 0.0);
			lua_setfield(L, -2, T->fields[k]);
		}
		lua_pushvalue(L, cls);
		lua_setmetatable(L, -2);
		lua_rawseti(L, pool, i);
	}
	T->poolRef = luaL_ref(L, LUA_REGISTRYINDEX); // desempilha o pool

	lua_pop(L, 1); // classe
}

// cria um vec3 a partir de codigo nativo (usado por Physics.cpp)
void MGOLD_pushVec3(lua_State *L, double x, double y, double z){
	lua_createtable(L, 0, 3);
	lua_pushnumber(L, x);
	lua_setfield(L, -2, "x");
	lua_pushnumber(L, y);
	lua_setfield(L, -2, "y");
	lua_pushnumber(L, z);
	lua_setfield(L, -2, "z");
	lua_rawgeti(L, LUA_REGISTRYINDEX, TYPES[1].classRef);
	lua_setmetatable(L, -2);
}

void MGOLD_setVectorsFunc(lua_State *L){
	lua_newtable(L); // vectors
	int module = lua_gettop(L);

	for (int i = 0; i < 4; i++){
		TYPES[i].tempIndex = 0;
		registerType(L, &TYPES[i], module);
	}

	lua_pushcfunction(L, w_clean);
	lua_setfield(L, module, "clean");
}

}
}