/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#pragma once

#include "Core/XCPU/JIT/Compiler/CompilerPass.h"

namespace Xe {
  namespace XCPU {
    namespace Compiler {
      namespace Passes {

        class DeadCodeEliminationPass : public CompilerPass {
        public:
          DeadCodeEliminationPass();
          ~DeadCodeEliminationPass() override;

          bool Run(HIR::HIRBuilder *builder) override;

        private:
          void MakeNopRecursive(HIR::Instr *i);
          void ReplaceAssignment(HIR::Instr *i);
          bool CheckLocalUse(HIR::Instr *i);
        };

      }  // namespace passes
    }  // namespace compiler
  }  // namespace cpu
}  // namespace xe
