#include "modules/mgold/Tab.h"

#include "common/runtime.h"
#include "common/Module.h"

#include <stdint.h>
#include <string.h>
#include <math.h>
#include <string>
#include <new>


/*

HEADER
char[4] magic => "TAB\0"
uint64_t def_size
uint8_t version

TYPES
0 int
1 float
2 char
3 bool false
4 bool true
5 empty string
6 string
7 empty table
8 table

DEF TYPES
int => (uint8_t)<type><data>
float => (uint8_t)<type><data>
char => (uint8_t)<type><data>

string => (uint8_t)<type>(uint64_t)<length><data>
empty_string => <type>

empty_table => <type>
table => (uint8_t)<type>(uint64_t)<length><table>

TABLE
(uint64_t)<index size>
(uint64_t)<index><def>
...
(uint64_t)<index><def>
(uint64_t)<key size>
(uint64_t)<length><key><def>
...
(uint64_t)<length><key><def>


o resultado de {1,2,3, k1 = 0, k2 = 3}:
<TYPE(TABLE)><LENGTH><DATA>

DATA:
(uint64_t)<KEY COUNT>
<DEF(key)><DEF(value)>

*/

namespace
{

enum TabType
{
	TAB_INT = 0,
	TAB_FLOAT = 1,
	TAB_CHAR = 2,
	TAB_BOOL_FALSE = 3,
	TAB_BOOL_TRUE = 4,
	TAB_EMPTY_STRING = 5,
	TAB_STRING = 6,
	TAB_EMPTY_TABLE = 7,
	TAB_TABLE = 8
};

const size_t TAB_HEADER_SIZE = 13;
const int TAB_MAX_DEPTH = 200;
const uint8_t TAB_VERSION = 255;

typedef char tab_number_size_check[(sizeof(lua_Number) == 8) ? 1 : -1];

bool isLittleEndian()
{
	const uint16_t x = 1;
	return *(const uint8_t *)&x == 1;
}

void copyBE(void *dst, const void *src, size_t n)
{
	uint8_t *d = (uint8_t *)dst;
	const uint8_t *s = (const uint8_t *)src;

	if (isLittleEndian())
	{
		for (size_t i = 0; i < n; i++)
			d[i] = s[n - 1 - i];
	}
	else
	{
		memcpy(d, s, n);
	}
}

int numberToRaw(lua_State *L)
{
	lua_Number val = luaL_checknumber(L, 1);
	uint8_t buf[sizeof(lua_Number)];
	copyBE(buf, &val, sizeof(val));
	lua_pushlstring(L, (const char *)buf, sizeof(buf));
	return 1;
}

int rawToNumber(lua_State *L)
{
	size_t size = 0;
	const char *str = luaL_checklstring(L, 1, &size);

	if (size != sizeof(lua_Number))
		return luaL_error(L, "size of string is different than a lua_Number [%d]", (int)size);

	lua_Number value;
	copyBE(&value, str, sizeof(value));
	lua_pushnumber(L, value);
	return 1;
}

struct Reader
{
	const uint8_t *data;
	size_t size;
	size_t pos;
};

bool readBytes(Reader &r, void *dst, size_t n)
{
	if (n > r.size - r.pos)
		return false;
	memcpy(dst, r.data + r.pos, n);
	r.pos += n;
	return true;
}

bool readU8(Reader &r, uint8_t &v)
{
	return readBytes(r, &v, 1);
}

bool readU64(Reader &r, uint64_t &v)
{
	uint8_t tmp[8];
	if (!readBytes(r, tmp, 8))
		return false;
	copyBE(&v, tmp, 8);
	return true;
}

bool readNumber(Reader &r, lua_Number &v)
{
	uint8_t tmp[8];
	if (!readBytes(r, tmp, 8))
		return false;
	copyBE(&v, tmp, 8);
	return true;
}

const char *unpackValue(lua_State *L, Reader &r, int depth)
{
	const char *truncated = "unexpected end of data";

	if (depth > TAB_MAX_DEPTH)
		return "table nesting too deep";

	if (!lua_checkstack(L, 4))
		return "stack overflow";

	uint8_t type;
	if (!readU8(r, type))
		return truncated;

	switch (type)
	{
	case TAB_INT:
	case TAB_FLOAT:
	{
		lua_Number n;
		if (!readNumber(r, n))
			return truncated;
		lua_pushnumber(L, n);
		return NULL;
	}

	case TAB_CHAR:
	{
		uint8_t c;
		if (!readU8(r, c))
			return truncated;
		lua_pushlstring(L, (const char *)&c, 1);
		return NULL;
	}

	case TAB_BOOL_FALSE:
		lua_pushboolean(L, 0);
		return NULL;

	case TAB_BOOL_TRUE:
		lua_pushboolean(L, 1);
		return NULL;

	case TAB_EMPTY_STRING:
		lua_pushlstring(L, "", 0);
		return NULL;

	case TAB_STRING:
	{
		uint64_t len;
		if (!readU64(r, len))
			return truncated;
		if (len > (uint64_t)(r.size - r.pos))
			return "string length out of bounds";
		lua_pushlstring(L, (const char *)(r.data + r.pos), (size_t)len);
		r.pos += (size_t)len;
		return NULL;
	}

	case TAB_EMPTY_TABLE:
		lua_newtable(L);
		return NULL;

	case TAB_TABLE:
	{
		uint64_t count;
		if (!readU64(r, count))
			return truncated;

		if (count > (uint64_t)((r.size - r.pos) / 2))
			return "key count out of bounds";

		int hint = count > 65536 ? 65536 : (int)count;
		lua_createtable(L, 0, hint);
		int tbl = lua_gettop(L);

		for (uint64_t i = 0; i < count; i++)
		{
			const char *err = unpackValue(L, r, depth + 1);
			if (err)
				return err;

			if (lua_type(L, -1) == LUA_TNUMBER && lua_tonumber(L, -1) != lua_tonumber(L, -1))
				return "NaN used as table key";

			err = unpackValue(L, r, depth + 1);
			if (err)
				return err;

			lua_rawset(L, tbl);
		}
		return NULL;
	}

	default:
		return "unknown type";
	}
}


bool isPackable(int t)
{
	return t == LUA_TNUMBER || t == LUA_TBOOLEAN || t == LUA_TSTRING || t == LUA_TTABLE;
}

void putBE(std::string &out, const void *src, size_t n)
{
	uint8_t tmp[8];
	copyBE(tmp, src, n);
	out.append((const char *)tmp, n);
}

const char *packValue(lua_State *L, int idx, std::string &out, int depth)
{
	if (idx < 0)
		idx = lua_gettop(L) + idx + 1;

	if (depth > TAB_MAX_DEPTH)
		return "table nesting too deep (cyclic table?)";

	if (!lua_checkstack(L, 6))
		return "stack overflow";

	switch (lua_type(L, idx))
	{
	case LUA_TNUMBER:
	{
		lua_Number v = lua_tonumber(L, idx);
		bool isInt = (v >= -9007199254740992.0 && v <= 9007199254740992.0 && v == floor(v));
		out.push_back((char)(isInt ? TAB_INT : TAB_FLOAT));
		putBE(out, &v, sizeof(v));
		return NULL;
	}

	case LUA_TBOOLEAN:
		out.push_back((char)(lua_toboolean(L, idx) ? TAB_BOOL_TRUE : TAB_BOOL_FALSE));
		return NULL;

	case LUA_TSTRING:
	{
		size_t size;
		const char *str = lua_tolstring(L, idx, &size);

		if (size == 0)
		{
			out.push_back((char)TAB_EMPTY_STRING);
		}
		else if (size == 1)
		{
			out.push_back((char)TAB_CHAR);
			out.push_back(str[0]);
		}
		else
		{
			uint64_t len = (uint64_t)size;
			out.push_back((char)TAB_STRING);
			putBE(out, &len, 8);
			out.append(str, size);
		}
		return NULL;
	}

	case LUA_TTABLE:
	{
		size_t start = out.size();
		out.push_back((char)TAB_TABLE);
		out.append(8, '\0');

		uint64_t count = 0;

		lua_pushnil(L);
		while (lua_next(L, idx) != 0)
		{
			int kt = lua_type(L, -2);
			int vt = lua_type(L, -1);

			if (isPackable(kt) && isPackable(vt))
			{
				int top = lua_gettop(L);
				const char *err = packValue(L, top - 1, out, depth + 1);
				if (err)
					return err;
				err = packValue(L, top, out, depth + 1);
				if (err)
					return err;
				count++;
			}

			lua_pop(L, 1);
		}

		if (count == 0)
		{
			out.resize(start + 1);
			out[start] = (char)TAB_EMPTY_TABLE;
		}
		else
		{
			uint8_t tmp[8];
			copyBE(tmp, &count, 8);
			memcpy(&out[start + 1], tmp, 8);
		}
		return NULL;
	}

	default:
		return "unsupported type";
	}
}

int lua_pack(lua_State *L)
{
	luaL_checkany(L, 1);

	const char *err = NULL;
	bool ok = false;

	if (!isPackable(lua_type(L, 1)))
	{
		lua_pushnil(L);
		lua_pushstring(L, "unsupported type");
		return 2;
	}

	try
	{
		std::string out;
		out.reserve(64);

		out.append("TAB\0", 4);
		out.push_back((char)TAB_VERSION);
		out.append(8, '\0');

		int top = lua_gettop(L);
		err = packValue(L, 1, out, 0);
		lua_settop(L, top);

		if (!err)
		{
			uint64_t def_size = (uint64_t)(out.size() - TAB_HEADER_SIZE);
			uint8_t tmp[8];
			copyBE(tmp, &def_size, 8);
			memcpy(&out[5], tmp, 8);

			lua_pushlstring(L, out.data(), out.size());
			ok = true;
		}
	}
	catch (std::bad_alloc &)
	{
		err = "out of memory";
	}

	if (ok)
		return 1;

	lua_pushnil(L);
	lua_pushstring(L, err ? err : "unknown error");
	return 2;
}

int lua_unpack_tab(lua_State *L)
{
	size_t size = 0;
	const char *s = luaL_checklstring(L, 1, &size);

	if (size < TAB_HEADER_SIZE + 1)
	{
		lua_pushnil(L);
		lua_pushstring(L, "data too short");
		return 2;
	}

	if (memcmp(s, "TAB\0", 4) != 0)
	{
		lua_pushnil(L);
		lua_pushstring(L, "invalid magic");
		return 2;
	}

	Reader r;
	r.data = (const uint8_t *)s;
	r.size = size;
	r.pos = 5;
	uint64_t def_size = 0;
	readU64(r, def_size);

	if (def_size != (uint64_t)(size - TAB_HEADER_SIZE))
	{
		lua_pushnil(L);
		lua_pushstring(L, "definition size mismatch");
		return 2;
	}

	int top = lua_gettop(L);
	const char *err = unpackValue(L, r, 0);

	if (err)
	{
		lua_settop(L, top);
		lua_pushnil(L);
		lua_pushstring(L, err);
		return 2;
	}

	return 1;
}

int lua_tpack(lua_State *L)
{
	luaL_checkany(L, 1);

	if (!isPackable(lua_type(L, 1)))
	{
		lua_pushnil(L);
		return 1;
	}

	const char *err = NULL;
	bool ok = false;

	try
	{
		std::string out;
		int top = lua_gettop(L);
		err = packValue(L, 1, out, 0);
		lua_settop(L, top);

		if (!err)
		{
			lua_pushlstring(L, out.data(), out.size());
			ok = true;
		}
	}
	catch (std::bad_alloc &)
	{
		err = "out of memory";
	}

	if (ok)
		return 1;

	lua_pushnil(L);
	lua_pushstring(L, err);
	return 2;
}

int lua_tunpack(lua_State *L)
{
	size_t size = 0;
	const char *s = luaL_checklstring(L, 1, &size);

	Reader r;
	r.data = (const uint8_t *)s;
	r.size = size;
	r.pos = 0;

	int top = lua_gettop(L);
	const char *err = unpackValue(L, r, 0);

	if (err)
	{
		lua_settop(L, top);
		lua_pushnil(L);
		lua_pushstring(L, err);
		return 2;
	}

	return 1;
}

}
extern "C" {

	void MGOLD_setTabFunc(lua_State *L)
	{
		static const luaL_Reg funcs[] = {
			{ "intToRaw",    numberToRaw },
			{ "numberToRaw", numberToRaw },
			{ "rawToInt",    rawToNumber },
			{ "rawToNumber", rawToNumber },
			{ "unpack",      lua_unpack_tab },
			{ "pack",        lua_pack },
			{ "tpack",       lua_tpack },
			{ "tunpack",     lua_tunpack },
			{ NULL, NULL }
		};

		lua_newtable(L);

		for (const luaL_Reg *f = funcs; f->name != NULL; f++)
		{
			lua_pushcfunction(L, f->func);
			lua_setfield(L, -2, f->name);
		}
	}

}
