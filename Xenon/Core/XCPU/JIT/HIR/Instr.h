/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#pragma once

#include "Core/XCPU/PPU/PowerPC.h"
#include "Core/XCPU/JIT/HIR/Value.h"
#include "Core/XCPU/JIT/HIR/Opcodes.h"

namespace Xe {
  namespace XCPU {
    namespace HIR {

      class HIRBlock;
      class Label;

      // HIR Instruction
      class Instr {
      public:
        // HIR Block this instruction is linked to
        HIRBlock *block;
        // Next instruction in the block
        Instr *next;
        // Previous instruction in the block
        Instr *prev;

        // Opcode info pointer (opcode + signature/flags)
        const OpcodeInfo *opcode;
        uint16_t flags = 0;
        uint32_t ordinal;

        // Current instruction data (for branch/system instructions)
        uPPCInstr currentInstrData = { 0 };

        typedef union {
          Label *label;
          Value *value;
          uint64_t offset;
        } Op;

        Value *dest;
        Op src1;
        Op src2;
        Op src3;

        Value::Use *src1_use;
        Value::Use *src2_use;
        Value::Use *src3_use;

        void set_src1(Value *value);
        void set_src2(Value *value);
        void set_src3(Value *value);

        void MoveBefore(Instr *other);
        void Replace(const OpcodeInfo *new_opcode, uint16_t new_flags);
        void Remove();
      };

    }  // namespace hir
  }  // namespace cpu
}  // namespace xe
