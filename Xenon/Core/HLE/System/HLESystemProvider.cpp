/***************************************************************/
/* Copyright 2026 Xenon Emulator Project. All rights reserved. */
/***************************************************************/

#include "HLESystemProvider.h"
#include "Core/XeMain.h"
#include "Base/Logging/Log.h"
#include "Core/HLE/HLEFunctionManager.h"
#include "Core/XCPU/Interpreter/PPCInterpreter.h"

namespace Xe {
  namespace Core {
    namespace HLE {

      bool HLESystemProvider::Setup() {
        LOG_INFO(HLE, "[System]: System HLE provider initialized.");
        return true;
      }

      void HLESystemProvider::CollectFunctions(std::vector<sHLEFunction> &table) {
        // xboxkrnl ordinals for system functions (taken from xboxkrnl 2.0.17489 dev)
        constexpr u32 ordXexLoadImage = 409;
        constexpr u32 ordXexLoadExecutable = 409;

        table.emplace_back(sHLEFunction{ true, ordXexLoadImage, eSystemExecutables::xboxkrnl, 0x800A3F08, "XexLoadImage",
          reinterpret_cast<HLEFunctionPtr>(HLE_XexLoadImage), false });   

        LOG_INFO(HLE, "[System]: Registered {} system HLE functions.", 4);
      }

      // XexLoadImage is used to load a XEX executable from the filesystem, we use it to hook special system 
      // executables such as XAM, given that we can't possibly know when they're loaded.
      void HLESystemProvider::HLE_XexLoadImage(sPPEState *ppeState) {
        // Get current thread
        sPPUThread &thread = ppeState->ppuThread[PPCInterpreter::tl_curThreadId];

        // GPR[0x3] = image name pointer.
        const u64 imageNameAddr = thread.GPR[3];

        // Loaded module name.
        char moduleName[128];
        u8 *imageNamePtr = PPCInterpreter::MMUGetPointerFromRAM(imageNameAddr, ppeState, true);
        
        if (imageNamePtr != nullptr) {
          memcpy(moduleName, imageNamePtr, 128);
          LOG_INFO(HLE, "[System]: XexLoadImage: Loading {} executable.", moduleName);
          return;
          if (strcmp(moduleName, "hud.xex") == 0) {
            LOG_INFO(HLE, "[System]: XexLoadImage: Detected xam.xex load, hooking XAM HLE functions.");

            u32 address = 0x815F0000;
            u8 *imgBasePtr = PPCInterpreter::MMUGetPointerFromRAM(address, ppeState, true);
            if (imageNamePtr != nullptr) {
              LOG_INFO(HLE, "[System]: XexLoadImage: Detected xam.xex load, hooking XAM HLE functions. {:#x}", (u64)imgBasePtr);
              XeMain::GetCPU()->GetCPUContext()->xamImageParser.ParseImageData(imgBasePtr, 0x005a0200);
              // Parse HLE Function table
              XeMain::GetCPU()->GetCPUContext()->ProcessHLEFunctionTable(Xe::Core::HLE::eSystemExecutables::xam);
            }
          }
        }

      }
    }
  }
}
