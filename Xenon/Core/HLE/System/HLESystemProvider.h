/***************************************************************/
/* Copyright 2026 Xenon Emulator Project. All rights reserved. */
/***************************************************************/

#pragma once

#include "Core/HLE/HLEFunctionProvider.h"

namespace Xe {
  namespace Core {
    namespace HLE {

      // HLE provider for System related functions.
      class HLESystemProvider : public HLEFunctionProvider {
      public:
        HLESystemProvider() = default;
        ~HLESystemProvider() override = default;
        bool Setup() override;
        eHLECategory GetCategory() const override { return eHLECategory::System; }
        std::string GetName() const override { return "System"; }
        void CollectFunctions(std::vector<sHLEFunction> &table) override;

      private:
        // Static HLE trampolines
        static void HLE_XexLoadImage(sPPEState *ppeState);
      };

    }
  }
}
