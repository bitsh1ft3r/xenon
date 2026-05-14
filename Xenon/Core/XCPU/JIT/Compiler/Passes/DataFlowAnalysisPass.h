/***************************************************************/
/* Copyright 2026 Xenon Emulator Project. All rights reserved. */
/***************************************************************/

#pragma once

#include <cstdint>

#include "Core/XCPU/JIT/Compiler/CompilerPass.h"
#include "Core/XCPU/JIT/HIR/HIRBuilder.h"

namespace Xe {
  namespace XCPU {
    namespace Compiler {
      namespace Passes {

        // Performs data-flow analysis across the HIR block chain.
        //
        // Phase 1 – Linearization:
        //   Assigns sequential ordinals to every block and to every SSA Value
        //   produced in the function. These ordinals are stable identifiers for
        //   bitvector indexing in later passes.
        //
        // Phase 2 – Incoming-value analysis:
        //   Walks blocks in order. For each value defined in block A and used in
        //   block B (A != B), marks that value as "incoming" to B by setting the
        //   corresponding bit in HIRBlock::incoming_values. This information lets
        //   ContextPromotionPass (in a future cross-block pass) know which context
        //   slots are guaranteed to carry the same SSA value at block entry.
        //
        // NOTE: Full phi-node / StoreLocal-LoadLocal insertion (as done in Xenia's
        //       DataFlowAnalysisPass) is deferred — Xenon routes all cross-block
        //       data through context stores/loads, so the insert phase is not yet
        //       required for correctness.
        class DataFlowAnalysisPass : public CompilerPass {
        public:
          DataFlowAnalysisPass();
          ~DataFlowAnalysisPass() override;

          bool Run(HIR::HIRBuilder* builder) override;

        private:
          // Assigns block->ordinal for all blocks. Returns the total block count.
          uint32_t LinearizeBlocks(HIR::HIRBuilder* builder);
          // Assigns globally-unique value ordinals (value->ordinal) and returns
          // the total number of values defined in this function.
          uint32_t NumberValues(HIR::HIRBuilder* builder);
        };

      }  // namespace Passes
    }  // namespace Compiler
  }  // namespace XCPU
}  // namespace Xe
