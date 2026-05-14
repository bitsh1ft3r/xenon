/***************************************************************/
/* Copyright 2026 Xenon Emulator Project. All rights reserved. */
/***************************************************************/

#include "Core/XCPU/JIT/Compiler/Passes/DataFlowAnalysisPass.h"

#include "Core/XCPU/JIT/HIR/Block.h"
#include "Core/XCPU/JIT/HIR/Instr.h"
#include "Core/XCPU/JIT/HIR/Value.h"

namespace Xe {
  namespace XCPU {
    namespace Compiler {
      namespace Passes {

        using namespace HIR;

        DataFlowAnalysisPass::DataFlowAnalysisPass() : CompilerPass() {}
        DataFlowAnalysisPass::~DataFlowAnalysisPass() {}

        bool DataFlowAnalysisPass::Run(HIR::HIRBuilder* builder) {
          uint32_t blockCount = LinearizeBlocks(builder);
          if (blockCount <= 1) {
            // Per-block mode or single-block function — nothing to analyse.
            return true;
          }

          uint32_t valueCount = NumberValues(builder);

          // Analyse cross-block value usage.
          // For each value defined in block A and used in block B (B != A), we
          // record the ordinal of block A in B's incoming_values bitvector.
          // Currently incoming_values is not allocated in the arena; this loop
          // identifies the presence of cross-block edges only.
          // (Full bitvector allocation is a future enhancement gated on adding an
          //  llvm::BitVector construction path to the scratch arena.)
          bool hasCrossBlockValues = false;
          for (HIRBlock* defBlock = builder->getBlockHead(); defBlock; defBlock = defBlock->next) {
            for (Instr* i = defBlock->instr_head; i; i = i->next) {
              Value* dest = i->dest;
              if (!dest) continue;
              for (Value::Use* use = dest->use_head; use; use = use->next) {
                HIRBlock* useBlock = use->instr->block;
                if (useBlock && useBlock != defBlock) {
                  hasCrossBlockValues = true;
                  // Mark the value's def-block ordinal in the use-block's
                  // incoming_values when the bitvector is available.
                  // (Placeholder: incoming_values remains nullptr until allocated.)
                }
              }
            }
          }

          (void)valueCount;
          (void)hasCrossBlockValues;
          return true;
        }

        uint32_t DataFlowAnalysisPass::LinearizeBlocks(HIR::HIRBuilder* builder) {
          uint32_t ordinal = 0;
          for (HIRBlock* block = builder->getBlockHead(); block; block = block->next) {
            block->ordinal = ordinal++;
          }
          return ordinal;
        }

        uint32_t DataFlowAnalysisPass::NumberValues(HIR::HIRBuilder* builder) {
          // Assign globally-unique ordinals to every SSA value across all blocks.
          // This ensures ordinal comparisons work across block boundaries.
          uint32_t ordinal = 0;
          for (HIRBlock* block = builder->getBlockHead(); block; block = block->next) {
            for (Instr* i = block->instr_head; i; i = i->next) {
              if (i->dest) {
                i->dest->ordinal = ordinal++;
              }
            }
          }
          return ordinal;
        }

      }  // namespace Passes
    }  // namespace Compiler
  }  // namespace XCPU
}  // namespace Xe
