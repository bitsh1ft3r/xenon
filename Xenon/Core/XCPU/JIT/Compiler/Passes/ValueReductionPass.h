/***************************************************************/
/* Copyright 2026 Xenon Emulator Project. All rights reserved. */
/***************************************************************/

#pragma once

#include "Core/XCPU/JIT/Compiler/CompilerPass.h"
#include "Core/XCPU/JIT/HIR/HIRBuilder.h"
#include "Core/XCPU/JIT/HIR/Value.h"

namespace Xe {
  namespace XCPU {
    namespace Compiler {
      namespace Passes {

        // Computes the last-use point for every SSA Value across all blocks.
        // After this pass, Value::last_use points to the final Instr that reads
        // the value. Used by downstream analysis (e.g. register allocation) to
        // determine when a value's lifetime ends. Also assigns globally unique
        // ordinals to every instruction so ordinal comparisons are meaningful
        // across block boundaries.
        class ValueReductionPass : public CompilerPass {
        public:
          ValueReductionPass();
          ~ValueReductionPass() override;

          bool Run(HIR::HIRBuilder* builder) override;

        private:
          void ComputeLastUse(HIR::Value* value);
        };

      }  // namespace Passes
    }  // namespace Compiler
  }  // namespace XCPU
}  // namespace Xe
