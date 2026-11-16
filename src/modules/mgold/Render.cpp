#define PI 3.14159265358979323846
#include "modules/mgold/Render.h"
#include "modules/mgold/Shader.h"
#include "modules/mgold/Camera.h"
#include "modules/mgold/Object.h"
#include "modules/mgold/Light.h"

#include "common/runtime.h"
#include "common/Module.h"
#include "modules/graphics/Graphics.h"
#include "modules/graphics/Mesh.h"
#include "modules/graphics/Shader.h"
#include "modules/graphics/wrap_Shader.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace love
{
namespace mgold
{

#define instance() (Module::getInstance<graphics::Graphics>(Module::M_GRAPHICS))

namespace
{

struct Point3
{
	double x, y, z;
};

struct Rotation3
{
	double x, y, z;
};

const int PRIMITIVE_VERTEX_FLOATS = 32;

Rotation3 checkRotation(lua_State *L, int firstArgument)
{
	Rotation3 rotation = {
		luaL_optnumber(L, firstArgument, 0.0),
		luaL_optnumber(L, firstArgument + 1, 0.0),
		luaL_optnumber(L, firstArgument + 2, 0.0),
	};
	if (!std::isfinite(rotation.x) || !std::isfinite(rotation.y) || !std::isfinite(rotation.z))
	{
		luaL_argerror(L, firstArgument, "rotation angles must be finite");
		return { 0.0, 0.0, 0.0 };
	}
	return rotation;
}

Point3 rotatePoint(const Point3 &point, const Point3 &center, const Rotation3 &rotation)
{
	const double x = point.x - center.x;
	const double y = point.y - center.y;
	const double z = point.z - center.z;
	const double cosX = std::cos(rotation.x), sinX = std::sin(rotation.x);
	const double cosY = std::cos(rotation.y), sinY = std::sin(rotation.y);
	const double cosZ = std::cos(rotation.z), sinZ = std::sin(rotation.z);

	const double y1 = y * cosX - z * sinX;
	const double z1 = y * sinX + z * cosX;
	const double x2 = x * cosY + z1 * sinY;
	const double z2 = -x * sinY + z1 * cosY;
	const double x3 = x2 * cosZ - y1 * sinZ;
	const double y3 = x2 * sinZ + y1 * cosZ;
	return { center.x + x3, center.y + y3, center.z + z2 };
}

Point3 rotateVector(const Point3 &vector, const Rotation3 &rotation)
{
	const Point3 origin = { 0.0, 0.0, 0.0 };
	return rotatePoint(vector, origin, rotation);
}

const std::vector<graphics::Buffer::DataDeclaration> &primitiveVertexFormat()
{
	static const std::vector<graphics::Buffer::DataDeclaration> format = {
		{ "VertexPosition", graphics::DATAFORMAT_FLOAT_VEC3, 0, 0 },
		{ "VertexTexCoord", graphics::DATAFORMAT_FLOAT_VEC2, 0, 1 },
		{ "VertexColor", graphics::DATAFORMAT_FLOAT_VEC4, 0, 2 },
		{ "vertNormal", graphics::DATAFORMAT_FLOAT_VEC3, 0, 3 },
		{ "InstanceMat1", graphics::DATAFORMAT_FLOAT_VEC4, 0, 4 },
		{ "InstanceMat2", graphics::DATAFORMAT_FLOAT_VEC4, 0, 5 },
		{ "InstanceMat3", graphics::DATAFORMAT_FLOAT_VEC4, 0, 6 },
		{ "InstanceMat4", graphics::DATAFORMAT_FLOAT_VEC4, 0, 7 },
		{ "InstanceColor", graphics::DATAFORMAT_FLOAT_VEC4, 0, 8 },
	};
	return format;
}

void appendVertex(std::vector<float> &vertices, const Point3 &p, const Point3 &normal)
{
	const float vertex[PRIMITIVE_VERTEX_FLOATS] = {
		(float)p.x, (float)p.y, (float)p.z,
		0.0f, 0.0f,
		1.0f, 1.0f, 1.0f, 1.0f,
		(float)normal.x, (float)normal.y, (float)normal.z,
		1.0f, 0.0f, 0.0f, 0.0f,
		0.0f, 1.0f, 0.0f, 0.0f,
		0.0f, 0.0f, 1.0f, 0.0f,
		0.0f, 0.0f, 0.0f, 1.0f,
		1.0f, 1.0f, 1.0f, 1.0f,
	};
	vertices.insert(vertices.end(), vertex, vertex + PRIMITIVE_VERTEX_FLOATS);
}

void appendTriangle(std::vector<float> &vertices, const Point3 &a, const Point3 &b,
                    const Point3 &c, const Point3 &normal)
{
	appendVertex(vertices, a, normal);
	appendVertex(vertices, b, normal);
	appendVertex(vertices, c, normal);
}

void appendLine(std::vector<float> &vertices, const Point3 &a, const Point3 &b,
                float lineWidth, double viewportHeight)
{
	double dx = b.x - a.x, dy = b.y - a.y, dz = b.z - a.z;
	const double length = std::sqrt(dx * dx + dy * dy + dz * dz);
	if (length <= 1e-12)
		return;
	dx /= length;
	dy /= length;
	dz /= length;

	double ax, ay, az, bx, by, bz;
	if (!MGOLD_cameraLineOffset(a.x, a.y, a.z, dx, dy, dz, lineWidth, viewportHeight, ax, ay, az) ||
	    !MGOLD_cameraLineOffset(b.x, b.y, b.z, dx, dy, dz, lineWidth, viewportHeight, bx, by, bz))
		return;

	const Point3 aPlus = { a.x + ax, a.y + ay, a.z + az };
	const Point3 aMinus = { a.x - ax, a.y - ay, a.z - az };
	const Point3 bPlus = { b.x + bx, b.y + by, b.z + bz };
	const Point3 bMinus = { b.x - bx, b.y - by, b.z - bz };
	const Point3 normal = { 0.0, 0.0, 1.0 };
	appendTriangle(vertices, aPlus, aMinus, bPlus, normal);
	appendTriangle(vertices, bPlus, aMinus, bMinus, normal);
}

void drawPrimitive(lua_State *L, const std::vector<float> &vertices)
{
	if (vertices.empty())
		return;

	StrongRef<graphics::Mesh> mesh;
	luax_catchexcept(L, [&]() {
		mesh.set(instance()->newMesh(primitiveVertexFormat(), vertices.data(),
		                             vertices.size() * sizeof(float), graphics::PRIMITIVE_TRIANGLES,
		                             graphics::BUFFERDATAUSAGE_STREAM),
		         Acquire::NORETAIN);
	});

	Matrix4 transform;
	luax_catchexcept(L, [&]() { mesh->draw(instance(), transform); });
}

int checkDrawMode(lua_State *L)
{
	static const char *modes[] = { "fill", "line", nullptr };
	return luaL_checkoption(L, 1, "fill", modes);
}

int w_render_rectangle(lua_State *L)
{
	const int mode = checkDrawMode(L);
	const double x = luaL_checknumber(L, 2);
	const double y = luaL_checknumber(L, 3);
	const double z = luaL_checknumber(L, 4);
	const double width = luaL_checknumber(L, 5);
	const double height = luaL_checknumber(L, 6);
	const int cornerSegments = (int)luaL_optinteger(L, 8, 8);
	const Rotation3 rotation = checkRotation(L, 9);

	if (width < 0.0 || height < 0.0)
		return luaL_argerror(L, 5, "rectangle dimensions must be non-negative");
	if (cornerSegments < 1 || cornerSegments > 128)
		return luaL_argerror(L, 8, "corner segment count must be between 1 and 128");
	if (width == 0.0 || height == 0.0)
		return 0;

	const double radius = std::max(0.0, luaL_optnumber(L, 7, std::min(width, height) * 0.1));
	const double r = std::min(radius, std::min(width, height) * 0.5);
	std::vector<Point3> perimeter;
	if (r == 0.0)
	{
		perimeter = { { x, y, z }, { x + width, y, z },
		              { x + width, y + height, z }, { x, y + height, z } };
	}
	else
	{
		const double centers[4][2] = {
			{ x + width - r, y + r },
			{ x + width - r, y + height - r },
			{ x + r, y + height - r },
			{ x + r, y + r },
		};
		for (int corner = 0; corner < 4; corner++)
		{
			const double startAngle = -PI * 0.5 + corner * PI * 0.5;
			for (int i = 0; i <= cornerSegments; i++)
			{
				const double angle = startAngle + (PI * 0.5 * i / cornerSegments);
				perimeter.push_back({ centers[corner][0] + std::cos(angle) * r,
				                      centers[corner][1] + std::sin(angle) * r, z });
			}
		}
	}

	const Point3 center = { x + width * 0.5, y + height * 0.5, z };
	for (Point3 &point : perimeter)
		point = rotatePoint(point, center, rotation);

	std::vector<float> vertices;
	const Point3 normal = rotateVector({ 0.0, 0.0, 1.0 }, rotation);
	if (mode == 0)
	{
		for (size_t i = 0; i < perimeter.size(); i++)
			appendTriangle(vertices, center, perimeter[i], perimeter[(i + 1) % perimeter.size()], normal);
	}
	else
	{
		const double viewportHeight = instance()->getHeight();
		const float lineWidth = instance()->getLineWidth();
		for (size_t i = 0; i < perimeter.size(); i++)
			appendLine(vertices, perimeter[i], perimeter[(i + 1) % perimeter.size()], lineWidth, viewportHeight);
	}

	drawPrimitive(L, vertices);
	return 0;
}

int w_render_circle(lua_State *L)
{
	const int mode = checkDrawMode(L);
	const double x = luaL_checknumber(L, 2);
	const double y = luaL_checknumber(L, 3);
	const double z = luaL_checknumber(L, 4);
	const double radius = luaL_checknumber(L, 5);
	const int segments = (int)luaL_optinteger(L, 6, 32);
	const Rotation3 rotation = checkRotation(L, 7);
	if (radius < 0.0)
		return luaL_argerror(L, 5, "circle radius must be non-negative");
	if (segments < 3 || segments > 512)
		return luaL_argerror(L, 6, "circle segment count must be between 3 and 512");
	if (radius == 0.0)
		return 0;

	std::vector<Point3> perimeter;
	perimeter.reserve((size_t)segments);
	for (int i = 0; i < segments; i++)
	{
		const double angle = 2.0 * PI * i / segments;
		perimeter.push_back({ x + std::cos(angle) * radius, y + std::sin(angle) * radius, z });
	}

	const Point3 center = { x, y, z };
	for (Point3 &point : perimeter)
		point = rotatePoint(point, center, rotation);

	std::vector<float> vertices;
	const Point3 normal = rotateVector({ 0.0, 0.0, 1.0 }, rotation);
	if (mode == 0)
	{
		for (size_t i = 0; i < perimeter.size(); i++)
			appendTriangle(vertices, center, perimeter[i], perimeter[(i + 1) % perimeter.size()], normal);
	}
	else
	{
		const double viewportHeight = instance()->getHeight();
		const float lineWidth = instance()->getLineWidth();
		for (size_t i = 0; i < perimeter.size(); i++)
			appendLine(vertices, perimeter[i], perimeter[(i + 1) % perimeter.size()], lineWidth, viewportHeight);
	}

	drawPrimitive(L, vertices);
	return 0;
}

int w_render_line(lua_State *L)
{
	const Point3 start = { luaL_checknumber(L, 1), luaL_checknumber(L, 2), luaL_checknumber(L, 3) };
	const Point3 end = { luaL_checknumber(L, 4), luaL_checknumber(L, 5), luaL_checknumber(L, 6) };
	const Rotation3 rotation = checkRotation(L, 7);
	const Point3 center = {
		(start.x + end.x) * 0.5,
		(start.y + end.y) * 0.5,
		(start.z + end.z) * 0.5,
	};
	const Point3 a = rotatePoint(start, center, rotation);
	const Point3 b = rotatePoint(end, center, rotation);
	const double viewportHeight = instance()->getHeight();
	std::vector<float> vertices;
	appendLine(vertices, a, b, instance()->getLineWidth(), viewportHeight);
	drawPrimitive(L, vertices);
	return 0;
}

} // namespace

static int w_render_finish(lua_State *L)
{
	instance()->setShader();
	return 0;
}

static int w_render_begin(lua_State *L)
{
	graphics::Shader *shader = lua_toboolean(L, 1) ? graphics::luax_checkshader(L, 1) : MGOLD_defaultShader(L);

	luax_catchexcept(L, [&]() { instance()->setShader(shader); });

	MGOLD_sendCameraMatrix(L);
	MGOLD_updateLight(L);

	instance()->setColor(Colorf(1.0f, 1.0f, 1.0f, 1.0f));
	return 0;
}

static int w_render_render(lua_State *L)
{
	luaL_checktype(L, 1, LUA_TTABLE); // mesh

	graphics::Mesh *mesh = nullptr;
	int count = 0;
	if (!MGOLD_getMeshDrawInfo(L, 1, mesh, count))
		return luaL_error(L, "render.render: argument #1 is not a mgold mesh");
	if (count <= 0)
		return 0;

	graphics::Shader *customShader = nullptr;
	lua_getfield(L, 1, "shader");
	if (lua_toboolean(L, -1))
		customShader = graphics::luax_checkshader(L, -1);
	lua_pop(L, 1);

	StrongRef<graphics::Shader> oldShader;
	if (customShader && customShader != graphics::Shader::current)
	{
		oldShader.set(graphics::Shader::current);
		luax_catchexcept(L, [&]() { instance()->setShader(customShader); });
	}
	else
		customShader = nullptr;

	instance()->setColor(Colorf(1.0f, 1.0f, 1.0f, 1.0f));

	Matrix4 m;
	luax_catchexcept(L, [&]() { mesh->drawInstanced(instance(), m, count); });

	if (customShader)
	{
		if (oldShader)
		{
			luax_catchexcept(L, [&]() { instance()->setShader(oldShader); });
		}
		else
		{
			instance()->setShader();
		}
	}

	return 0;
}

void MGOLD_setRenderFunc(lua_State *L)
{
	lua_newtable(L); // render

	lua_pushcfunction(L, w_render_finish);
	lua_setfield(L, -2, "finish");

	lua_pushcfunction(L, w_render_begin);
	lua_setfield(L, -2, "begin");

	lua_pushcfunction(L, w_render_render);
	lua_setfield(L, -2, "render");

	lua_pushcfunction(L, w_render_rectangle);
	lua_setfield(L, -2, "rectangle");

	lua_pushcfunction(L, w_render_circle);
	lua_setfield(L, -2, "circle");

	lua_pushcfunction(L, w_render_line);
	lua_setfield(L, -2, "line");
}

} // mgold
} // love
