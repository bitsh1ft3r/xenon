/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#pragma once

#include "Core/XCPU/JIT/Compiler/Passes/ConditionalGroupSubpass.h"

namespace Xe {
  namespace XCPU {
    namespace Compiler {
      namespace Passes {

        class ConstantPropagationPass : public ConditionalGroupSubpass {
        public:
          ConstantPropagationPass();
          ~ConstantPropagationPass() override;

          bool Run(HIR::HIRBuilder *builder, bool &result) override;

        private:
        };

      }  // namespace passes
    }  // namespace compiler
  }  // namespace cpu
}  // namespace xe
