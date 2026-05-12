#include "HLEVideoProvider.h"
/***************************************************************/
/* Copyright 2026 Xenon Emulator Project. All rights reserved. */
/***************************************************************/

#include <bit>

#include "HLEVideoProvider.h"

#include "Base/Logging/Log.h"
#include "Core/HLE/HLEFunctionManager.h"
#include "Core/XCPU/Interpreter/PPCInterpreter.h"

#include "../xenia/gpu/xenos.h"

extern uint8_t *fbAddressPtr;
extern uint8_t *internalWidthPtr;
extern uint8_t *internalHeightPtr;

namespace Xe {
  namespace Core {
    namespace HLE {

      bool HLEVideoProvider::Setup() {
        LOG_INFO(HLE, "[Video]: Video HLE provider initialized.");
        return true;
      }

      void HLEVideoProvider::CollectFunctions(std::vector<sHLEFunction> &table) {
        // xboxkrnl ordinals for video functions (taken from xboxkrnl 2.0.17489 dev)
        constexpr u32 ordVdRetrainEDRAM = 617;
        constexpr u32 ordVdIsHSIOTrainingSucceeded = 454;
        constexpr u32 ordVdSwap = 603;
        constexpr u32 ordVdQueryVideoMode = 458;
        constexpr u32 ordVdSetDisplayMode = 467;

        table.emplace_back(sHLEFunction{ true, ordVdRetrainEDRAM, eSystemExecutables::xboxkrnl, 0x800FC218, "VdRetrainEDRAM",
          reinterpret_cast<HLEFunctionPtr>(HLE_VdRetrainEDRAM) });
        table.emplace_back(sHLEFunction{ true, ordVdIsHSIOTrainingSucceeded, eSystemExecutables::xboxkrnl, 0x800F9128, "VdIsHSIOTrainingSucceeded",
          reinterpret_cast<HLEFunctionPtr>(HLE_VdIsHSIOTrainingSucceeded) });
        table.emplace_back(sHLEFunction{ true, ordVdSwap, eSystemExecutables::xboxkrnl, 0x800F8E20, "VdSwap",
          reinterpret_cast<HLEFunctionPtr>(HLE_VdSwap), false });

        LOG_INFO(HLE, "[Video]: Registered {} video HLE functions.", 4);
      }

      // VdRetrainEDRAM
      // Returns 0, STATUS_SUCCESS
      void HLEVideoProvider::HLE_VdRetrainEDRAM(sPPEState *ppeState) {
        sPPUThread &thread = ppeState->ppuThread[PPCInterpreter::tl_curThreadId];
        thread.GPR[3] = 0;
      }

      // VdIsHSIOTrainingSucceeded
      // Returns 1 - HSIO training always succeeds
      void HLEVideoProvider::HLE_VdIsHSIOTrainingSucceeded(sPPEState *ppeState) {
        sPPUThread &thread = ppeState->ppuThread[PPCInterpreter::tl_curThreadId]; 
        thread.GPR[3] = 1;
      }

      // VdSwap
      void HLEVideoProvider::HLE_VdSwap(sPPEState *ppeState) {
        // Get current thread
        sPPUThread &thread = ppeState->ppuThread[PPCInterpreter::tl_curThreadId];

        namespace xenos = xe::gpu::xenos;
        // Get GPU Fetch RAM pointer
        u8 *gpuFetchPtr = PPCInterpreter::MMUGetPointerFromRAM(thread.GPR[4], ppeState, true);       
        if (gpuFetchPtr == nullptr) {
          return;
        }

        xenos::xe_gpu_texture_fetch_t gpu_fetch;
        xe::copy_and_swap_32_unaligned(&gpu_fetch, reinterpret_cast<uint32_t *>(gpuFetchPtr), 6);

        u32 fbAddress = 0;
        u32 fbWidth = 0;
        u32 fbHeight = 0;

        memcpy(&fbAddress, fbAddressPtr, 4);
        memcpy(&fbWidth, internalWidthPtr, 4);
        memcpy(&fbHeight, internalHeightPtr, 4);


        // Mask to GPU address space and add 4Kb page due to a design bug in the system 
        gpu_fetch.base_address = fbAddress >> 12;// (gpu_fetch.base_address & 0x1FFFF) + 0x1;

        u32 width = fbWidth;// gpu_fetch.size_2d.width + 1;

        u32 height = fbHeight;// gpu_fetch.size_2d.height + 1;


        // Pitch
        gpu_fetch.pitch = fbWidth >> 5;

        // Get ringbuffer pointer
        u32 *ringbufferPtr = reinterpret_cast<u32 *>(PPCInterpreter::MMUGetPointerFromRAM(thread.GPR[3], ppeState, true));
        if (ringbufferPtr == nullptr) {
          return;
        }

        u32 bufferOffset = 0;
        ringbufferPtr[bufferOffset++] = byteswap_be<u32>(xenos::MakePacketType0(0x4800, 6));
        ringbufferPtr[bufferOffset++] = byteswap_be<u32>(gpu_fetch.dword_0);
        ringbufferPtr[bufferOffset++] = byteswap_be<u32>(gpu_fetch.dword_1);
        ringbufferPtr[bufferOffset++] = byteswap_be<u32>(gpu_fetch.dword_2);
        ringbufferPtr[bufferOffset++] = byteswap_be<u32>(gpu_fetch.dword_3);
        ringbufferPtr[bufferOffset++] = byteswap_be<u32>(gpu_fetch.dword_4);
        ringbufferPtr[bufferOffset++] = byteswap_be<u32>(gpu_fetch.dword_5);

        ringbufferPtr[bufferOffset++] = byteswap_be<u32>(xenos::MakePacketType3(xenos::PM4_XE_SWAP, 4));
        ringbufferPtr[bufferOffset++] = byteswap_be<u32>(xe::gpu::xenos::kSwapSignature);
        ringbufferPtr[bufferOffset++] = byteswap_be<u32>(fbAddress);

        ringbufferPtr[bufferOffset++] = byteswap_be<u32>(1280);
        ringbufferPtr[bufferOffset++] = byteswap_be<u32>(720);

        // Fill the rest of the buffer with NOP packets.
        for (uint32_t i = bufferOffset; i < 64; i++) {
          ringbufferPtr[i] = byteswap_be<u32>(xenos::MakePacketType2());
        }

        // Advance ringbuffer pointer
        thread.GPR[3] += bufferOffset * 4;
      }
    }
  }
}
