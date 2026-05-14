/***************************************************************/
/* Copyright 2026 Xenon Emulator Project. All rights reserved. */
/***************************************************************/

#include "Core/XCPU/JIT/Compiler/Passes/ControlFlowAnalysisPass.h"

#include "Base/Logging/Log.h"
#include "Core/XCPU/JIT/HIR/Block.h"
#include "Core/XCPU/JIT/HIR/Instr.h"
#include "Core/XCPU/JIT/HIR/Opcodes.h"

namespace Xe {
  namespace XCPU {
    namespace Compiler {
      namespace Passes {

        using namespace HIR;

        ControlFlowAnalysisPass::ControlFlowAnalysisPass() : CompilerPass() {}
        ControlFlowAnalysisPass::~ControlFlowAnalysisPass() {}

        bool ControlFlowAnalysisPass::Run(HIR::HIRBuilder* builder) {
          uint32_t blockCount = 0;
          uint32_t intraEdges = 0;
          uint32_t externalExits = 0;

          for (HIRBlock* block = builder->getBlockHead(); block; block = block->next) {
            ++blockCount;

            // Find the logical exit instruction (walk back past SYNC_EXCEPTION_CHECKs).
            const Instr* exitInstr = block->instr_tail;
            while (exitInstr && exitInstr->opcode &&
                   exitInstr->opcode->num == OPCODE_SYNC_EXCEPTION_CHECK) {
              exitInstr = exitInstr->prev;
            }

            if (!exitInstr || !exitInstr->opcode) {
              ++externalExits;
              continue;
            }

            switch (exitInstr->opcode->num) {
            case OPCODE_BRANCH_LABEL:
              // Unconditional intra-function jump.
              ++intraEdges;
              break;
            case OPCODE_BRANCH_TRUE_LABEL:
              // Conditional: one intra-function taken edge + one fallthrough.
              intraEdges += 2;
              break;
            default:
              // External exit (stores NIA and returns to dispatcher).
              ++externalExits;
              break;
            }
          }

          if (blockCount > 1) {
            LOG_DEBUG(Xenon, "[CFAnalysis]: {} blocks, {} intra-function edges, {} external exits",
              blockCount, intraEdges, externalExits);
          }

          return true;
        }

      }  // namespace Passes
    }  // namespace Compiler
  }  // namespace XCPU
}  // namespace Xe
