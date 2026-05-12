// Minimal microprofileui stub for xenia compatibility
// The real microprofile UI is not needed for the D3D12 GPU test harness.
#pragma once

#include "microprofile.h"

#ifndef MICROPROFILEUI_ENABLED
#define MICROPROFILEUI_ENABLED 0
#endif

struct MicroProfileUI {
  int bShowSpikes;
  uint32_t nOpacityBackground;
  uint32_t nOpacityForeground;
};

#ifndef MICROPROFILEUI_IMPL

extern MicroProfileUI g_MicroProfileUI;

inline void MicroProfileDraw(uint32_t nWidth, uint32_t nHeight) {}
inline void MicroProfileMouseButton(uint32_t nLeft, uint32_t nRight) {}
inline void MicroProfileMousePosition(uint32_t nX, uint32_t nY, int nWheelDelta) {}
inline void MicroProfileModKey(uint32_t nKeyState) {}

#else

MicroProfileUI g_MicroProfileUI;

#endif
