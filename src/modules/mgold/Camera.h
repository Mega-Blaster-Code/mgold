#pragma once

#include "common/runtime.h"

namespace love
{
namespace mgold
{

void MGOLD_setCameraFunc(lua_State *L);

// acesso nativo (Player.cpp): move a camera mantendo direcao/pitch, le yaw e vetor de olhar
void MGOLD_cameraSetPosition(double x, double y, double z);
double MGOLD_cameraYaw();
void MGOLD_cameraLookVector(double &x, double &y, double &z);

// envia as matrizes da camera ao shader atual (so o que mudou)
void MGOLD_sendCameraMatrix(lua_State *L);

}
}