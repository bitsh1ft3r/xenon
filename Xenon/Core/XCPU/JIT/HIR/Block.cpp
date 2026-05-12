/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "Core/XCPU/JIT/HIR/Block.h"
#include "Core/XCPU/JIT/HIR/Instr.h"

namespace Xe {
  namespace XCPU {
    namespace HIR {

      void HIRBlock::AssertNoCycles() {
        // Walk the instruction list and ensure there are no cycles.
        Instr *hare = instr_head;
        Instr *tortoise = instr_head;
        while (hare && hare->next) {
          hare = hare->next->next;
          tortoise = tortoise->next;
          if (hare == tortoise) {
            // Cycle detected!
            assert(false && "Cycle detected in instruction list");
            return;
          }
        }
      }

    }  // namespace HIR
  }  // namespace XCPU
}  // namespace Xe
