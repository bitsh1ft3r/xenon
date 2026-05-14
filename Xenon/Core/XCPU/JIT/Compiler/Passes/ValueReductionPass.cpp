/***************************************************************/
/* Copyright 2026 Xenon Emulator Project. All rights reserved. */
/***************************************************************/

#include "Core/XCPU/JIT/Compiler/Passes/ValueReductionPass.h"
#include "Core/XCPU/JIT/HIR/Block.h"
#include "Core/XCPU/JIT/HIR/Instr.h"

namespace Xe {
  namespace XCPU {
    namespace Compiler {
      namespace Passes {

        using namespace HIR;

        ValueReductionPass::ValueReductionPass() : CompilerPass() {}
        ValueReductionPass::~ValueReductionPass() {}

        bool ValueReductionPass::Run(HIR::HIRBuilder* builder) {
          // Assign globally unique ordinals so cross-block comparisons work.
          uint32_t ordinal = 0;
          for (HIRBlock* block = builder->getBlockHead(); block; block = block->next) {
            for (Instr* i = block->instr_head; i; i = i->next) {
              i->ordinal = ordinal++;
            }
          }

          // Compute last_use for every defined value.
          for (HIRBlock* block = builder->getBlockHead(); block; block = block->next) {
            for (Instr* i = block->instr_head; i; i = i->next) {
              if (i->dest) {
                ComputeLastUse(i->dest);
              }
            }
          }
          return true;
        }

        void ValueReductionPass::ComputeLastUse(HIR::Value* value) {
          // Walk the use chain, picking the use with the highest instruction ordinal.
          Instr* last = nullptr;
          uint32_t lastOrdinal = 0;
          for (Value::Use* use = value->use_head; use; use = use->next) {
            if (!last || use->instr->ordinal >= lastOrdinal) {
              last = use->instr;
              lastOrdinal = use->instr->ordinal;
            }
          }
          value->last_use = last;
        }

      }  // namespace Passes
    }  // namespace Compiler
  }  // namespace XCPU
}  // namespace Xe
