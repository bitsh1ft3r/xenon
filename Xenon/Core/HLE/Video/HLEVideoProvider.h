/***************************************************************/
/* Copyright 2026 Xenon Emulator Project. All rights reserved. */
/***************************************************************/

#pragma once

#include "Core/HLE/HLEFunctionProvider.h"

namespace Xe {
  namespace Core {
    namespace HLE {

      // HLE provider for Video related functions.
      class HLEVideoProvider : public HLEFunctionProvider {
      public:
        HLEVideoProvider() = default;
        ~HLEVideoProvider() override = default;

        bool Setup() override;
        eHLECategory GetCategory() const override { return eHLECategory::Video; }
        std::string GetName() const override { return "Video"; }
        void CollectFunctions(std::vector<sHLEFunction> &table) override;

      private:
        // Static HLE trampolines
        static void HLE_VdRetrainEDRAM(sPPEState *ppeState);
        static void HLE_VdIsHSIOTrainingSucceeded(sPPEState *ppeState);
        static void HLE_VdSwap(sPPEState *ppeState);
      };

    }
  }
}
