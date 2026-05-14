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

        // Analyzes the control-flow graph of the HIR block chain.
        //
        // Walks each block and classifies its exit type by inspecting the last
        // meaningful instruction:
        //   OPCODE_BRANCH_LABEL      → unconditional intra-function edge
        //   OPCODE_BRANCH_TRUE_LABEL → conditional taken edge + fallthrough edge
        //   everything else          → external function exit (ret to dispatcher)
        //
        // Results are logged at DEBUG level when PPC_SCANNER_DEBUG is defined.
        // No structural changes are made to the HIR; this pass is informational
        // and a prerequisite for future loop-detection and dominator-tree passes.
        class ControlFlowAnalysisPass : public CompilerPass {
        public:
          ControlFlowAnalysisPass();
          ~ControlFlowAnalysisPass() override;

          bool Run(HIR::HIRBuilder* builder) override;
        };

      }  // namespace Passes
    }  // namespace Compiler
  }  // namespace XCPU
}  // namespace Xe
