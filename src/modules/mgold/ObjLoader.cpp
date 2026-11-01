#include "modules/mgold/ObjLoader.h"

#include "modules/filesystem/Filesystem.h"

#include <string>
#include <vector>
#include <cstdlib>
#include <cstring>
#include <cstdio>

namespace love
{

namespace mgold
{

// ---------------------------------------------------------------------------------------------
// carregador de .obj (portado de groverbuger / g3d)
//
// objloader(path [, uFlip [, vFlip]]) -> { {x,y,z, nx,ny,nz, u,v, 1,1,1,1}, ... }  (3 por triangulo)
//
//  * o arquivo e lido direto pelo modulo filesystem do LOVE (sem love.filesystem.lines em Lua)
//  * o parse e feito em C++ sobre o buffer inteiro, sem criar strings por palavra
//  * so depois do parse as tabelas de vertice sao criadas, uma vez cada. Em faces com mais de 3
//    vertices (fan), o vertice central e o vertice compartilhado reutilizam a MESMA tabela, como
//    no original
// ---------------------------------------------------------------------------------------------

struct FaceVert{
	int v, vt, vn; // 0 = ausente
};

struct Parsed{
	std::vector<double> pos, uv, nrm;
	std::vector<FaceVert> fv;
	std::vector<int> faceCount;
	int totalVerts = 0;
};

static inline bool isSpace(char c){
	return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
}

static bool nextWord(const char *&q, const char *eol, const char *&ws, const char *&we){
	while (q < eol && isSpace(*q))
		q++;
	if (q >= eol)
		return false;
	ws = q;
	while (q < eol && !isSpace(*q))
		q++;
	we = q;
	return true;
}

// numero da palavra (0 se faltar ou nao for numero)
static double wordNumber(const char *ws, const char *we){
	if (!ws)
		return 0.0;
	char *end = nullptr;
	double v = std::strtod(ws, &end);
	if (end == ws || end > we)
		return 0.0;
	return v;
}

static bool readSigned(const char *&p, const char *e, long long &val, bool &present, bool required){
	bool neg = false;
	if (p < e && *p == '-'){
		neg = true;
		p++;
	}

	const char *d = p;
	long long v = 0;
	while (p < e && *p >= '0' && *p <= '9'){
		v = v * 10 + (*p - '0');
		if (v > 2000000000LL)
			v = 2000000000LL;
		p++;
	}

	if (p == d){
		present = false;
		return !required;
	}

	val = neg ? -v : v;
	present = true;
	return true;
}

// formato: v | v/vt | v//vn | v/vt/vn   (mesmo padrao do original)
static bool parseFaceToken(const char *s, const char *e, long long *vals, bool *has){
	const char *p = s;

	vals[0] = vals[1] = vals[2] = 0;
	has[0] = has[1] = has[2] = false;

	if (!readSigned(p, e, vals[0], has[0], true))
		return false;
	if (p < e && *p == '/')
		p++;
	if (!readSigned(p, e, vals[1], has[1], false))
		return false;
	if (p < e && *p == '/')
		p++;
	if (!readSigned(p, e, vals[2], has[2], false))
		return false;

	return p == e;
}

// devolve false e preenche err se o arquivo for invalido
static bool parseObj(const std::string &text, bool uFlip, bool vFlip, Parsed &out, char *err, size_t errSize){
	const char *p = text.c_str();
	const char *end = p + text.size();

	while (p < end){
		const char *eol = (const char *) std::memchr(p, '\n', (size_t) (end - p));
		if (!eol)
			eol = end;

		const char *q = p;
		const char *ws = nullptr, *we = nullptr;

		if (nextWord(q, eol, ws, we)){
			size_t len = (size_t) (we - ws);

			if (len == 1 && ws[0] == 'v'){
				const char *w[3] = {nullptr, nullptr, nullptr};
				const char *wend[3] = {nullptr, nullptr, nullptr};
				for (int i = 0; i < 3; i++)
					if (!nextWord(q, eol, w[i], wend[i]))
						break;
				for (int i = 0; i < 3; i++)
					out.pos.push_back(wordNumber(w[i], wend[i]));
			}
			else if (len == 2 && ws[0] == 'v' && ws[1] == 't'){
				const char *w[2] = {nullptr, nullptr};
				const char *wend[2] = {nullptr, nullptr};
				for (int i = 0; i < 2; i++)
					if (!nextWord(q, eol, w[i], wend[i]))
						break;

				double u = wordNumber(w[0], wend[0]);
				double v = wordNumber(w[1], wend[1]);
				if (uFlip) u = 1 - u;
				if (vFlip) v = 1 - v;
				out.uv.push_back(u);
				out.uv.push_back(v);
			}
			else if (len == 2 && ws[0] == 'v' && ws[1] == 'n'){
				const char *w[3] = {nullptr, nullptr, nullptr};
				const char *wend[3] = {nullptr, nullptr, nullptr};
				for (int i = 0; i < 3; i++)
					if (!nextWord(q, eol, w[i], wend[i]))
						break;
				for (int i = 0; i < 3; i++)
					out.nrm.push_back(wordNumber(w[i], wend[i]));
			}
			else if (len == 1 && ws[0] == 'f'){
				long long posCount = (long long) (out.pos.size() / 3);
				long long uvCount = (long long) (out.uv.size() / 2);
				long long nrmCount = (long long) (out.nrm.size() / 3);

				int n = 0;
				while (nextWord(q, eol, ws, we)){
					long long vals[3];
					bool has[3];

					bool ok = parseFaceToken(ws, we, vals, has);
					if (ok)
						ok = has[0] && vals[0] >= 1 && vals[0] <= posCount &&
						     (!has[1] || (vals[1] >= 1 && vals[1] <= uvCount)) &&
						     (!has[2] || (vals[2] >= 1 && vals[2] <= nrmCount));

					if (!ok){
						std::snprintf(err, errSize, "mgi3 objloader invalid face token: %.*s", (int) (we - ws), ws);
						return false;
					}

					FaceVert f;
					f.v = (int) vals[0];
					f.vt = has[1] ? (int) vals[1] : 0;
					f.vn = has[2] ? (int) vals[2] : 0;
					out.fv.push_back(f);
					n++;
				}

				out.faceCount.push_back(n);
				out.totalVerts += n > 3 ? 3 * (n - 2) : n;
			}
		}

		p = eol + 1;
	}

	return true;
}

// cria a tabela de um vertice no topo da pilha
static void pushVertex(lua_State *L, const Parsed &P, const FaceVert &f){
	lua_createtable(L, 12, 0);
	int t = lua_gettop(L);

	const double *pos = &P.pos[(size_t) (f.v - 1) * 3];
	for (int k = 0; k < 3; k++){
		lua_pushnumber(L, pos[k]);
		lua_rawseti(L, t, k + 1);
	}

	// sem normal no arquivo: os campos 4..6 ficam nil, igual ao original
	if (f.vn){
		const double *n = &P.nrm[(size_t) (f.vn - 1) * 3];
		for (int k = 0; k < 3; k++){
			lua_pushnumber(L, n[k]);
			lua_rawseti(L, t, 4 + k);
		}
	}

	double u = 0, v = 0;
	if (f.vt){
		u = P.uv[(size_t) (f.vt - 1) * 2];
		v = P.uv[(size_t) (f.vt - 1) * 2 + 1];
	}
	lua_pushnumber(L, u);
	lua_rawseti(L, t, 7);
	lua_pushnumber(L, v);
	lua_rawseti(L, t, 8);

	for (int k = 9; k <= 12; k++){
		lua_pushnumber(L, 1);
		lua_rawseti(L, t, k);
	}
}

static int w_load(lua_State *L){
	const char *path = luaL_checkstring(L, 1);
	bool uFlip = lua_toboolean(L, 2) != 0;
	bool vFlip = lua_toboolean(L, 3) != 0;

	auto fs = Module::getInstance<filesystem::Filesystem>(Module::M_FILESYSTEM);
	if (fs == nullptr)
		return luaL_error(L, "love.filesystem is not loaded");

	filesystem::FileData *fd = nullptr;
	luax_catchexcept(L, [&]() { fd = fs->read(path); });

	char err[320];
	err[0] = 0;

	{
		// objetos C++ ficam neste escopo para serem destruidos ANTES de qualquer luaL_error
		std::string text((const char *) fd->getData(), fd->getSize());
		fd->release();

		Parsed P;
		if (!parseObj(text, uFlip, vFlip, P, err, sizeof(err)))
			goto done;

		{
			lua_createtable(L, P.totalVerts, 0);
			int result = lua_gettop(L);
			lua_createtable(L, 8, 0); // vertices da face atual (reaproveitada)
			int scratch = lua_gettop(L);

			int count = 0;
			size_t base = 0;

			for (size_t fi = 0; fi < P.faceCount.size(); fi++){
				int n = P.faceCount[fi];

				for (int i = 0; i < n; i++){
					pushVertex(L, P, P.fv[base + i]);
					lua_rawseti(L, scratch, i + 1);
				}
				base += n;

				if (n > 3){
					// triangulacao em leque a partir do primeiro vertice
					for (int i = 2; i <= n - 1; i++){
						lua_rawgeti(L, scratch, 1);
						lua_rawseti(L, result, ++count);
						lua_rawgeti(L, scratch, i);
						lua_rawseti(L, result, ++count);
						lua_rawgeti(L, scratch, i + 1);
						lua_rawseti(L, result, ++count);
					}
				}
				else{
					for (int i = 1; i <= n; i++){
						lua_rawgeti(L, scratch, i);
						lua_rawseti(L, result, ++count);
					}
				}
			}

			lua_pop(L, 1); // scratch (result fica no topo)
		}
	}

done:
	if (err[0])
		return luaL_error(L, "%s", err);
	return 1;
}

void MGOLD_setObjLoaderFunc(lua_State *L){
	lua_pushcfunction(L, w_load);
}

}
}