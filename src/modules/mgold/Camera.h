#pragma once

#include "common/runtime.h"

namespace love
{
namespace mgold
{

void MGOLD_setCameraFunc(lua_State *L);

void MGOLD_cameraSetPosition(double x, double y, double z);
double MGOLD_cameraYaw();
void MGOLD_cameraLookVector(double &x, double &y, double &z);
bool MGOLD_cameraLineOffset(double x, double y, double z, double dx, double dy, double dz,
                            double lineWidth, double viewportHeight,
                            double &offsetX, double &offsetY, double &offsetZ);

void MGOLD_sendCameraMatrix(lua_State *L);

}
}