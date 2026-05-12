/***************************************************************/
/* Copyright 2026 Xenon Emulator Project. All rights reserved. */
/***************************************************************/

#include "Base/Logging/Log.h"
#include "Core/HLE/Input/HLEInputManager.h"

namespace Xe {
  namespace Core {
    namespace HLE {

      HLEInputManager &HLEInputManager::Get() {
        static HLEInputManager instance;
        return instance;
      }

      void HLEInputManager::SetBackend(std::unique_ptr<HLEInputSystem> backend) {
        activeBackend = std::move(backend);
      }

      // Static trampolines 

      void HLEInputManager::HLE_XInputGetState(sPPEState *ppeState) {
        // Get active backend
        HLEInputSystem *backend = Get().GetBackend();

        if (!backend || !backend->IsPresent()) { return; }

        backend->GetState(ppeState);
        return;
      }

      void HLEInputManager::HLE_XInputSetState(sPPEState *ppeState) {
        // Get active backend
        HLEInputSystem *backend = Get().GetBackend();
        
        if (!backend || !backend->IsPresent()) { return; }

        backend->SetState(ppeState);
        return;
      }

      void HLEInputManager::HLE_XInputGetCapabilities(sPPEState *ppeState) {
        // Get active backend
        HLEInputSystem *backend = Get().GetBackend();
        
        if (!backend || !backend->IsPresent()) { return; }

        backend->GetCapabilities(ppeState);
        return;
      }

      void HLEInputManager::HLE_XInputGetKeystroke(sPPEState *ppeState) {
        // Get active backend
        HLEInputSystem *backend = Get().GetBackend();
        
        if (!backend || !backend->IsPresent()) { return; }

        backend->GetKeystroke(ppeState);
        return;
      }

    }
  }
}