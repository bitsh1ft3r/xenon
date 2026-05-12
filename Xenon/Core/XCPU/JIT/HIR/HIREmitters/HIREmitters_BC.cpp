/***************************************************************/
/* Copyright 2026 Xenon Emulator Project. All rights reserved. */
/***************************************************************/

#include "Base/Assert.h"
#include "Core/XCPU/JIT/HIR/HIREmitters/HIREmitters.h"

namespace Xe {
  namespace XCPU {
    namespace HIR {

      // TODO: Implement the branch instructions in the HIR emitter

      // Branch
      int HIRInstrEmit_bx(HIRBuilder &b, const uPPCInstr &instr) {
        b.Branch(instr);
        return 0;
      }

      // Branch Conditional
      int HIRInstrEmit_bcx(HIRBuilder &b, const uPPCInstr &instr) {
        b.BranchConditional(instr);
        return 0;
      }

      // Branch Conditional To CTR
      int HIRInstrEmit_bcctrx(HIRBuilder &b, const uPPCInstr &instr) {
        b.BranchConditionalToCTR(instr);
        return 0;
      }

      // Branch Conditional To LR
      int HIRInstrEmit_bclrx(HIRBuilder &b , const uPPCInstr &instr) {
        b.BranchConditionalToLR(instr);
        return 0;
      }

    }
  }
}