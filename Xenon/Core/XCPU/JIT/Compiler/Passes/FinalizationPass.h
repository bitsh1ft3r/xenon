/***************************************************************/
/* Copyright 2026 Xenon Emulator Project. All rights reserved. */
/***************************************************************/

#pragma once

#include "Core/XCPU/JIT/Compiler/CompilerPass.h"
#include "Core/XCPU/JIT/HIR/HIRBuilder.h"

namespace Xe {
  namespace XCPU {
    namespace Compiler {
      namespace Passes {

        // Final cleanup pass before code generation.
        //
        // 1. Assigns final sequential ordinals to every HIRBlock so the backend
        //    can use block->ordinal as a stable numeric id.
        //
        // 2. Removes redundant OPCODE_BRANCH_LABEL instructions whose target is
        //    the immediately following block. In per-function mode, after optimization
        //    passes, a block may end with an unconditional label-jump to the very next
        //    block in the chain. Because EmitFunction binds labels consecutively, such
        //    a jump is a pure no-op in the generated machine code. Removing it avoids
        //    an unnecessary JMP instruction and, crucially, lets EmitFunction see the
        //    preceding SYNC_EXCEPTION_CHECK and emit a correct ret() for external exits.
        class FinalizationPass : public CompilerPass {
        public:
          FinalizationPass();
          ~FinalizationPass() override;

          bool Run(HIR::HIRBuilder* builder) override;
        };

      }  // namespace Passes
    }  // namespace Compiler
  }  // namespace XCPU
}  // namespace Xe
