/***************************************************************/
/* Copyright 2026 Xenon Emulator Project. All rights reserved. */
/***************************************************************/

#include "Core/XCPU/JIT/Compiler/Passes/FinalizationPass.h"

#include "Core/XCPU/JIT/HIR/Block.h"
#include "Core/XCPU/JIT/HIR/Instr.h"
#include "Core/XCPU/JIT/HIR/Opcodes.h"

namespace Xe {
  namespace XCPU {
    namespace Compiler {
      namespace Passes {

        using namespace HIR;

        FinalizationPass::FinalizationPass() : CompilerPass() {}
        FinalizationPass::~FinalizationPass() {}

        bool FinalizationPass::Run(HIR::HIRBuilder* builder) {
          uint16_t blockOrdinal = 0;
          for (HIRBlock* block = builder->getBlockHead(); block; block = block->next) {
            block->ordinal = blockOrdinal++;

            // Remove OPCODE_BRANCH_LABEL that targets the immediately next block.
            // In EmitFunction, block labels are bound in chain order, so a jump to
            // the next block is a native no-op. Removing it avoids a spurious JMP
            // and lets EmitFunction classify the exit type correctly.
            Instr* tail = block->instr_tail;
            if (tail && tail->opcode && tail->opcode->num == OPCODE_BRANCH_LABEL && block->next) {
              auto* targetBlock = reinterpret_cast<HIRBlock*>(tail->src1.offset);
              if (targetBlock == block->next) {
                tail->Remove();
              }
            }
          }
          return true;
        }

      }  // namespace Passes
    }  // namespace Compiler
  }  // namespace XCPU
}  // namespace Xe
