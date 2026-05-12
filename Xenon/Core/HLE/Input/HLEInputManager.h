/***************************************************************/
/* Copyright 2026 Xenon Emulator Project. All rights reserved. */
/***************************************************************/

#pragma once

#include <memory>
#include "Core/HLE/HLEFunction.h"
#include "Core/HLE/Input/HLEInputSystem.h"

namespace Xe {
  namespace Core {
    namespace HLE {

      // Manages the active HLE input backend and exposes static
      // HLEFunctionPtr compatible trampolines for the HLE function table.
      class HLEInputManager {
      public:
        // Singleton access
        static HLEInputManager &Get();

        // Set the active input backend
        void SetBackend(std::unique_ptr<HLEInputSystem> backend);
        // Get the active input backend
        HLEInputSystem *GetBackend() { return activeBackend.get(); }

        // Static trampolines for the function table, these call the corresponding method on the active backend
        static void HLE_XInputGetState(sPPEState *ppeState);
        static void HLE_XInputSetState(sPPEState *ppeState);
        static void HLE_XInputGetCapabilities(sPPEState *ppeState);
        static void HLE_XInputGetKeystroke(sPPEState *ppeState);

      private:
        HLEInputManager() = default;
        std::unique_ptr<HLEInputSystem> activeBackend;
      };

    }
  }
}