#include "modules/mgold/Stdlib.h"
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <new>
#include <memory>
#include <string>
#include <vector>
#include "common/runtime.h"
#include "common/Module.h"

#define MGOLD_MEMORY "mgoldStdlib"
#define MGOLD_LAYOUT "mgoldLayout"
#define MGOLD_ARENA  "mgoldArena"
#define MGOLD_POOL   "mgoldPool"
#define MGOLD_STACK  "mgoldStack"

#define MGOLD_MAX_VIEWS   16   
#define MGOLD_ARENA_ALIGN 16   
#define MGOLD_ERRLEN      192
#define MGOLD_MAX_DEPTH   8    

namespace love
{
namespace mgold
{






static void setErr(char *err, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(err, MGOLD_ERRLEN, fmt, ap);
	va_end(ap);
}

static void prefixField(char *err, const char *name)
{
	char tmp[MGOLD_ERRLEN];
	snprintf(tmp, MGOLD_ERRLEN, "field '%.40s': %s", name, err);
	memcpy(err, tmp, MGOLD_ERRLEN);
}

static size_t alignUp(size_t v, size_t a)
{
	return (v + a - 1) / a * a;
}

static int absIdx(lua_State *L, int i)
{
	return (i > 0 || i <= LUA_REGISTRYINDEX) ? i : lua_gettop(L) + i + 1;
}

static void setMeta(lua_State *L, const char *name)
{
	luaL_getmetatable(L, name);
	lua_setmetatable(L, -2);
}

static void setFuncs(lua_State *L, const luaL_Reg *l)
{
	for (; l->name; l++)
	{
		lua_pushcfunction(L, l->func);
		lua_setfield(L, -2, l->name);
	}
}

static void *testUData(lua_State *L, int idx, const char *tname)
{
	void *p = lua_touserdata(L, idx);
	if (!p || !lua_getmetatable(L, idx))
		return nullptr;
	luaL_getmetatable(L, tname);
	bool eq = lua_rawequal(L, -1, -2) != 0;
	lua_pop(L, 2);
	return eq ? p : nullptr;
}





enum { T_SINT, T_UINT, T_FP, T_PTR, T_BOOL, T_STR };

struct TypeInfo
{
	const char *name;
	uint8_t size;
	uint8_t align;
	uint8_t kind;
};

#define TI(n, ct, k) { n, (uint8_t)sizeof(ct), (uint8_t)alignof(ct), (uint8_t)(k) }

static const TypeInfo kTypes[] = {
	TI("char", char, T_SINT),
	TI("int8", int8_t, T_SINT),     TI("int8_t", int8_t, T_SINT),
	TI("uint8", uint8_t, T_UINT),   TI("uint8_t", uint8_t, T_UINT),
	TI("int16", int16_t, T_SINT),   TI("int16_t", int16_t, T_SINT),
	TI("uint16", uint16_t, T_UINT), TI("uint16_t", uint16_t, T_UINT),
	TI("int32", int32_t, T_SINT),   TI("int32_t", int32_t, T_SINT),
	TI("uint32", uint32_t, T_UINT), TI("uint32_t", uint32_t, T_UINT),
	TI("int64", int64_t, T_SINT),   TI("int64_t", int64_t, T_SINT),
	TI("uint64", uint64_t, T_UINT), TI("uint64_t", uint64_t, T_UINT),

	TI("short", short, T_SINT),               TI("unsigned short", unsigned short, T_UINT),
	TI("int", int, T_SINT),                   TI("unsigned int", unsigned int, T_UINT),
	TI("long", long, T_SINT),                 TI("unsigned long", unsigned long, T_UINT),
	TI("long long", long long, T_SINT),       TI("unsigned long long", unsigned long long, T_UINT),
	TI("size_t", size_t, T_UINT),
	TI("bool", uint8_t, T_BOOL),
	TI("string", uint32_t, T_STR),   

	TI("float", float, T_FP),
	TI("double", double, T_FP),
	TI("lua_Integer", lua_Integer, T_SINT),
	TI("lua_Number", lua_Number, T_FP),

	TI("ptr", void *, T_PTR),
	TI("void*", void *, T_PTR),
};

static const TypeInfo *findType(const char *s, size_t len)
{
	for (size_t i = 0; i < sizeof(kTypes) / sizeof(kTypes[0]); i++)
	{
		if (strlen(kTypes[i].name) == len && memcmp(kTypes[i].name, s, len) == 0)
			return &kTypes[i];
	}
	return nullptr;
}

static int64_t loadSigned(const uint8_t *p, size_t s)
{
	switch (s)
	{
	case 1: { int8_t v;  memcpy(&v, p, 1); return v; }
	case 2: { int16_t v; memcpy(&v, p, 2); return v; }
	case 4: { int32_t v; memcpy(&v, p, 4); return v; }
	default: { int64_t v; memcpy(&v, p, 8); return v; }
	}
}

static uint64_t loadUnsigned(const uint8_t *p, size_t s)
{
	switch (s)
	{
	case 1: { uint8_t v;  memcpy(&v, p, 1); return v; }
	case 2: { uint16_t v; memcpy(&v, p, 2); return v; }
	case 4: { uint32_t v; memcpy(&v, p, 4); return v; }
	default: { uint64_t v; memcpy(&v, p, 8); return v; }
	}
}

static void storeInt(uint8_t *p, size_t s, uint64_t bits)
{
	switch (s)
	{
	case 1: { uint8_t v = (uint8_t)bits;   memcpy(p, &v, 1); break; }
	case 2: { uint16_t v = (uint16_t)bits; memcpy(p, &v, 2); break; }
	case 4: { uint32_t v = (uint32_t)bits; memcpy(p, &v, 4); break; }
	default: { memcpy(p, &bits, 8); break; }
	}
}


static bool writePrim(lua_State *L, const TypeInfo &t, int idx, uint8_t *p, char *err, size_t cap)
{
	int lt = lua_type(L, idx);

	if (t.kind == T_STR)
	{
		if (lt != LUA_TSTRING)
		{
			setErr(err, "expected string, got %s", lua_typename(L, lt));
			return false;
		}
		size_t len;
		const char *s = lua_tolstring(L, idx, &len);
		if (len > cap)
		{
			setErr(err, "string too long (%llu bytes, capacity %llu)",
				(unsigned long long)len, (unsigned long long)cap);
			return false;
		}
		uint32_t l = (uint32_t)len;
		memcpy(p, &l, 4);
		memcpy(p + 4, s, len);
		return true;
	}

	if (t.kind == T_BOOL)
	{
		if (lt == LUA_TBOOLEAN)
			*p = lua_toboolean(L, idx) ? 1 : 0;
		else if (lt == LUA_TNUMBER)
			*p = lua_tonumber(L, idx) != 0 ? 1 : 0;
		else
		{
			setErr(err, "expected boolean or number for '%s'", t.name);
			return false;
		}
		return true;
	}

	if (lt != LUA_TNUMBER)
	{
		setErr(err, "expected number for '%s', got %s", t.name, lua_typename(L, lt));
		return false;
	}

	double n = (double)lua_tonumber(L, idx);

	if (t.kind == T_FP)
	{
		if (t.size == 4) { float f = (float)n; memcpy(p, &f, 4); }
		else { double d = n;       memcpy(p, &d, 8); }
		return true;
	}

	if (n != n || n >= 18446744073709551616.0 || n < -9223372036854775808.0)
	{
		setErr(err, "value out of range for '%s'", t.name);
		return false;
	}

	uint64_t bits = (n < 0) ? (uint64_t)(int64_t)n : (uint64_t)n;
	storeInt(p, t.size, bits);
	return true;
}

static void readPrim(lua_State *L, const TypeInfo &t, const uint8_t *p, size_t cap)
{
	switch (t.kind)
	{
	case T_STR:
	{
		uint32_t l;
		memcpy(&l, p, 4);
		if (l > cap)
			l = (uint32_t)cap;
		lua_pushlstring(L, (const char *)p + 4, l);
		break;
	}
	case T_BOOL: lua_pushboolean(L, *p != 0); break;
	case T_SINT: lua_pushnumber(L, (lua_Number)loadSigned(p, t.size)); break;
	case T_UINT: lua_pushnumber(L, (lua_Number)loadUnsigned(p, t.size)); break;
	case T_PTR:  lua_pushinteger(L, (lua_Integer)(uintptr_t)loadUnsigned(p, t.size)); break;
	default:
		if (t.size == 4) { float f;  memcpy(&f, p, 4); lua_pushnumber(L, (lua_Number)f); }
		else { double d; memcpy(&d, p, 8); lua_pushnumber(L, (lua_Number)d); }
		break;
	}
}





struct Layout;
typedef std::shared_ptr<Layout> LayoutPtr;

struct Field
{
	std::string name;
	const TypeInfo *prim;   
	LayoutPtr sub;          
	size_t offset;
	size_t elemSize;
	size_t count;           
	size_t cap;             
	bool isArray;
};

struct Layout
{
	std::vector<Field> fields;
	size_t size;
	size_t align;
	Layout() : size(0), align(1) {}
};

struct TypeRef
{
	const TypeInfo *prim;
	LayoutPtr lay;
	TypeRef() : prim(nullptr) {}
	size_t size() const { return prim ? prim->size : lay->size; }
	size_t align() const { return prim ? prim->align : lay->align; }
};

static bool compileLayout(lua_State *L, int idx, LayoutPtr &out, char *err, int depth);

static bool resolveFieldType(lua_State *L, int idx, const TypeInfo *&prim, LayoutPtr &sub, char *err, int depth)
{
	int t = lua_type(L, idx);
	if (t == LUA_TSTRING)
	{
		size_t len;
		const char *s = lua_tolstring(L, idx, &len);
		prim = findType(s, len);
		if (!prim)
		{
			setErr(err, "unknown type '%s'", s);
			return false;
		}
		return true;
	}
	if (t == LUA_TTABLE || t == LUA_TUSERDATA)
		return compileLayout(L, idx, sub, err, depth + 1);

	setErr(err, "type must be a string or a struct definition");
	return false;
}


static bool compileLayout(lua_State *L, int idx, LayoutPtr &out, char *err, int depth)
{
	idx = absIdx(L, idx);

	if (depth > MGOLD_MAX_DEPTH)
	{
		setErr(err, "struct nested too deeply");
		return false;
	}

	if (lua_type(L, idx) == LUA_TUSERDATA)
	{
		LayoutPtr *p = (LayoutPtr *)testUData(L, idx, MGOLD_LAYOUT);
		if (!p || !*p)
		{
			setErr(err, "expected struct definition");
			return false;
		}
		out = *p;
		return true;
	}

	if (lua_type(L, idx) != LUA_TTABLE)
	{
		setErr(err, "expected struct definition (table)");
		return false;
	}

	if (!lua_checkstack(L, 10))
	{
		setErr(err, "stack overflow");
		return false;
	}

	size_t n = lua_objlen(L, idx);
	if (n == 0)
	{
		setErr(err, "struct has no fields");
		return false;
	}

	LayoutPtr lay = std::make_shared<Layout>();
	size_t off = 0;
	size_t maxAlign = 1;

	for (size_t i = 1; i <= n; i++)
	{
		lua_rawgeti(L, idx, (int)i);
		int fi = lua_gettop(L);

		if (lua_type(L, fi) != LUA_TTABLE)
		{
			setErr(err, "field #%d must be {name, type [, count]}", (int)i);
			return false;
		}

		lua_rawgeti(L, fi, 1);
		lua_rawgeti(L, fi, 2);
		lua_rawgeti(L, fi, 3);

		if (lua_type(L, fi + 1) != LUA_TSTRING)
		{
			setErr(err, "field #%d: name must be a string", (int)i);
			return false;
		}

		Field f;
		size_t nl;
		const char *nm = lua_tolstring(L, fi + 1, &nl);
		f.name.assign(nm, nl);
		f.prim = nullptr;
		f.offset = 0;
		f.elemSize = 0;
		f.count = 1;
		f.cap = 0;
		f.isArray = false;

		for (size_t k = 0; k < lay->fields.size(); k++)
		{
			if (lay->fields[k].name == f.name)
			{
				setErr(err, "duplicate field '%.40s'", f.name.c_str());
				return false;
			}
		}

		if (!resolveFieldType(L, fi + 2, f.prim, f.sub, err, depth))
		{
			prefixField(err, f.name.c_str());
			return false;
		}

		if (!lua_isnil(L, fi + 3))
		{
			if (lua_type(L, fi + 3) != LUA_TNUMBER)
			{
				setErr(err, "field '%.40s': count must be a number", f.name.c_str());
				return false;
			}
			double c = (double)lua_tonumber(L, fi + 3);
			if (c < 1 || c > 1048576 || c != (double)(size_t)c)
			{
				setErr(err, "field '%.40s': invalid array count", f.name.c_str());
				return false;
			}
			if (f.prim && f.prim->kind == T_STR)
				f.cap = (size_t)c;       
			else
			{
				f.count = (size_t)c;
				f.isArray = true;
			}
		}
		else if (f.prim && f.prim->kind == T_STR)
		{
			setErr(err, "field '%.40s': string needs a capacity, e.g. {\"name\", \"string\", 32}", f.name.c_str());
			return false;
		}

		size_t a = f.prim ? f.prim->align : f.sub->align;
		f.elemSize = f.prim ? f.prim->size : f.sub->size;
		if (f.prim && f.prim->kind == T_STR)
			f.elemSize = f.prim->size + f.cap;   

		off = alignUp(off, a);
		f.offset = off;
		off += f.elemSize * f.count;
		if (a > maxAlign)
			maxAlign = a;

		if (off > ((size_t)1 << 30))
		{
			setErr(err, "struct too large");
			return false;
		}

		lay->fields.push_back(f);
		lua_pop(L, 4);
	}

	lay->align = maxAlign;
	lay->size = alignUp(off, maxAlign);
	out = lay;
	return true;
}

static bool resolveType(lua_State *L, int idx, TypeRef &t, char *err)
{
	if (lua_type(L, idx) == LUA_TSTRING)
	{
		size_t len;
		const char *s = lua_tolstring(L, idx, &len);
		t.prim = findType(s, len);
		if (!t.prim)
		{
			setErr(err, "unknown type '%s'", s);
			return false;
		}
		return true;
	}
	return compileLayout(L, idx, t.lay, err, 0);
}

static bool writeLayout(lua_State *L, const Layout &lay, int idx, uint8_t *p, char *err, int depth);

static bool writeElem(lua_State *L, const Field &f, int idx, uint8_t *p, char *err, int depth)
{
	if (f.prim)
		return writePrim(L, *f.prim, idx, p, err, f.cap);
	return writeLayout(L, *f.sub, idx, p, err, depth + 1);
}


static bool writeLayout(lua_State *L, const Layout &lay, int idx, uint8_t *p, char *err, int depth)
{
	idx = absIdx(L, idx);

	if (depth > MGOLD_MAX_DEPTH)
	{
		setErr(err, "struct nested too deeply");
		return false;
	}
	if (lua_type(L, idx) != LUA_TTABLE)
	{
		setErr(err, "expected table for struct");
		return false;
	}
	if (!lua_checkstack(L, 8))
	{
		setErr(err, "stack overflow");
		return false;
	}

	for (size_t i = 0; i < lay.fields.size(); i++)
	{
		const Field &f = lay.fields[i];

		lua_pushlstring(L, f.name.c_str(), f.name.size());
		lua_rawget(L, idx);
		int vi = lua_gettop(L);
		bool ok = true;

		if (!lua_isnil(L, vi))
		{
			if (!f.isArray)
			{
				ok = writeElem(L, f, vi, p + f.offset, err, depth);
			}
			else if (lua_type(L, vi) != LUA_TTABLE)
			{
				setErr(err, "expected array table");
				ok = false;
			}
			else
			{
				for (size_t k = 0; k < f.count && ok; k++)
				{
					lua_rawgeti(L, vi, (int)k + 1);
					if (!lua_isnil(L, -1))
						ok = writeElem(L, f, lua_gettop(L), p + f.offset + k * f.elemSize, err, depth);
					lua_pop(L, 1);
				}
			}
		}

		lua_pop(L, 1);

		if (!ok)
		{
			prefixField(err, f.name.c_str());
			return false;
		}
	}
	return true;
}

static bool writeType(lua_State *L, const TypeRef &t, int idx, uint8_t *p, char *err, size_t cap = 0)
{
	if (t.prim)
		return writePrim(L, *t.prim, idx, p, err, cap);
	return writeLayout(L, *t.lay, idx, p, err, 0);
}

static void readElem(lua_State *L, const Field &f, const uint8_t *p);

static void readLayout(lua_State *L, const Layout &lay, const uint8_t *p)
{
	lua_checkstack(L, 6);
	lua_createtable(L, 0, (int)lay.fields.size());

	for (size_t i = 0; i < lay.fields.size(); i++)
	{
		const Field &f = lay.fields[i];
		const uint8_t *fp = p + f.offset;

		if (!f.isArray)
		{
			readElem(L, f, fp);
		}
		else
		{
			lua_createtable(L, (int)f.count, 0);
			for (size_t k = 0; k < f.count; k++)
			{
				readElem(L, f, fp + k * f.elemSize);
				lua_rawseti(L, -2, (int)k + 1);
			}
		}
		lua_setfield(L, -2, f.name.c_str());
	}
}

static void readElem(lua_State *L, const Field &f, const uint8_t *p)
{
	if (f.prim)
		readPrim(L, *f.prim, p, f.cap);
	else
		readLayout(L, *f.sub, p);
}

static void readType(lua_State *L, const TypeRef &t, const uint8_t *p, size_t cap = 0)
{
	if (t.prim)
		readPrim(L, *t.prim, p, cap);
	else
		readLayout(L, *t.lay, p);
}

static bool isStrType(const TypeRef &t) { return t.prim && t.prim->kind == T_STR; }


static bool valueSize(lua_State *L, const TypeRef &t, int idx, size_t *sz, char *err)
{
	if (!isStrType(t))
	{
		*sz = t.size();
		return true;
	}
	if (lua_type(L, idx) != LUA_TSTRING)
	{
		setErr(err, "expected string, got %s", lua_typename(L, lua_type(L, idx)));
		return false;
	}
	*sz = 4 + lua_objlen(L, idx);
	return true;
}


static bool storedStringSize(const uint8_t *p, size_t avail, size_t *sz, char *err)
{
	uint32_t l;
	if (avail < 4)
	{
		setErr(err, "Buffer too small for string header");
		return false;
	}
	memcpy(&l, p, 4);
	if ((size_t)l > avail - 4)
	{
		setErr(err, "Stored string length (%llu) exceeds buffer", (unsigned long long)l);
		return false;
	}
	*sz = 4 + (size_t)l;
	return true;
}

static int layout_getSize(lua_State *L)
{
	LayoutPtr *p = (LayoutPtr *)luaL_checkudata(L, 1, MGOLD_LAYOUT);
	lua_pushinteger(L, (lua_Integer)(*p)->size);
	return 1;
}

static int layout_getAlign(lua_State *L)
{
	LayoutPtr *p = (LayoutPtr *)luaL_checkudata(L, 1, MGOLD_LAYOUT);
	lua_pushinteger(L, (lua_Integer)(*p)->align);
	return 1;
}

static int layout_offsetof(lua_State *L)
{
	LayoutPtr *p = (LayoutPtr *)luaL_checkudata(L, 1, MGOLD_LAYOUT);
	size_t len;
	const char *name = luaL_checklstring(L, 2, &len);
	const Layout &lay = **p;
	for (size_t i = 0; i < lay.fields.size(); i++)
	{
		if (lay.fields[i].name.size() == len && memcmp(lay.fields[i].name.data(), name, len) == 0)
		{
			lua_pushinteger(L, (lua_Integer)lay.fields[i].offset);
			return 1;
		}
	}
	lua_pushnil(L);
	return 1;
}

static int layout_gc(lua_State *L)
{
	LayoutPtr *p = (LayoutPtr *)lua_touserdata(L, 1);
	if (p)
		p->~LayoutPtr();
	return 0;
}

static const luaL_Reg layout_methods[] = {
	{ "getSize", layout_getSize },
	{ "getAlign", layout_getAlign },
	{ "offsetof", layout_offsetof },
	{ nullptr, nullptr }
};

static int w_struct(lua_State *L)
{
	LayoutPtr *p = (LayoutPtr *)lua_newuserdata(L, sizeof(LayoutPtr));
	new (p) LayoutPtr();
	setMeta(L, MGOLD_LAYOUT);

	char err[MGOLD_ERRLEN];
	err[0] = 0;
	if (!compileLayout(L, 1, *p, err, 0))
		return luaL_error(L, "%s", err);
	return 1;
}

static int sizeofImpl(lua_State *L, bool wantAlign)
{
	char err[MGOLD_ERRLEN];
	err[0] = 0;
	bool isStr = lua_type(L, 1) == LUA_TSTRING;
	bool ok;
	size_t v = 0;
	{
		TypeRef t;
		ok = resolveType(L, 1, t, err);
		if (ok)
			v = wantAlign ? t.align() : t.size();
	}
	if (!ok)
	{
		if (isStr)
		{
			lua_pushnil(L);
			return 1;
		}
		return luaL_error(L, "%s", err);
	}
	lua_pushinteger(L, (lua_Integer)v);
	return 1;
}

static int w_sizeof(lua_State *L) { return sizeofImpl(L, false); }
static int w_alignof(lua_State *L) { return sizeofImpl(L, true); }






struct Life
{
	bool alive;
	int refs;
	int views;
};

static Life *lifeNew()
{
	Life *l = new Life;
	l->alive = true;
	l->refs = 0;
	l->views = 0;
	return l;
}
static void lifeRef(Life *l) { l->refs++; }
static void lifeUnref(Life *l)
{
	if (--l->refs <= 0)
		delete l;
}

struct Seg { size_t off; size_t size; Life *life; };   
struct Entry { size_t off; size_t size; size_t prevTop; }; 


struct Storage
{
	uint8_t *data;
	size_t size;
	bool alive;
	int refs;

	
	std::vector<Seg> segs;

	
	size_t stride;
	std::vector<Life *> slots;
	std::vector<uint32_t> freeList;
	LayoutPtr layout;

	
	size_t top;
	std::vector<Entry> entries;

	Storage() : data(nullptr), size(0), alive(true), refs(0), stride(0), top(0) {}
	~Storage();
};

static void killStorage(Storage *st)
{
	if (!st->alive)
		return;
	st->alive = false;

	for (size_t i = 0; i < st->segs.size(); i++)
	{
		if (st->segs[i].life)
		{
			st->segs[i].life->alive = false;
			lifeUnref(st->segs[i].life);
		}
	}
	st->segs.clear();

	for (size_t i = 0; i < st->slots.size(); i++)
	{
		if (st->slots[i])
		{
			st->slots[i]->alive = false;
			lifeUnref(st->slots[i]);
		}
	}
	st->slots.clear();
	st->freeList.clear();
	st->entries.clear();
	st->top = 0;

	free(st->data);
	st->data = nullptr;
}

Storage::~Storage() { killStorage(this); }

static void stUnref(Storage *st)
{
	if (--st->refs <= 0)
		delete st;
}

static Storage *stNew(size_t size)
{
	Storage *st = new (std::nothrow) Storage;
	if (!st)
		return nullptr;
	st->data = (uint8_t *)calloc(1, size);
	if (!st->data)
	{
		delete st;
		return nullptr;
	}
	st->size = size;
	return st;
}



static void arenaInit(Storage *st)
{
	st->segs.clear();
	Seg s;
	s.off = 0;
	s.size = st->size;
	s.life = nullptr;
	st->segs.push_back(s);
}

static bool arenaAlloc(Storage *st, size_t req, size_t *outOff, Life *life)
{
	size_t need = alignUp(req, MGOLD_ARENA_ALIGN);

	for (size_t i = 0; i < st->segs.size(); i++)
	{
		if (st->segs[i].life || st->segs[i].size < need)
			continue;

		size_t off = st->segs[i].off;
		size_t restSize = st->segs[i].size - need;

		st->segs[i].size = need;
		st->segs[i].life = life;
		lifeRef(life);

		if (restSize > 0)
		{
			Seg rest;
			rest.off = off + need;
			rest.size = restSize;
			rest.life = nullptr;
			st->segs.insert(st->segs.begin() + i + 1, rest);
		}
		*outOff = off;
		return true;
	}
	return false;
}

static void arenaRelease(Storage *st, size_t off, Life *life)
{
	size_t lo = 0, hi = st->segs.size();
	while (lo < hi)
	{
		size_t mid = (lo + hi) / 2;
		if (st->segs[mid].off < off) lo = mid + 1;
		else hi = mid;
	}
	if (lo >= st->segs.size() || st->segs[lo].off != off || st->segs[lo].life != life)
		return;

	lifeUnref(st->segs[lo].life);
	st->segs[lo].life = nullptr;

	if (lo + 1 < st->segs.size() && !st->segs[lo + 1].life)
	{
		st->segs[lo].size += st->segs[lo + 1].size;
		st->segs.erase(st->segs.begin() + lo + 1);
	}
	if (lo > 0 && !st->segs[lo - 1].life)
	{
		st->segs[lo - 1].size += st->segs[lo].size;
		st->segs.erase(st->segs.begin() + lo);
	}
}

static void arenaReset(Storage *st)
{
	for (size_t i = 0; i < st->segs.size(); i++)
	{
		if (st->segs[i].life)
		{
			st->segs[i].life->alive = false;
			lifeUnref(st->segs[i].life);
		}
	}
	arenaInit(st);
}



static void poolRelease(Storage *st, size_t off, Life *life)
{
	if (st->stride == 0)
		return;
	size_t idx = off / st->stride;
	if (idx < st->slots.size() && st->slots[idx] == life)
	{
		st->slots[idx] = nullptr;
		lifeUnref(life);
		st->freeList.push_back((uint32_t)idx);
	}
}

static void poolReset(Storage *st)
{
	for (size_t i = 0; i < st->slots.size(); i++)
	{
		if (st->slots[i])
		{
			st->slots[i]->alive = false;
			lifeUnref(st->slots[i]);
			st->slots[i] = nullptr;
		}
	}
	st->freeList.clear();
	for (size_t i = st->slots.size(); i > 0; i--)
		st->freeList.push_back((uint32_t)(i - 1));
}





enum { K_RAW, K_ARENA, K_POOL, K_VIEW };

struct mgold_mem
{
	Storage *st;
	Life *life;
	size_t offset;
	size_t capacity;
	uint8_t kind;
	bool readonly;
};

static bool mem_valid(const mgold_mem *m)
{
	return m->st && m->life && m->st->alive && m->life->alive;
}

static uint8_t *mem_data(const mgold_mem *m)
{
	return m->st->data + m->offset;
}

static mgold_mem *check_mem(lua_State *L, int idx, bool write)
{
	mgold_mem *m = (mgold_mem *)luaL_checkudata(L, idx, MGOLD_MEMORY);
	if (!mem_valid(m))
		luaL_error(L, "Can't use freed memory");
	if (write && m->readonly)
		luaL_error(L, "Memory is read-only");
	return m;
}


static mgold_mem *newHandle(lua_State *L)
{
	mgold_mem *m = (mgold_mem *)lua_newuserdata(L, sizeof(mgold_mem));
	memset(m, 0, sizeof(*m));
	setMeta(L, MGOLD_MEMORY);
	return m;
}


static void initHandle(mgold_mem *m, Storage *st, Life *life, size_t off, size_t cap, uint8_t kind, bool ro)
{
	st->refs++;
	m->st = st;
	m->life = life;
	m->offset = off;
	m->capacity = cap;
	m->kind = kind;
	m->readonly = ro;
}


static void mem_release(mgold_mem *m)
{
	if (!m->st || !m->life || m->kind == K_VIEW)
		return;
	if (!m->life->alive)
		return;

	m->life->alive = false;

	Storage *st = m->st;
	if (!st->alive)
		return;

	switch (m->kind)
	{
	case K_RAW:   killStorage(st); break;
	case K_ARENA: arenaRelease(st, m->offset, m->life); break;
	case K_POOL:  poolRelease(st, m->offset, m->life); break;
	}
}

static size_t optOffset(lua_State *L, int idx)
{
	lua_Integer o = luaL_optinteger(L, idx, 0);
	if (o < 0)
		luaL_error(L, "Invalid offset");
	return (size_t)o;
}



static int memory_getSize(lua_State *L)
{
	mgold_mem *m = check_mem(L, 1, false);
	lua_pushinteger(L, (lua_Integer)m->capacity);
	return 1;
}

static int memory_hex(lua_State *L)
{
	mgold_mem *m = check_mem(L, 1, false);
	static const char digits[] = "0123456789ABCDEF";
	const uint8_t *d = mem_data(m);

	luaL_Buffer b;
	luaL_buffinit(L, &b);
	for (size_t i = 0; i < m->capacity; i++)
	{
		luaL_addchar(&b, digits[d[i] >> 4]);
		luaL_addchar(&b, digits[d[i] & 15]);
	}
	luaL_pushresult(&b);
	return 1;
}

static int memory_free(lua_State *L)
{
	mgold_mem *m = (mgold_mem *)luaL_checkudata(L, 1, MGOLD_MEMORY);
	if (m->kind == K_VIEW)
		return luaL_error(L, "Views can't be freed");
	mem_release(m);
	return 0;
}

static int memory_gc(lua_State *L)
{
	mgold_mem *m = (mgold_mem *)lua_touserdata(L, 1);
	if (!m || !m->st || !m->life)
		return 0;

	if (m->kind == K_VIEW)
		m->life->views--;
	else
		mem_release(m);

	lifeUnref(m->life);
	stUnref(m->st);
	m->life = nullptr;
	m->st = nullptr;
	return 0;
}

static int memory_address(lua_State *L)
{
	mgold_mem *m = check_mem(L, 1, false);
	lua_pushinteger(L, (lua_Integer)(uintptr_t)mem_data(m));
	return 1;
}

static int memory_isFree(lua_State *L)
{
	mgold_mem *m = (mgold_mem *)luaL_checkudata(L, 1, MGOLD_MEMORY);
	lua_pushboolean(L, mem_valid(m) ? 0 : 1);
	return 1;
}

static int memory_isView(lua_State *L)
{
	mgold_mem *m = (mgold_mem *)luaL_checkudata(L, 1, MGOLD_MEMORY);
	lua_pushboolean(L, m->kind == K_VIEW);
	return 1;
}

static int memory_isReadOnly(lua_State *L)
{
	mgold_mem *m = (mgold_mem *)luaL_checkudata(L, 1, MGOLD_MEMORY);
	lua_pushboolean(L, m->readonly);
	return 1;
}

static int memory_clear(lua_State *L)
{
	mgold_mem *m = check_mem(L, 1, true);
	memset(mem_data(m), 0, m->capacity);
	return 0;
}

static int memory_fill(lua_State *L)
{
	mgold_mem *m = check_mem(L, 1, true);

	size_t minOffset = (size_t)luaL_checkinteger(L, 2);
	size_t maxOffset = (size_t)luaL_checkinteger(L, 3);
	uint8_t byte = (uint8_t)luaL_checkinteger(L, 4);

	if (maxOffset == 0)
		maxOffset = m->capacity - 1;

	if (minOffset > maxOffset)
		return luaL_error(L, "Invalid minOffset");
	if (maxOffset >= m->capacity)
		return luaL_error(L, "Invalid maxOffset");

	memset(mem_data(m) + minOffset, byte, maxOffset - minOffset + 1);
	return 0;
}



static int memory_view(lua_State *L)
{
	mgold_mem *m = check_mem(L, 1, false);

	lua_Integer mn = luaL_optinteger(L, 2, 0);
	lua_Integer mx = luaL_optinteger(L, 3, (lua_Integer)m->capacity - 1);

	if (mn < 0 || mn > mx)
		return luaL_error(L, "Invalid view range");
	if ((size_t)mx >= m->capacity)
		return luaL_error(L, "View is out of bounds");
	if (m->life->views >= MGOLD_MAX_VIEWS)
		return luaL_error(L, "View limit reached (max %d per object)", MGOLD_MAX_VIEWS);

	mgold_mem *v = newHandle(L);
	lifeRef(m->life);
	m->life->views++;
	initHandle(v, m->st, m->life, m->offset + (size_t)mn, (size_t)(mx - mn + 1), K_VIEW, true);

	
	lua_createtable(L, 1, 0);
	lua_pushvalue(L, 1);
	lua_rawseti(L, -2, 1);
	lua_setfenv(L, -2);
	return 1;
}

static const luaL_Reg memory_methods[] = {
	{ "getSize", memory_getSize },
	{ "getHex", memory_hex },
	{ "free", memory_free },
	{ "isFree", memory_isFree },
	{ "address", memory_address },
	{ "clear", memory_clear },
	{ "fill", memory_fill },
	{ "view", memory_view },
	{ "isView", memory_isView },
	{ "isReadOnly", memory_isReadOnly },
	{ nullptr, nullptr }
};

static int memory_index(lua_State *L)
{
	mgold_mem *m = (mgold_mem *)luaL_checkudata(L, 1, MGOLD_MEMORY);

	if (lua_type(L, 2) == LUA_TSTRING)
	{
		const char *key = lua_tostring(L, 2);
		for (const luaL_Reg *r = memory_methods; r->name; r++)
		{
			if (strcmp(key, r->name) == 0)
			{
				lua_pushcfunction(L, r->func);
				return 1;
			}
		}
		return luaL_error(L, "No function '%s'", key);
	}

	if (!mem_valid(m))
		return luaL_error(L, "Can't use freed memory");

	lua_Integer index = luaL_checkinteger(L, 2);
	if (index < 0 || (size_t)index >= m->capacity)
		return luaL_error(L, "Index is out of bounds");

	lua_pushinteger(L, mem_data(m)[index]);
	return 1;
}

static int memory_newindex(lua_State *L)
{
	mgold_mem *m = check_mem(L, 1, true);

	lua_Integer index = luaL_checkinteger(L, 2);
	if (index < 0 || (size_t)index >= m->capacity)
		return luaL_error(L, "Index is out of bounds");

	mem_data(m)[index] = (uint8_t)luaL_checkinteger(L, 3);
	return 0;
}

static int memory_tostring(lua_State *L)
{
	mgold_mem *m = check_mem(L, 1, false);
	lua_pushlstring(L, (const char *)mem_data(m), m->capacity);
	return 1;
}

static int w_malloc(lua_State *L)
{
	lua_Integer size = luaL_checkinteger(L, 1);
	if (size <= 0 || (uint64_t)size > (uint64_t)(SIZE_MAX / 2))
		return luaL_error(L, "Invalid allocation size");

	mgold_mem *m = newHandle(L);

	Storage *st = stNew((size_t)size);
	if (!st)
		return luaL_error(L, "Out of memory");

	Life *life = lifeNew();
	lifeRef(life);
	initHandle(m, st, life, 0, (size_t)size, K_RAW, false);
	return 1;
}





static void getReadable(lua_State *L, int idx, const uint8_t **data, size_t *size)
{
	if (lua_type(L, idx) == LUA_TSTRING)
	{
		*data = (const uint8_t *)lua_tolstring(L, idx, size);
		return;
	}
	mgold_mem *m = check_mem(L, idx, false);
	*data = mem_data(m);
	*size = m->capacity;
}


static int w_unpack(lua_State *L)
{
	const uint8_t *data = nullptr;
	size_t dataSize = 0;
	getReadable(L, 1, &data, &dataSize);
	size_t off = optOffset(L, 3);

	char err[MGOLD_ERRLEN];
	err[0] = 0;
	bool ok;
	{
		TypeRef t;
		ok = resolveType(L, 2, t, err);
		size_t need = 0;
		if (ok && isStrType(t))
		{
			if (off > dataSize)
			{
				setErr(err, "Invalid offset");
				ok = false;
			}
			else
				ok = storedStringSize(data + off, dataSize - off, &need, err);
		}
		else if (ok)
			need = t.size();
		if (ok && (off > dataSize || need > dataSize - off))
		{
			setErr(err, "Buffer too small (need %llu bytes at offset %llu, have %llu)",
				(unsigned long long)need, (unsigned long long)off, (unsigned long long)dataSize);
			ok = false;
		}
		if (ok)
			readType(L, t, data + off, need - (isStrType(t) ? 4 : 0));
	}
	if (!ok)
		return luaL_error(L, "%s", err);
	return 1;
}


static int w_pack(lua_State *L)
{
	mgold_mem *m = check_mem(L, 1, true);
	size_t off = optOffset(L, 4);
	uint8_t *base = mem_data(m);

	char err[MGOLD_ERRLEN];
	err[0] = 0;
	bool ok;
	{
		TypeRef t;
		ok = resolveType(L, 2, t, err);
		size_t need = 0;
		if (ok)
			ok = valueSize(L, t, 3, &need, err);
		if (ok && (off > m->capacity || need > m->capacity - off))
		{
			setErr(err, "Buffer too small (need %llu bytes at offset %llu, have %llu)",
				(unsigned long long)need, (unsigned long long)off, (unsigned long long)m->capacity);
			ok = false;
		}
		if (ok)
		{
			memset(base + off, 0, need);   
			ok = writeType(L, t, 3, base + off, err, need - (isStrType(t) ? 4 : 0));
		}
	}
	if (!ok)
		return luaL_error(L, "%s", err);
	return 0;
}





struct mgold_alloc
{
	Storage *st;
};

static mgold_alloc *newAllocUD(lua_State *L, const char *tname)
{
	mgold_alloc *a = (mgold_alloc *)lua_newuserdata(L, sizeof(mgold_alloc));
	a->st = nullptr;
	setMeta(L, tname);
	return a;
}

static Storage *check_alloc(lua_State *L, int idx, const char *tname)
{
	mgold_alloc *a = (mgold_alloc *)luaL_checkudata(L, idx, tname);
	if (!a->st || !a->st->alive)
		luaL_error(L, "Allocator was destroyed");
	return a->st;
}

static int alloc_gc(lua_State *L)
{
	mgold_alloc *a = (mgold_alloc *)lua_touserdata(L, 1);
	if (a && a->st)
	{
		stUnref(a->st);
		a->st = nullptr;
	}
	return 0;
}

static int alloc_isDestroyed(lua_State *L, const char *tname)
{
	mgold_alloc *a = (mgold_alloc *)luaL_checkudata(L, 1, tname);
	lua_pushboolean(L, (!a->st || !a->st->alive) ? 1 : 0);
	return 1;
}

static int alloc_destroy(lua_State *L, const char *tname)
{
	mgold_alloc *a = (mgold_alloc *)luaL_checkudata(L, 1, tname);
	if (a->st)
		killStorage(a->st);
	return 0;
}

static lua_Integer checkSize(lua_State *L, int idx)
{
	lua_Integer s = luaL_checkinteger(L, idx);
	if (s <= 0 || (uint64_t)s > (uint64_t)(SIZE_MAX / 4))
		luaL_error(L, "Invalid size");
	return s;
}



static int w_arena(lua_State *L)
{
	lua_Integer size = checkSize(L, 1);
	mgold_alloc *a = newAllocUD(L, MGOLD_ARENA);

	Storage *st = stNew(alignUp((size_t)size, MGOLD_ARENA_ALIGN));
	if (!st)
		return luaL_error(L, "Out of memory");
	st->refs = 1;
	a->st = st;
	arenaInit(st);
	return 1;
}

static int arena_alloc(lua_State *L)
{
	Storage *st = check_alloc(L, 1, MGOLD_ARENA);
	lua_Integer size = luaL_checkinteger(L, 2);
	if (size <= 0 || (uint64_t)size > (uint64_t)st->size)
		return luaL_error(L, "Invalid allocation size");

	mgold_mem *m = newHandle(L);
	Life *life = lifeNew();
	size_t off = 0;

	if (!arenaAlloc(st, (size_t)size, &off, life))
	{
		delete life;
		return luaL_error(L, "Arena out of memory");
	}

	lifeRef(life);
	memset(st->data + off, 0, (size_t)size);
	initHandle(m, st, life, off, (size_t)size, K_ARENA, false);
	return 1;
}

static int arena_free(lua_State *L)
{
	Storage *st = check_alloc(L, 1, MGOLD_ARENA);
	mgold_mem *m = (mgold_mem *)luaL_checkudata(L, 2, MGOLD_MEMORY);
	if (m->kind != K_ARENA || m->st != st)
		return luaL_error(L, "Memory does not belong to this arena");
	mem_release(m);
	return 0;
}

static int arena_reset(lua_State *L)
{
	arenaReset(check_alloc(L, 1, MGOLD_ARENA));
	return 0;
}

static int arena_getCapacity(lua_State *L)
{
	lua_pushinteger(L, (lua_Integer)check_alloc(L, 1, MGOLD_ARENA)->size);
	return 1;
}

static int arena_getUsed(lua_State *L)
{
	Storage *st = check_alloc(L, 1, MGOLD_ARENA);
	size_t used = 0;
	for (size_t i = 0; i < st->segs.size(); i++)
		if (st->segs[i].life)
			used += st->segs[i].size;
	lua_pushinteger(L, (lua_Integer)used);
	return 1;
}

static int arena_getLargestFree(lua_State *L)
{
	Storage *st = check_alloc(L, 1, MGOLD_ARENA);
	size_t best = 0;
	for (size_t i = 0; i < st->segs.size(); i++)
		if (!st->segs[i].life && st->segs[i].size > best)
			best = st->segs[i].size;
	lua_pushinteger(L, (lua_Integer)best);
	return 1;
}

static int arena_destroy(lua_State *L) { return alloc_destroy(L, MGOLD_ARENA); }
static int arena_isDestroyed(lua_State *L) { return alloc_isDestroyed(L, MGOLD_ARENA); }

static const luaL_Reg arena_methods[] = {
	{ "alloc", arena_alloc },
	{ "free", arena_free },
	{ "reset", arena_reset },
	{ "getCapacity", arena_getCapacity },
	{ "getUsed", arena_getUsed },
	{ "getLargestFree", arena_getLargestFree },
	{ "destroy", arena_destroy },
	{ "isDestroyed", arena_isDestroyed },
	{ nullptr, nullptr }
};




static Storage *buildPool(lua_State *L, int itemIdx, size_t count, char *err)
{
	LayoutPtr lay;
	size_t stride = 0;

	if (lua_type(L, itemIdx) == LUA_TNUMBER)
	{
		lua_Integer s = lua_tointeger(L, itemIdx);
		if (s <= 0)
		{
			setErr(err, "Invalid item size");
			return nullptr;
		}
		stride = (size_t)s;
	}
	else
	{
		if (!compileLayout(L, itemIdx, lay, err, 0))
			return nullptr;
		stride = lay->size;
	}

	if (stride == 0 || count > ((size_t)1 << 30) / stride)
	{
		setErr(err, "Pool too large");
		return nullptr;
	}

	Storage *st = stNew(stride * count);
	if (!st)
	{
		setErr(err, "Out of memory");
		return nullptr;
	}
	st->stride = stride;
	st->layout = lay;
	st->slots.assign(count, nullptr);
	poolReset(st);
	return st;
}


static int w_pool(lua_State *L)
{
	lua_Integer count = luaL_checkinteger(L, 2);
	if (count <= 0 || count > 0x7fffffff)
		return luaL_error(L, "Invalid pool capacity");

	mgold_alloc *a = newAllocUD(L, MGOLD_POOL);

	char err[MGOLD_ERRLEN];
	err[0] = 0;
	Storage *st = buildPool(L, 1, (size_t)count, err);
	if (!st)
		return luaL_error(L, "%s", err);

	st->refs = 1;
	a->st = st;
	return 1;
}


static int pool_alloc(lua_State *L)
{
	Storage *st = check_alloc(L, 1, MGOLD_POOL);
	if (st->freeList.empty())
		return luaL_error(L, "Pool is full");

	bool hasValues = !lua_isnoneornil(L, 2);
	if (hasValues && !st->layout)
		return luaL_error(L, "Pool was created with a byte size, not a struct");

	mgold_mem *m = newHandle(L);

	uint32_t idx = st->freeList.back();
	st->freeList.pop_back();

	Life *life = lifeNew();
	lifeRef(life);          
	st->slots[idx] = life;
	lifeRef(life);          

	uint8_t *p = st->data + (size_t)idx * st->stride;
	memset(p, 0, st->stride);
	initHandle(m, st, life, (size_t)idx * st->stride, st->stride, K_POOL, false);

	if (hasValues)
	{
		char err[MGOLD_ERRLEN];
		err[0] = 0;
		if (!writeLayout(L, *st->layout, 2, p, err, 0))
		{
			mem_release(m);
			return luaL_error(L, "%s", err);
		}
	}
	return 1;
}

static int pool_free(lua_State *L)
{
	Storage *st = check_alloc(L, 1, MGOLD_POOL);
	mgold_mem *m = (mgold_mem *)luaL_checkudata(L, 2, MGOLD_MEMORY);
	if (m->kind != K_POOL || m->st != st)
		return luaL_error(L, "Memory does not belong to this pool");
	mem_release(m);
	return 0;
}

static int pool_reset(lua_State *L)
{
	poolReset(check_alloc(L, 1, MGOLD_POOL));
	return 0;
}

static int pool_getItemSize(lua_State *L)
{
	lua_pushinteger(L, (lua_Integer)check_alloc(L, 1, MGOLD_POOL)->stride);
	return 1;
}

static int pool_getCapacity(lua_State *L)
{
	lua_pushinteger(L, (lua_Integer)check_alloc(L, 1, MGOLD_POOL)->slots.size());
	return 1;
}

static int pool_getUsed(lua_State *L)
{
	Storage *st = check_alloc(L, 1, MGOLD_POOL);
	lua_pushinteger(L, (lua_Integer)(st->slots.size() - st->freeList.size()));
	return 1;
}

static int pool_destroy(lua_State *L) { return alloc_destroy(L, MGOLD_POOL); }
static int pool_isDestroyed(lua_State *L) { return alloc_isDestroyed(L, MGOLD_POOL); }

static const luaL_Reg pool_methods[] = {
	{ "alloc", pool_alloc },
	{ "free", pool_free },
	{ "reset", pool_reset },
	{ "getItemSize", pool_getItemSize },
	{ "getCapacity", pool_getCapacity },
	{ "getUsed", pool_getUsed },
	{ "destroy", pool_destroy },
	{ "isDestroyed", pool_isDestroyed },
	{ nullptr, nullptr }
};



static int w_stack(lua_State *L)
{
	lua_Integer size = checkSize(L, 1);
	mgold_alloc *a = newAllocUD(L, MGOLD_STACK);

	Storage *st = stNew((size_t)size);
	if (!st)
		return luaL_error(L, "Out of memory");
	st->refs = 1;
	a->st = st;
	return 1;
}


static int stack_push(lua_State *L)
{
	Storage *st = check_alloc(L, 1, MGOLD_STACK);

	char err[MGOLD_ERRLEN];
	err[0] = 0;
	size_t off = 0;
	bool ok;
	{
		TypeRef t;
		ok = resolveType(L, 2, t, err);
		if (ok && lua_gettop(L) < 3)
		{
			setErr(err, "missing value");
			ok = false;
		}
		size_t sz = 0;
		if (ok)
			ok = valueSize(L, t, 3, &sz, err);
		if (ok)
		{
			size_t start = alignUp(st->top, t.align());
			if (start > st->size || sz > st->size - start)
			{
				setErr(err, "Stack overflow");
				ok = false;
			}
			else
			{
				memset(st->data + start, 0, sz);
				ok = writeType(L, t, 3, st->data + start, err, sz - (isStrType(t) ? 4 : 0));
				if (ok)
				{
					Entry e;
					e.off = start;
					e.size = sz;
					e.prevTop = st->top;
					st->entries.push_back(e);
					st->top = start + sz;
					off = start;
				}
			}
		}
	}
	if (!ok)
		return luaL_error(L, "%s", err);

	lua_pushinteger(L, (lua_Integer)off);
	return 1;
}

static int stackPopImpl(lua_State *L, bool remove)
{
	Storage *st = check_alloc(L, 1, MGOLD_STACK);
	if (st->entries.empty())
		return luaL_error(L, "Stack underflow");

	Entry e = st->entries.back();

	char err[MGOLD_ERRLEN];
	err[0] = 0;
	bool ok;
	{
		TypeRef t;
		ok = resolveType(L, 2, t, err);
		if (ok && isStrType(t))
		{
			size_t real = 0;
			ok = storedStringSize(st->data + e.off, e.size, &real, err);
			if (ok && real != e.size)
			{
				setErr(err, "Top entry is not a string");
				ok = false;
			}
		}
		else if (ok && t.size() != e.size)
		{
			setErr(err, "Type size (%llu) does not match top entry size (%llu)",
				(unsigned long long)t.size(), (unsigned long long)e.size);
			ok = false;
		}
		if (ok)
			readType(L, t, st->data + e.off, isStrType(t) ? e.size - 4 : 0);
	}
	if (!ok)
		return luaL_error(L, "%s", err);

	if (remove)
	{
		st->top = e.prevTop;
		st->entries.pop_back();
	}
	return 1;
}

static int stack_pop(lua_State *L) { return stackPopImpl(L, true); }
static int stack_peek(lua_State *L) { return stackPopImpl(L, false); }

static int stack_clear(lua_State *L)
{
	Storage *st = check_alloc(L, 1, MGOLD_STACK);
	st->top = 0;
	st->entries.clear();
	return 0;
}

static int stack_getTop(lua_State *L)
{
	lua_pushinteger(L, (lua_Integer)check_alloc(L, 1, MGOLD_STACK)->top);
	return 1;
}

static int stack_getCount(lua_State *L)
{
	lua_pushinteger(L, (lua_Integer)check_alloc(L, 1, MGOLD_STACK)->entries.size());
	return 1;
}

static int stack_getCapacity(lua_State *L)
{
	lua_pushinteger(L, (lua_Integer)check_alloc(L, 1, MGOLD_STACK)->size);
	return 1;
}

static int stack_destroy(lua_State *L) { return alloc_destroy(L, MGOLD_STACK); }
static int stack_isDestroyed(lua_State *L) { return alloc_isDestroyed(L, MGOLD_STACK); }

static const luaL_Reg stack_methods[] = {
	{ "push", stack_push },
	{ "pop", stack_pop },
	{ "peek", stack_peek },
	{ "clear", stack_clear },
	{ "getTop", stack_getTop },
	{ "getCount", stack_getCount },
	{ "getCapacity", stack_getCapacity },
	{ "destroy", stack_destroy },
	{ "isDestroyed", stack_isDestroyed },
	{ nullptr, nullptr }
};





static void registerMetatables(lua_State *L)
{
	
	luaL_newmetatable(L, MGOLD_MEMORY);
	lua_pushcfunction(L, memory_index);    lua_setfield(L, -2, "__index");
	lua_pushcfunction(L, memory_newindex); lua_setfield(L, -2, "__newindex");
	lua_pushcfunction(L, memory_tostring); lua_setfield(L, -2, "__tostring");
	lua_pushcfunction(L, memory_gc);       lua_setfield(L, -2, "__gc");
	lua_pop(L, 1);

	
	luaL_newmetatable(L, MGOLD_LAYOUT);
	lua_newtable(L);
	setFuncs(L, layout_methods);
	lua_setfield(L, -2, "__index");
	lua_pushcfunction(L, layout_gc);       lua_setfield(L, -2, "__gc");
	lua_pop(L, 1);

	
	const char *names[3] = { MGOLD_ARENA, MGOLD_POOL, MGOLD_STACK };
	const luaL_Reg *methods[3] = { arena_methods, pool_methods, stack_methods };
	for (int i = 0; i < 3; i++)
	{
		luaL_newmetatable(L, names[i]);
		lua_newtable(L);
		setFuncs(L, methods[i]);
		lua_setfield(L, -2, "__index");
		lua_pushcfunction(L, alloc_gc);    lua_setfield(L, -2, "__gc");
		lua_pop(L, 1);
	}
}

void MGOLD_setStdlibFunc(lua_State *L)
{
	lua_newtable(L);

	registerMetatables(L);

	
	lua_newtable(L);
	lua_pushcfunction(L, w_malloc); lua_setfield(L, -2, "malloc");
	lua_pushcfunction(L, w_arena);  lua_setfield(L, -2, "arena");
	lua_pushcfunction(L, w_pool);   lua_setfield(L, -2, "pool");
	lua_pushcfunction(L, w_stack);  lua_setfield(L, -2, "stack");
	lua_setfield(L, -2, "alloc");

	
	lua_newtable(L);
	lua_pushcfunction(L, w_sizeof);  lua_setfield(L, -2, "sizeof");
	lua_pushcfunction(L, w_alignof); lua_setfield(L, -2, "alignof");
	lua_pushcfunction(L, w_struct);  lua_setfield(L, -2, "struct");
	lua_setfield(L, -2, "sizes");

	
	lua_newtable(L);
	lua_pushcfunction(L, w_unpack); lua_setfield(L, -2, "unpack");
	lua_pushcfunction(L, w_pack);   lua_setfield(L, -2, "pack");
	lua_setfield(L, -2, "package");
}

}
}
