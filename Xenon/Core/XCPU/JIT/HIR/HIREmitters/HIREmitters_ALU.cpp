/***************************************************************/
/* Copyright 2026 Xenon Emulator Project. All rights reserved. */
/***************************************************************/

/*
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// Modified for use on the Xenon Emulator.

#include "Base/Assert.h"
#include "Core/XCPU/JIT/HIR/HIREmitters/HIREmitters.h"

namespace Xe {
  namespace XCPU {
    namespace HIR {

      //
      // Helpers
      //

      // PPC carry semantics on a 64-bit-capable PPC (Xenon):
      //   MSR.SF=1 (64-bit mode): CA = carry out of bit 0 of the 64-bit result.
      //   MSR.SF=0 (32-bit mode): CA = carry out of bit 32 (i.e. the 32-bit result).
      // Mirrors PPC_ALU.cpp::addResult, which is the authoritative model.
      // The branch on GetMSRSF() is a translate-time constant — the optimizer
      // folds the dead arm and the cache key already specializes per-SF.
      static Value *NarrowForSF(HIRBuilder &b, Value *v) {
        return b.GetMSRSF() ? v : b.Truncate(v, INT32_TYPE);
      }

      Value *AddDidCarry(HIRBuilder &b, Value *v1, Value *v2) {
        // Carry iff (v1 + v2) overflows the working width:
        //   v1 + v2 > MAX  <=>  v2 > ~v1   (~v1 == MAX - v1)
        v1 = NarrowForSF(b, v1);
        v2 = NarrowForSF(b, v2);
        return b.CompareUGT(v2, b.Not(v1));
      }

      Value *SubDidCarry(HIRBuilder &b, Value *v1, Value *v2) {
        // PPC `subfc rD, rA, rB` is rB - rA; CA = no-borrow indicator
        // (rB >= rA, unsigned). Caller passes (rb, ra). We compute on the
        // narrowed working width.
        // (v1 > ~(-v2)) || (v2 == 0)  ==  v1 >= v2  (the v2==0 arm catches the
        // ~(-0) = ~0 = MAX wraparound case).
        v1 = NarrowForSF(b, v1);
        v2 = NarrowForSF(b, v2);
        return b.Or(b.CompareUGT(v1, b.Not(b.Neg(v2))),
                    b.IsFalse(v2));
      }

      // https://github.com/sebastianbiallas/pearpc/blob/0b3c823f61456faa677f6209545a7b906e797421/src/cpu/cpu_generic/ppc_tools.h#L26
      Value *AddWithCarryDidCarry(HIRBuilder &b, Value *v1, Value *v2, Value *v3) {
        // Three-input carry: result = v1 + v2 + v3 (where v3 is 0 or 1).
        // Overflow iff (v1+v2) wraps OR ((v1+v2)+v3) wraps. Compute on the
        // working width so the wrap point matches CA's defined position.
        ASSERT(v3->type == INT8_TYPE);
        TypeName workType = b.GetMSRSF() ? INT64_TYPE : INT32_TYPE;
        v1 = NarrowForSF(b, v1);
        v2 = NarrowForSF(b, v2);
        v3 = b.ZeroExtend(v3, workType);
        return b.Or(b.CompareULT(b.Add(b.Add(v1, v2), v3), v3),
                    b.CompareULT(b.Add(v1, v2), v1));
      }


      //
      // Definitions for ALU HIR Emitters
      //

      int HIRInstrEmit_addx(HIRBuilder &b, const uPPCInstr &instr) {
        // RD <- (RA) + (RB)
        Value *v = b.Add(b.LoadGPR(instr.ra), b.LoadGPR(instr.rb));
        b.StoreGPR(instr.rd, v);
        
        if (instr.oe) { INSTRNOTIMPLEMENTED(); }
        
        if (instr.rc) { b.UpdateCR0(v); }
        return 0;
      }

      int HIRInstrEmit_addcx(HIRBuilder &b, const uPPCInstr &instr) {
        // RD <- (RA) + (RB)
        // CA <- carry bit
        Value *ra = b.LoadGPR(instr.ra);
        Value *rb = b.LoadGPR(instr.rb);
        Value *v = b.Add(ra, rb);
        b.StoreGPR(instr.rd, v);
        
        if (instr.oe) { INSTRNOTIMPLEMENTED(); }
        
        else { b.StoreCA(AddDidCarry(b, ra, rb)); }
        if (instr.rc) {
          b.UpdateCR0(v);
        }

        return 0;
      }

      int HIRInstrEmit_addex(HIRBuilder &b, const uPPCInstr &instr) {
        // RD <- (RA) + (RB) + XER[CA]
        // CA <- carry bit
        Value *ra = b.LoadGPR(instr.ra);
        Value *rb = b.LoadGPR(instr.rb);
        Value *v = b.AddWithCarry(ra, rb, b.LoadCA());
        b.StoreGPR(instr.rd, v);
        if (instr.oe) {
          INSTRNOTIMPLEMENTED();
          // e.update_xer_with_overflow(EFLAGS OF?);
        }
        else {
          b.StoreCA(AddWithCarryDidCarry(b, ra, rb, b.LoadCA()));
        }
        if (instr.rc) {
          b.UpdateCR0(v);
        }
        return 0;
      }

      int HIRInstrEmit_addi(HIRBuilder &b, const uPPCInstr &instr) {
        // if RA = 0 then
        //   RT <- EXTS(SI)
        // else
        //   RT <- (RA) + EXTS(SI)
        Value *si = b.LoadConstantInt64(SignExtend16(instr.simm16));
        Value *v = si;
        if (instr.ra) {
          v = b.Add(b.LoadGPR(instr.ra), si);
        }
        b.StoreGPR(instr.rd, v);
        return 0;
      }

      int HIRInstrEmit_addicx(HIRBuilder &b, const uPPCInstr &instr) {
        // RT <- (RA) + EXTS(SI)
        // CA <- carry bit
        Value *ra = b.LoadGPR(instr.ra);
        Value *v = b.Add(b.LoadGPR(instr.ra), b.LoadConstantInt64(SignExtend16(instr.simm16)));
        b.StoreGPR(instr.rd, v);
        b.StoreCA(AddDidCarry(b, ra, b.LoadConstantInt64(SignExtend16(instr.simm16))));
        if (instr.main & 1) {
          b.UpdateCR0(v);
        }
        return 0;
      }

      int HIRInstrEmit_addis(HIRBuilder &b, const uPPCInstr &instr) {
        // if RA = 0 then
        //   RT <- EXTS(SI) || i16.0
        // else
        //   RT <- (RA) + EXTS(SI) || i16.0
        Value *si = b.LoadConstantInt64(SignExtend16(instr.simm16) << 16);
        Value *v = si;
        if (instr.ra) {
          v = b.Add(b.LoadGPR(instr.ra), si);
        }
        b.StoreGPR(instr.rd, v);
        return 0;
      }

      int HIRInstrEmit_addmex(HIRBuilder &b, const uPPCInstr &instr) {
        // RT <- (RA) + CA - 1
        // CA <- carry bit
        Value *ra = b.LoadGPR(instr.ra);
        Value *v = b.AddWithCarry(ra, b.LoadConstantInt64(-1), b.LoadCA());
        b.StoreGPR(instr.rd, v);
        if (instr.oe) {
          INSTRNOTIMPLEMENTED();
        } else {
          // Just CA update.
          b.StoreCA(AddWithCarryDidCarry(b, ra, b.LoadConstantInt64(-1), b.LoadCA()));
        }
        if (instr.rc) {
          b.UpdateCR0(v);
        }
        return 0;
      }

      int HIRInstrEmit_addzex(HIRBuilder &b, const uPPCInstr &instr) {
        // RT <- (RA) + CA
        // CA <- carry bit
        Value *ra = b.LoadGPR(instr.ra);
        Value *v = b.AddWithCarry(ra, b.LoadZeroInt64(), b.LoadCA());
        b.StoreGPR(instr.rd, v);
        if (instr.oe) {
          INSTRNOTIMPLEMENTED();
          return 1;
        }
        else {
          // Just CA update.
          b.StoreCA(AddWithCarryDidCarry(b, ra, b.LoadZeroInt64(), b.LoadCA()));
        }
        if (instr.rc) {
          b.UpdateCR0(v);
        }
        return 0;
      }

      int HIRInstrEmit_divdx(HIRBuilder &b, const uPPCInstr &instr) {
        // dividend <- (RA)
        // divisor <- (RB)
        // if divisor = 0 then
        //   if OE = 1 then
        //     XER[OV] <- 1
        //   return
        // RT <- dividend ÷ divisor
        Value *divisor = b.LoadGPR(instr.rb);
        // TODO(benvanik): check if zero
        //                 if OE=1, set XER[OV] = 1
        //                 else skip the divide
        Value *v = b.Div(b.LoadGPR(instr.ra), divisor);
        b.StoreGPR(instr.rd, v);
        if (instr.oe) {
          INSTRNOTIMPLEMENTED();
          return 1;
        }
        if (instr.rc) {
          b.UpdateCR0(v);
        }
        return 0;
      }

      int HIRInstrEmit_divdux(HIRBuilder &b, const uPPCInstr &instr) {
        // dividend <- (RA)
        // divisor <- (RB)
        // if divisor = 0 then
        //   if OE = 1 then
        //     XER[OV] <- 1
        //   return
        // RT <- dividend ÷ divisor
        Value *divisor = b.LoadGPR(instr.rb);
        // TODO(benvanik): check if zero
        //                 if OE=1, set XER[OV] = 1
        //                 else skip the divide
        Value *v = b.Div(b.LoadGPR(instr.ra), divisor, ARITHMETIC_UNSIGNED);
        b.StoreGPR(instr.rd, v);
        if (instr.oe) {
          // If we are OE=1 we need to clear the overflow bit.
          // e.update_xer_with_overflow(e.get_uint64(0));
          INSTRNOTIMPLEMENTED();
          return 1;
        }
        if (instr.rc) {
          b.UpdateCR0(v);
        }
        return 0;
      }

      int HIRInstrEmit_divwx(HIRBuilder &b, const uPPCInstr &instr) {
        // dividend[0:31] <- (RA)[32:63]
        // divisor[0:31] <- (RB)[32:63]
        // if divisor = 0 then
        //   if OE = 1 then
        //     XER[OV] <- 1
        //   return
        // RT[32:63] <- dividend ÷ divisor
        // RT[0:31] <- undefined
        Value *divisor = b.Truncate(b.LoadGPR(instr.rb), INT32_TYPE);
        // TODO(benvanik): check if zero
        //                 if OE=1, set XER[OV] = 1
        //                 else skip the divide
        Value *v = b.Div(b.Truncate(b.LoadGPR(instr.ra), INT32_TYPE), divisor);
        v = b.ZeroExtend(v, INT64_TYPE);
        b.StoreGPR(instr.rd, v);
        if (instr.oe) {
          // If we are OE=1 we need to clear the overflow bit.
          // e.update_xer_with_overflow(e.get_uint64(0));
          INSTRNOTIMPLEMENTED();
          return 1;
        }
        if (instr.rc) {
          b.UpdateCR0(v);
        }
        return 0;
      }

      int HIRInstrEmit_divwux(HIRBuilder &b, const uPPCInstr &instr) {
        // dividend[0:31] <- (RA)[32:63]
        // divisor[0:31] <- (RB)[32:63]
        // if divisor = 0 then
        //   if OE = 1 then
        //     XER[OV] <- 1
        //   return
        // RT[32:63] <- dividend ÷ divisor
        // RT[0:31] <- undefined
        Value *divisor = b.Truncate(b.LoadGPR(instr.rb), INT32_TYPE);
        // TODO(benvanik): check if zero
        //                 if OE=1, set XER[OV] = 1
        //                 else skip the divide
        Value *v = b.Div(b.Truncate(b.LoadGPR(instr.ra), INT32_TYPE), divisor,
          ARITHMETIC_UNSIGNED);
        v = b.ZeroExtend(v, INT64_TYPE);
        b.StoreGPR(instr.rd, v);
        if (instr.oe) {
          // If we are OE=1 we need to clear the overflow bit.
          // e.update_xer_with_overflow(e.get_uint64(0));
          INSTRNOTIMPLEMENTED();
          return 1;
        }
        if (instr.rc) {
          b.UpdateCR0(v);
        }
        return 0;
      }

      int HIRInstrEmit_mulhdx(HIRBuilder &b, const uPPCInstr &instr) {
        // RT <- ((RA) × (RB) as 128)[0:63]
        if (instr.oe) {
          // With XER update.
          INSTRNOTIMPLEMENTED();
          return 1;
        }
        Value *v = b.MulHi(b.LoadGPR(instr.ra), b.LoadGPR(instr.rb));
        b.StoreGPR(instr.rd, v);
        if (instr.rc) {
          b.UpdateCR0(v);
        }
        return 0;
      }

      int HIRInstrEmit_mulhdux(HIRBuilder &b, const uPPCInstr &instr) {
        // RT <- ((RA) × (RB) as 128)[0:63]
        if (instr.oe) {
          // With XER update.
          INSTRNOTIMPLEMENTED();
          return 1;
        }
        Value *v =
          b.MulHi(b.LoadGPR(instr.ra), b.LoadGPR(instr.rb), ARITHMETIC_UNSIGNED);
        b.StoreGPR(instr.rd, v);
        if (instr.rc) {
          b.UpdateCR0(v);
        }
        return 0;
      }

      int HIRInstrEmit_mulhwx(HIRBuilder &b, const uPPCInstr &instr) {
        // RT[32:64] <- ((RA)[32:63] × (RB)[32:63])[0:31]
        if (instr.oe) {
          // With XER update.
          INSTRNOTIMPLEMENTED();
          return 1;
        }
        Value *v = b.SignExtend(b.MulHi(b.Truncate(b.LoadGPR(instr.ra), INT32_TYPE),
          b.Truncate(b.LoadGPR(instr.rb), INT32_TYPE)),
          INT64_TYPE);
        b.StoreGPR(instr.rd, v);
        if (instr.rc) {
          b.UpdateCR0(v);
        }
        return 0;
      }

      int HIRInstrEmit_mulhwux(HIRBuilder &b, const uPPCInstr &instr) {
        // RT[32:64] <- ((RA)[32:63] × (RB)[32:63])[0:31]
        if (instr.oe) {
          // With XER update.
          INSTRNOTIMPLEMENTED();
          return 1;
        }
        Value *v = b.ZeroExtend(
          b.MulHi(b.Truncate(b.LoadGPR(instr.ra), INT32_TYPE),
            b.Truncate(b.LoadGPR(instr.rb), INT32_TYPE), ARITHMETIC_UNSIGNED),
          INT64_TYPE);
        b.StoreGPR(instr.rd, v);
        if (instr.rc) {
          b.UpdateCR0(v);
        }
        return 0;
      }

      int HIRInstrEmit_mulldx(HIRBuilder &b, const uPPCInstr &instr) {
        // RT <- ((RA) × (RB))[64:127]
        if (instr.oe) {
          // With XER update.
          INSTRNOTIMPLEMENTED();
          return 1;
        }
        Value *v = b.Mul(b.LoadGPR(instr.ra), b.LoadGPR(instr.rb));
        b.StoreGPR(instr.rd, v);
        if (instr.rc) {
          b.UpdateCR0(v);
        }
        return 0;
      }

      int HIRInstrEmit_mulli(HIRBuilder &b, const uPPCInstr &instr) {
        // prod[0:127] <- (RA) × EXTS(SI)
        // RT <- prod[64:127]
        Value *v = b.Mul(b.LoadGPR(instr.ra), b.LoadConstantInt64(SignExtend16(instr.simm16)));
        b.StoreGPR(instr.rd, v);
        return 0;
      }

      int HIRInstrEmit_mullwx(HIRBuilder &b, const uPPCInstr &instr) {
        // RT <- (RA)[32:63] × (RB)[32:63]
        if (instr.oe) {
          // With XER update.
          INSTRNOTIMPLEMENTED();
          return 1;
        }
        Value *v = b.Mul(
          b.SignExtend(b.Truncate(b.LoadGPR(instr.ra), INT32_TYPE), INT64_TYPE),
          b.SignExtend(b.Truncate(b.LoadGPR(instr.rb), INT32_TYPE), INT64_TYPE));
        b.StoreGPR(instr.rd, v);
        if (instr.rc) {
          b.UpdateCR0(v);
        }
        return 0;
      }

      int HIRInstrEmit_negx(HIRBuilder &b, const uPPCInstr &instr) {
        // RT <- ¬(RA) + 1
        if (instr.oe) {
          // With XER update.
          // This is a different codepath as we need to use llvm.ssub.with.overflow.

          // if RA == 0x8000000000000000 then no-op and set OV=1
          // This may just magically do that...

          INSTRNOTIMPLEMENTED();
          return 1;
          // Function* ssub_with_overflow = Intrinsic::getDeclaration(
          //    e.gen_module(), Intrinsic::ssub_with_overflow, jit_type_nint);
          // jit_value_t v = b.CreateCall2(ssub_with_overflow,
          //                         e.get_int64(0), b.LoadGPR(instr.ra));
          // jit_value_t v0 = b.CreateExtractValue(v, 0);
          // b.StoreGPR(instr.rd, v0);
          // e.update_xer_with_overflow(b.CreateExtractValue(v, 1));

          // if (instr.rc) {
          //  // With cr0 update.
          //  b.UpdateCRx(0, v0, e.get_int64(0), true);
          //}
        }
        else {
          // No OE bit setting.
          Value *v = b.Neg(b.LoadGPR(instr.ra));
          b.StoreGPR(instr.rd, v);
          if (instr.rc) {
            b.UpdateCR0(v);
          }
        }
        return 0;
      }

      int HIRInstrEmit_subfx(HIRBuilder &b, const uPPCInstr &instr) {
        // RT <- ¬(RA) + (RB) + 1
        Value *v = b.Sub(b.LoadGPR(instr.rb), b.LoadGPR(instr.ra));
        b.StoreGPR(instr.rd, v);
        if (instr.oe) {
          INSTRNOTIMPLEMENTED();
          return 1;
          // e.update_xer_with_overflow(EFLAGS??);
        }
        if (instr.rc) {
          b.UpdateCR0(v);
        }
        return 0;
      }

      int HIRInstrEmit_subfcx(HIRBuilder &b, const uPPCInstr &instr) {
        // RT <- ¬(RA) + (RB) + 1
        Value *ra = b.LoadGPR(instr.ra);
        Value *rb = b.LoadGPR(instr.rb);
        Value *v = b.Sub(rb, ra);
        b.StoreGPR(instr.rd, v);
        if (instr.oe) {
          INSTRNOTIMPLEMENTED();
          return 1;
          // e.update_xer_with_overflow(EFLAGS??);
        }
        else {
          b.StoreCA(SubDidCarry(b, rb, ra));
        }
        if (instr.rc) {
          b.UpdateCR0(v);
        }
        return 0;
      }

      int HIRInstrEmit_subficx(HIRBuilder &b, const uPPCInstr &instr) {
        // RT <- ¬(RA) + EXTS(SI) + 1
        Value *ra = b.LoadGPR(instr.ra);
        Value *v = b.Sub(b.LoadConstantInt64(SignExtend16(instr.simm16)), ra);
        b.StoreGPR(instr.rd, v);
        b.StoreCA(SubDidCarry(b, b.LoadConstantInt64(SignExtend16(instr.simm16)), ra));
        return 0;
      }

      int HIRInstrEmit_subfex(HIRBuilder &b, const uPPCInstr &instr) {
        // RT <- ¬(RA) + (RB) + CA
        Value *not_ra = b.Not(b.LoadGPR(instr.ra));
        Value *rb = b.LoadGPR(instr.rb);
        Value *v = b.AddWithCarry(not_ra, rb, b.LoadCA());
        b.StoreGPR(instr.rd, v);
        if (instr.oe) {
          INSTRNOTIMPLEMENTED();
          return 1;
          // e.update_xer_with_overflow_and_carry(b.CreateExtractValue(v, 1));
        }
        else {
          b.StoreCA(AddWithCarryDidCarry(b, not_ra, rb, b.LoadCA()));
        }
        if (instr.rc) {
          b.UpdateCR0(v);
        }
        return 0;
      }

      int HIRInstrEmit_subfmex(HIRBuilder &b, const uPPCInstr &instr) {
        // RT <- ¬(RA) + CA - 1
        Value *not_ra = b.Not(b.LoadGPR(instr.ra));
        Value *v = b.AddWithCarry(not_ra, b.LoadConstantInt64(-1), b.LoadCA());
        b.StoreGPR(instr.rd, v);
        if (instr.oe) {
          INSTRNOTIMPLEMENTED();
          return 1;
          // e.update_xer_with_overflow_and_carry(b.CreateExtractValue(v, 1));
        }
        else {
          b.StoreCA(
            AddWithCarryDidCarry(b, not_ra, b.LoadConstantInt64(-1), b.LoadCA()));
        }
        if (instr.rc) {
          b.UpdateCR0(v);
        }
        return 0;
      }

      int HIRInstrEmit_subfzex(HIRBuilder &b, const uPPCInstr &instr) {
        // RT <- ¬(RA) + CA
        Value *not_ra = b.Not(b.LoadGPR(instr.ra));
        Value *v = b.AddWithCarry(not_ra, b.LoadZeroInt64(), b.LoadCA());
        b.StoreGPR(instr.rd, v);
        if (instr.oe) {
          INSTRNOTIMPLEMENTED();
          return 1;
          // e.update_xer_with_overflow_and_carry(b.CreateExtractValue(v, 1));
        }
        else {
          b.StoreCA(AddWithCarryDidCarry(b, not_ra, b.LoadZeroInt64(), b.LoadCA()));
        }
        if (instr.rc) {
          b.UpdateCR0(v);
        }
        return 0;
      }

      // Integer compare (A-4)

      int HIRInstrEmit_cmp(HIRBuilder &b, const uPPCInstr &instr) {
        Value *lhs;
        Value *rhs;
        if (instr.l10) {
          lhs = b.LoadGPR(instr.ra);
          rhs = b.LoadGPR(instr.rb);
        }
        else {
          lhs = b.Truncate(b.LoadGPR(instr.ra), INT32_TYPE);
          rhs = b.Truncate(b.LoadGPR(instr.rb), INT32_TYPE);
        }
        b.UpdateCRx(instr.crfd, lhs, rhs, true, false);
        return 0;
      }

      int HIRInstrEmit_cmpi(HIRBuilder &b, const uPPCInstr &instr) {
        Value *lhs;
        Value *rhs;
        if (instr.l10) {
          lhs = b.LoadGPR(instr.ra);
          rhs = b.LoadConstantInt64(SignExtend16(instr.simm16));
        } else {
          lhs = b.Truncate(b.LoadGPR(instr.ra), INT32_TYPE);
          rhs = b.LoadConstantInt32(SignExtend16(instr.simm16));
        }
        b.UpdateCRx(instr.crfd, lhs, rhs, true, false);
        return 0;
      }

      int HIRInstrEmit_cmpl(HIRBuilder &b, const uPPCInstr &instr) {
        Value *lhs;
        Value *rhs;
        if (instr.l10) {
          lhs = b.LoadGPR(instr.ra);
          rhs = b.LoadGPR(instr.rb);
        } else {
          lhs = b.Truncate(b.LoadGPR(instr.ra), INT32_TYPE);
          rhs = b.Truncate(b.LoadGPR(instr.rb), INT32_TYPE);
        }
        b.UpdateCRx(instr.crfd, lhs, rhs, false, false);
        return 0;
      }

      int HIRInstrEmit_cmpli(HIRBuilder &b, const uPPCInstr &instr) {
        Value *lhs;
        Value *rhs;
        if (instr.l10) {
          lhs = b.LoadGPR(instr.ra);
          rhs = b.LoadConstantUint64(instr.uimm16);
        } else {
          lhs = b.Truncate(b.LoadGPR(instr.ra), INT32_TYPE);
          rhs = b.LoadConstantUint32(instr.uimm16);
        }
        b.UpdateCRx(instr.crfd, lhs, rhs, false, false);
        return 0;
      }

      // Integer logical (A-5)

      int HIRInstrEmit_andx(HIRBuilder &b, const uPPCInstr &instr) {
        // RA <- (RS) & (RB)
        Value *ra = b.And(b.LoadGPR(instr.rd), b.LoadGPR(instr.rb));
        b.StoreGPR(instr.ra, ra);
        if (instr.rc) {
          b.UpdateCRx(0, ra);
        }
        return 0;
      }

      int HIRInstrEmit_andcx(HIRBuilder &b, const uPPCInstr &instr) {
        // RA <- (RS) & ¬(RB)
        Value *ra = b.AndNot(b.LoadGPR(instr.rd), b.LoadGPR(instr.rb));
        b.StoreGPR(instr.ra, ra);
        if (instr.rc) {
          b.UpdateCRx(0, ra);
        }
        return 0;
      }

      int HIRInstrEmit_andix(HIRBuilder &b, const uPPCInstr &instr) {
        // RA <- (RS) & (i48.0 || UI)
        Value *ra = b.And(b.LoadGPR(instr.rd), b.LoadConstantUint64(ZeroExtend16(instr.simm16)));
        b.StoreGPR(instr.ra, ra);
        b.UpdateCRx(0, ra);
        return 0;
      }

      int HIRInstrEmit_andisx(HIRBuilder &b, const uPPCInstr &instr) {
        // RA <- (RS) & (i32.0 || UI || i16.0)
        Value *ra =
          b.And(b.LoadGPR(instr.rd), b.LoadConstantUint64(ZeroExtend16(instr.simm16) << 16));
        b.StoreGPR(instr.ra, ra);
        b.UpdateCRx(0, ra);
        return 0;
      }

      int HIRInstrEmit_cntlzdx(HIRBuilder &b, const uPPCInstr &instr) {
        // n <- 0
        // do while n < 64
        //   if (RS)[n] = 1 then leave n
        //   n <- n + 1
        // RA <- n
        Value *v = b.CountLeadingZeros(b.LoadGPR(instr.rd));
        v = b.ZeroExtend(v, INT64_TYPE);
        b.StoreGPR(instr.ra, v);
        if (instr.rc) {
          b.UpdateCR0(v);
        }
        return 0;
      }

      int HIRInstrEmit_cntlzwx(HIRBuilder &b, const uPPCInstr &instr) {
        // n <- 32
        // do while n < 64
        //   if (RS)[n] = 1 then leave n
        //   n <- n + 1
        // RA <- n - 32
        Value *v = b.CountLeadingZeros(b.Truncate(b.LoadGPR(instr.rd), INT32_TYPE));
        v = b.ZeroExtend(v, INT64_TYPE);
        b.StoreGPR(instr.ra, v);
        if (instr.rc) {
          b.UpdateCR0(v);
        }
        return 0;
      }

      int HIRInstrEmit_eqvx(HIRBuilder &b, const uPPCInstr &instr) {
        // RA <- (RS) == (RB)
        Value *ra = b.Not(b.Xor(b.LoadGPR(instr.rd), b.LoadGPR(instr.rb)));
        b.StoreGPR(instr.ra, ra);
        if (instr.rc) {
          b.UpdateCRx(0, ra);
        }
        return 0;
      }

      int HIRInstrEmit_extsbx(HIRBuilder &b, const uPPCInstr &instr) {
        // s <- (RS)[56]
        // RA[56:63] <- (RS)[56:63]
        // RA[0:55] <- i56.s
        Value *rt = b.LoadGPR(instr.rd);
        rt = b.SignExtend(b.Truncate(rt, INT8_TYPE), INT64_TYPE);
        b.StoreGPR(instr.ra, rt);
        if (instr.rc) {
          b.UpdateCRx(0, rt);
        }
        return 0;
      }

      int HIRInstrEmit_extshx(HIRBuilder &b, const uPPCInstr &instr) {
        // s <- (RS)[48]
        // RA[48:63] <- (RS)[48:63]
        // RA[0:47] <- 48.s
        Value *rt = b.LoadGPR(instr.rd);
        rt = b.SignExtend(b.Truncate(rt, INT16_TYPE), INT64_TYPE);
        b.StoreGPR(instr.ra, rt);
        if (instr.rc) {
          b.UpdateCRx(0, rt);
        }
        return 0;
      }

      int HIRInstrEmit_extswx(HIRBuilder &b, const uPPCInstr &instr) {
        // s <- (RS)[32]
        // RA[32:63] <- (RS)[32:63]
        // RA[0:31] <- i32.s
        Value *rt = b.LoadGPR(instr.rd);
        rt = b.SignExtend(b.Truncate(rt, INT32_TYPE), INT64_TYPE);
        b.StoreGPR(instr.ra, rt);
        if (instr.rc) {
          b.UpdateCRx(0, rt);
        }
        return 0;
      }

      int HIRInstrEmit_nandx(HIRBuilder &b, const uPPCInstr &instr) {
        // RA <- ¬((RS) & (RB))
        Value *ra = b.Not(b.And(b.LoadGPR(instr.rd), b.LoadGPR(instr.rb)));
        b.StoreGPR(instr.ra, ra);
        if (instr.rc) {
          b.UpdateCRx(0, ra);
        }
        return 0;
      }

      int HIRInstrEmit_norx(HIRBuilder &b, const uPPCInstr &instr) {
        // RA <- ¬((RS) | (RB))
        Value *ra = b.Not(b.Or(b.LoadGPR(instr.rd), b.LoadGPR(instr.rb)));
        b.StoreGPR(instr.ra, ra);
        if (instr.rc) {
          b.UpdateCRx(0, ra);
        }
        return 0;
      }

      int HIRInstrEmit_orx(HIRBuilder &b, const uPPCInstr &instr) {
        // RA <- (RS) | (RB)
        if (instr.rd == instr.rb && instr.rd == instr.ra && !instr.rc) {
          // Sometimes used as no-op.
          b.Nop();
          return 0;
        }
        Value *ra;
        if (instr.rd == instr.rb) {
          ra = b.LoadGPR(instr.rd);
        }
        else {
          ra = b.Or(b.LoadGPR(instr.rd), b.LoadGPR(instr.rb));
        }
        b.StoreGPR(instr.ra, ra);
        if (instr.rc) {
          b.UpdateCRx(0, ra);
        }
        return 0;
      }

      int HIRInstrEmit_orcx(HIRBuilder &b, const uPPCInstr &instr) {
        // RA <- (RS) | ¬(RB)
        Value *ra = b.Or(b.LoadGPR(instr.rd), b.Not(b.LoadGPR(instr.rb)));
        b.StoreGPR(instr.ra, ra);
        if (instr.rc) {
          b.UpdateCRx(0, ra);
        }
        return 0;
      }

      int HIRInstrEmit_ori(HIRBuilder &b, const uPPCInstr &instr) {
        // RA <- (RS) | (i48.0 || UI)
        if (!instr.ra && !instr.rd && !instr.simm16) {
          b.Nop();
          return 0;
        }
        Value *ra = b.Or(b.LoadGPR(instr.rd), b.LoadConstantUint64(ZeroExtend16(instr.simm16)));
        b.StoreGPR(instr.ra, ra);
        return 0;
      }

      int HIRInstrEmit_oris(HIRBuilder &b, const uPPCInstr &instr) {
        // RA <- (RS) | (i32.0 || UI || i16.0)
        Value *ra =
          b.Or(b.LoadGPR(instr.rd), b.LoadConstantUint64(ZeroExtend16(instr.simm16) << 16));
        b.StoreGPR(instr.ra, ra);
        return 0;
      }

      int HIRInstrEmit_xorx(HIRBuilder &b, const uPPCInstr &instr) {
        // RA <- (RS) XOR (RB)
        Value *ra = b.Xor(b.LoadGPR(instr.rd), b.LoadGPR(instr.rb));
        b.StoreGPR(instr.ra, ra);
        if (instr.rc) {
          b.UpdateCRx(0, ra);
        }
        return 0;
      }

      int HIRInstrEmit_xori(HIRBuilder &b, const uPPCInstr &instr) {
        // RA <- (RS) XOR (i48.0 || UI)
        Value *ra = b.Xor(b.LoadGPR(instr.rd), b.LoadConstantUint64(ZeroExtend16(instr.simm16)));
        b.StoreGPR(instr.ra, ra);
        return 0;
      }

      int HIRInstrEmit_xoris(HIRBuilder &b, const uPPCInstr &instr) {
        // RA <- (RS) XOR (i32.0 || UI || i16.0)
        Value *ra =
          b.Xor(b.LoadGPR(instr.rd), b.LoadConstantUint64(ZeroExtend16(instr.simm16) << 16));
        b.StoreGPR(instr.ra, ra);
        return 0;
      }

      // Integer rotate (A-6)

      int HIRInstrEmit_rldclx(HIRBuilder &b, const uPPCInstr &instr) {
        // n <- rB[58:63]
        // r <- ROTL[64](rS, n)
        // b <- mb[5] || mb[0:4]
        // m <- MASK(b, 63)
        // rA <- r & m
        Value *n = b.And(b.Truncate(b.LoadGPR(instr.rb), INT8_TYPE),
          b.LoadConstantInt8(0x3F));

        u32 mb = instr.mbe64;
        u64 m = CreateMask(mb, 63);
        Value *v = b.LoadGPR(instr.rd);

        v = b.RotateLeft(v, n);
        if (m != 0xFFFFFFFFFFFFFFFF) {
          v = b.And(v, b.LoadConstantUint64(m));
        }

        b.StoreGPR(instr.ra, v);
        if (instr.rc) {
          b.UpdateCR0(v);
        }
        return 0;
      }

      int HIRInstrEmit_rldcrx(HIRBuilder &b, const uPPCInstr &instr) {
        // n <- rB[58:63]
        // r <- ROTL[64](rS, n)
        // b <- mb[5] || mb[0:4]
        // m <- MASK(0, b)
        // rA <- r & m
        Value *n = b.And(b.Truncate(b.LoadGPR(instr.rb), INT8_TYPE),
          b.LoadConstantInt8(0x3F));

        u32 mb = instr.mbe64;
        u64 m = CreateMask(0, mb);
        Value *v = b.LoadGPR(instr.rd);

        v = b.RotateLeft(v, n);
        if (m != 0xFFFFFFFFFFFFFFFF) {
          v = b.And(v, b.LoadConstantUint64(m));
        }

        b.StoreGPR(instr.ra, v);
        if (instr.rc) {
          b.UpdateCR0(v);
        }
        return 0;
      }

      int HIRInstrEmit_rldicx(HIRBuilder &b, const uPPCInstr &instr) {
        u32 sh = instr.sh64;
        u32 mb = instr.mbe64;
        u64 m = CreateMask(mb, instr.sh64 ^ 63);
        Value *v = b.LoadGPR(instr.rd);
        if (sh) {
          v = b.RotateLeft(v, b.LoadConstantInt8(sh));
        }
        if (m != 0xFFFFFFFFFFFFFFFF) {
          v = b.And(v, b.LoadConstantUint64(m));
        }
        b.StoreGPR(instr.ra, v);
        if (instr.rc) {
          b.UpdateCR0(v);
        }
        return 0;
      }

      int HIRInstrEmit_rldiclx(HIRBuilder &b, const uPPCInstr &instr) {
        // n <- sh[5] || sh[0:4]
        // r <- ROTL64((RS), n)
        // b <- mb[5] || mb[0:4]
        // m <- MASK(b, 63)
        // RA <- r & m
        u32 sh = instr.sh64;
        u32 mb = instr.mbe64;
        u64 m = CreateMask(mb, 63);
        Value *v = b.LoadGPR(instr.rd);
        if (sh == 64 - mb) {
          // srdi == rldicl ra,rs,64-n,n
          v = b.Shr(v, int8_t(mb));
        }
        else {
          if (sh) {
            v = b.RotateLeft(v, b.LoadConstantInt8(sh));
          }
          if (m != 0xFFFFFFFFFFFFFFFF) {
            v = b.And(v, b.LoadConstantUint64(m));
          }
        }
        b.StoreGPR(instr.ra, v);
        if (instr.rc) {
          b.UpdateCR0(v);
        }
        return 0;
      }

      int HIRInstrEmit_rldicrx(HIRBuilder &b, const uPPCInstr &instr) {
        // n <- sh[5] || sh[0:4]
        // r <- ROTL64((RS), n)
        // e <- me[5] || me[0:4]
        // m <- MASK(0, e)
        // RA <- r & m
        u32 sh = instr.sh64;
        u32 mb = instr.mbe64;
        u64 m = CreateMask(0, mb);
        Value *v = b.LoadGPR(instr.rd);
        if (mb == 63 - sh) {
          // sldi ==  rldicr ra,rs,n,63-n
          v = b.Shl(v, int8_t(sh));
        }
        else {
          if (sh) {
            v = b.RotateLeft(v, b.LoadConstantInt8(sh));
          }
          if (m != 0xFFFFFFFFFFFFFFFF) {
            v = b.And(v, b.LoadConstantUint64(m));
          }
        }
        b.StoreGPR(instr.ra, v);
        if (instr.rc) {
          b.UpdateCR0(v);
        }
        return 0;
      }

      int HIRInstrEmit_rldimix(HIRBuilder &b, const uPPCInstr &instr) {
        // n <- sh[5] || sh[0:4]
        // r <- ROTL64((RS), n)
        // b <- me[5] || me[0:4]
        // m <- MASK(b, ¬n)
        // RA <- (r & m) | ((RA)&¬m)
        u32 sh = instr.sh64;
        u32 mb = instr.mbe64;
        u64 m = CreateMask(mb, sh ^ 63);
        Value *v = b.LoadGPR(instr.rd);
        if (sh) {
          v = b.RotateLeft(v, b.LoadConstantInt8(sh));
        }
        if (m != 0xFFFFFFFFFFFFFFFF) {
          Value *ra = b.LoadGPR(instr.ra);
          v = b.Or(b.And(v, b.LoadConstantUint64(m)),
            b.And(ra, b.LoadConstantUint64(~m)));
        }
        b.StoreGPR(instr.ra, v);
        if (instr.rc) {
          b.UpdateCR0(v);
        }
        return 0;
      }

      int HIRInstrEmit_rlwimix(HIRBuilder &b, const uPPCInstr &instr) {
        // n <- SH
        // r <- ROTL32((RS)[32:63], n)
        // m <- MASK(MB+32, ME+32)
        // RA <- r&m | (RA)&¬m
        Value *v = b.LoadGPR(instr.rd);
        // (x||x)
        v = b.Or(b.Shl(v, 32), b.ZeroExtend(b.Truncate(v, INT32_TYPE), INT64_TYPE));
        if (instr.sh32) {
          v = b.RotateLeft(v, b.LoadConstantInt8(instr.sh32));
        }
        // Compiler sometimes masks with 0xFFFFFFFF (identity) - avoid the work here
        // as our truncation/zero-extend does it for us.
        u64 m = CreateMask(instr.mb32 + 32, instr.me32 + 32);
        if (m != 0xFFFFFFFFFFFFFFFFull) {
          v = b.And(v, b.LoadConstantUint64(m));
        }
        v = b.Or(v, b.And(b.LoadGPR(instr.ra), b.LoadConstantUint64(~m)));
        b.StoreGPR(instr.ra, v);
        if (instr.rc) {
          b.UpdateCR0(v);
        }
        return 0;
      }

      int HIRInstrEmit_rlwinmx(HIRBuilder &b, const uPPCInstr &instr) {
        // n <- SH
        // r <- ROTL32((RS)[32:63], n)
        // m <- MASK(MB+32, ME+32)
        // RA <- r & m
        Value *v = b.LoadGPR(instr.rd);

        // (x||x)
        v = b.Or(b.Shl(v, 32), b.ZeroExtend(b.Truncate(v, INT32_TYPE), INT64_TYPE));

        // TODO(benvanik): optimize srwi
        // TODO(benvanik): optimize slwi
        // The compiler will generate a bunch of these for the special case of SH=0.
        // Which seems to just select some bits and set cr0 for use with a branch.
        // We can detect this and do less work.
        if (instr.sh32) {
          v = b.RotateLeft(v, b.LoadConstantInt8(instr.sh32));
        }
        // Compiler sometimes masks with 0xFFFFFFFF (identity) - avoid the work here
        // as our truncation/zero-extend does it for us.
        u64 m = CreateMask(instr.mb32 + 32, instr.me32 + 32);
        if (m != 0xFFFFFFFFFFFFFFFFull) {
          v = b.And(v, b.LoadConstantUint64(m));
        }
        b.StoreGPR(instr.ra, v);
        if (instr.rc) {
          b.UpdateCR0(v);
        }
        return 0;
      }

      int HIRInstrEmit_rlwnmx(HIRBuilder &b, const uPPCInstr &instr) {
        // n <- (RB)[59:63]
        // r <- ROTL32((RS)[32:63], n)
        // m <- MASK(MB+32, ME+32)
        // RA <- r & m
        Value *sh =
          b.And(b.Truncate(b.LoadGPR(instr.sh32), INT8_TYPE), b.LoadConstantInt8(0x1F));
        Value *v = b.LoadGPR(instr.rd);
        // (x||x)
        v = b.Or(b.Shl(v, 32), b.ZeroExtend(b.Truncate(v, INT32_TYPE), INT64_TYPE));
        v = b.RotateLeft(v, sh);
        v = b.And(v, b.LoadConstantUint64(CreateMask(instr.mb32 + 32, instr.me32 + 32)));
        b.StoreGPR(instr.ra, v);
        if (instr.rc) {
          b.UpdateCR0(v);
        }
        return 0;
      }

      // Integer shift (A-7)

      int HIRInstrEmit_sldx(HIRBuilder &b, const uPPCInstr &instr) {
        // n <- (RB)[58:63]
        // r <- ROTL64((RS), n)
        // if (RB)[57] = 0 then
        //   m <- MASK(0, 63-n)
        // else
        //   m <- i64.0
        // RA <- r & m
        Value *sh = b.And(b.Truncate(b.LoadGPR(instr.rb), INT8_TYPE), b.LoadConstantInt8(0x7F));
        Value *v = b.Select(b.IsTrue(b.Shr(sh, 6)), b.LoadZeroInt64(), b.Shl(b.LoadGPR(instr.rd), sh));
        b.StoreGPR(instr.ra, v);
        if (instr.rc) {
          b.UpdateCR0(v);
        }
        return 0;
      }

      int HIRInstrEmit_slwx(HIRBuilder &b, const uPPCInstr &instr) {
        // n <- (RB)[59:63]
        // r <- ROTL32((RS)[32:63], n)
        // if (RB)[58] = 0 then
        //   m <- MASK(32, 63-n)
        // else
        //   m <- i64.0
        // RA <- r & m
        Value *sh = b.And(b.Truncate(b.LoadGPR(instr.rb), INT8_TYPE), b.LoadConstantInt8(0x3F));
        Value *v = b.Select(b.IsTrue(b.Shr(sh, 5)), b.LoadZeroInt32(), 
          b.Shl(b.Truncate(b.LoadGPR(instr.rd), INT32_TYPE), sh));
        v = b.ZeroExtend(v, INT64_TYPE);
        b.StoreGPR(instr.ra, v);
        if (instr.rc) {
          b.UpdateCR0(v);
        }
        return 0;
      }

      int HIRInstrEmit_srdx(HIRBuilder &b, const uPPCInstr &instr) {
        // n <- (RB)[58:63]
        // r <- ROTL64((RS), 64-n)
        // if (RB)[57] = 0 then
        //   m <- MASK(n, 63)
        // else
        //   m <- i64.0
        // RA <- r & m
        Value *sh = b.And(b.Truncate(b.LoadGPR(instr.rb), INT8_TYPE), b.LoadConstantInt8(0x7F));
        Value *v = b.Select(b.IsTrue(b.And(sh, b.LoadConstantInt8(0x40))),
          b.LoadZeroInt64(), b.Shr(b.LoadGPR(instr.rd), sh));
        b.StoreGPR(instr.ra, v);
        if (instr.rc) {
          b.UpdateCR0(v);
        }
        return 0;
      }

      int HIRInstrEmit_srwx(HIRBuilder &b, const uPPCInstr &instr) {
        // n <- (RB)[59:63]
        // r <- ROTL32((RS)[32:63], 64-n)
        // if (RB)[58] = 0 then
        //   m <- MASK(n+32, 63)
        // else
        //   m <- i64.0
        // RA <- r & m
        Value *sh = b.And(b.Truncate(b.LoadGPR(instr.rb), INT8_TYPE), b.LoadConstantInt8(0x3F));
        Value *v = b.Select(b.IsTrue(b.And(sh, b.LoadConstantInt8(0x20))), b.LoadZeroInt32(),
            b.Shr(b.Truncate(b.LoadGPR(instr.rd), INT32_TYPE), sh));
        v = b.ZeroExtend(v, INT64_TYPE);
        b.StoreGPR(instr.ra, v);
        if (instr.rc) {
          b.UpdateCR0(v);
        }
        return 0;
      }

      int HIRInstrEmit_sradx(HIRBuilder &b, const uPPCInstr &instr) {
        // n <- rB[58-63]
        // r <- ROTL[64](rS, 64 - n)
        // if rB[57] = 0 then m ← MASK(n, 63)
        // else m ← (64)0
        // S ← rS[0]
        // rA <- (r & m) | (((64)S) & ¬ m)
        // XER[CA] <- S & ((r & ¬ m) ¦ 0)
        // if n == 0: rA <- rS, XER[CA] = 0
        // if n >= 64: rA <- 64 sign bits of rS, XER[CA] = sign bit of rS
        Value *rt = b.LoadGPR(instr.rd);
        Value *sh = b.And(b.Truncate(b.LoadGPR(instr.rb), INT8_TYPE), b.LoadConstantInt8(0x7F));
        Value *clamp_sh = b.Min(sh, b.LoadConstantInt8(0x3F));
        Value *v = b.Sha(rt, clamp_sh);

        // CA is set if any bits are shifted out of the right and if the result
        // is negative.
        Value *ca = b.And(b.IsTrue(b.Shr(rt, 63)), b.CompareNE(b.Shl(v, clamp_sh), rt));
        b.StoreCA(ca);

        b.StoreGPR(instr.ra, v);
        if (instr.rc) {
          b.UpdateCR0(v);
        }
        return 0;
      }

      int HIRInstrEmit_sradix(HIRBuilder &b, const uPPCInstr &instr) {
        // n <- sh[5] || sh[0-4]
        // r <- ROTL[64](rS, 64 - n)
        // m ← MASK(n, 63)
        // S ← rS[0]
        // rA <- (r & m) | (((64)S) & ¬ m)
        // XER[CA] <- S & ((r & ¬ m) ¦ 0)
        // if n == 0: rA <- rS, XER[CA] = 0
        // if n >= 64: rA <- 64 sign bits of rS, XER[CA] = sign bit of rS
        Value *v = b.LoadGPR(instr.rd);
        int8_t sh = instr.sh64;

        // CA is set if any bits are shifted out of the right and if the result
        // is negative.
        if (sh) {
          u64 mask = CreateMask(64 - sh, 63);
          Value *ca = b.And(b.Truncate(b.Shr(v, 63), INT8_TYPE), b.IsTrue(b.And(v, b.LoadConstantUint64(mask))));
          b.StoreCA(ca);

          v = b.Sha(v, sh);
        } else { b.StoreCA(b.LoadZeroInt8()); }

        b.StoreGPR(instr.ra, v);
        if (instr.rc) {
          b.UpdateCR0(v);
        }
        return 0;
      }

      int HIRInstrEmit_srawx(HIRBuilder &b, const uPPCInstr &instr) {
        // n <- rB[59-63]
        // r <- ROTL32((RS)[32:63], 64-n)
        // m <- MASK(n+32, 63)
        // s <- (RS)[32]
        // RA <- r&m | (i64.s)&¬m
        // CA <- s & ((r&¬m)[32:63]≠0)
        // if n == 0: rA <- sign_extend(rS), XER[CA] = 0
        // if n >= 32: rA <- 64 sign bits of rS, XER[CA] = sign bit of lo_32(rS)
        Value *rt = b.Truncate(b.LoadGPR(instr.rd), INT32_TYPE);
        Value *sh = b.And(b.Truncate(b.LoadGPR(instr.rb), INT8_TYPE), b.LoadConstantInt8(0x3F));
        Value *clamp_sh = b.Min(sh, b.LoadConstantInt8(0x1F));
        Value *v = b.Sha(rt, b.Min(sh, clamp_sh));

        // CA is set if any bits are shifted out of the right and if the result
        // is negative.
        Value *ca = b.And(b.IsTrue(b.Shr(rt, 31)), b.CompareNE(b.Shl(v, clamp_sh), rt));
        b.StoreCA(ca);

        v = b.SignExtend(v, INT64_TYPE);
        b.StoreGPR(instr.ra, v);
        if (instr.rc) {
          b.UpdateCR0(v);
        }
        return 0;
      }

      int HIRInstrEmit_srawix(HIRBuilder &b, const uPPCInstr &instr) {
        // n <- SH
        // r <- ROTL32((RS)[32:63], 64-n)
        // m <- MASK(n+32, 63)
        // s <- (RS)[32]
        // RA <- r&m | (i64.s)&¬m
        // CA <- s & ((r&¬m)[32:63]≠0)
        // if n == 0: rA <- sign_extend(rS), XER[CA] = 0
        // if n >= 32: rA <- 64 sign bits of rS, XER[CA] = sign bit of lo_32(rS)
        Value *v = b.Truncate(b.LoadGPR(instr.rd), INT32_TYPE);
        Value *ca;
        if (!instr.rb) {
          // No shift, just a fancy sign extend and CA clearer.
          v = b.SignExtend(v, INT64_TYPE);
          ca = b.LoadZeroInt8();
        }
        else {
          // CA is set if any bits are shifted out of the right and if the result
          // is negative.
          u32 mask = (u32)CreateMask(64 - instr.rb, 63);
          ca = b.And(b.Truncate(b.Shr(v, 31), INT8_TYPE), b.IsTrue(b.And(v, b.LoadConstantUint32(mask))));

          v = b.Sha(v, (int8_t)instr.rb), v = b.SignExtend(v, INT64_TYPE);
        }
        b.StoreCA(ca);
        b.StoreGPR(instr.ra, v);
        if (instr.rc) {
          b.UpdateCR0(v);
        }
        return 0;
      }

      // Condition Register logical instructions

      // Helper to extract a single CR bit (0-31) from the CR register.
      static Value *LoadCRBit(HIRBuilder &b, u32 bitIndex) {
        // CR is stored as a 32-bit value with bit 0 being the MSB.
        // To extract bit N, we shift right by (31 - N) and mask with 1.
        Value *cr = b.LoadCR();
        Value *shifted = b.Shr(cr, static_cast<s8>(31 - bitIndex));
        return b.Truncate(b.And(shifted, b.LoadConstantUint32(1)), INT8_TYPE);
      }

      // Helper to store a single CR bit (0-31) into the CR register.
      static void StoreCRBit(HIRBuilder &b, u32 bitIndex, Value *bitValue) {
        // Create a mask to clear the target bit.
        u32 mask = ~(1u << (31 - bitIndex));
        Value *cr = b.LoadCR();
        // Clear the bit.
        cr = b.And(cr, b.LoadConstantUint32(mask));
        // Shift the new bit value into position and OR it in.
        Value *shiftedBit = b.Shl(b.ZeroExtend(bitValue, INT32_TYPE), static_cast<s8>(31 - bitIndex));
        cr = b.Or(cr, shiftedBit);
        b.StoreCR(cr);
      }

      int HIRInstrEmit_crand(HIRBuilder &b, const uPPCInstr &instr) {
        // CR[crbD] <- CR[crbA] & CR[crbB]
        Value *cra = LoadCRBit(b, instr.crba);
        Value *crb = LoadCRBit(b, instr.crbb);
        Value *result = b.And(cra, crb);
        StoreCRBit(b, instr.crbd, result);
        return 0;
      }

      int HIRInstrEmit_crandc(HIRBuilder &b, const uPPCInstr &instr) {
        // CR[crbD] <- CR[crbA] & ~CR[crbB]
        Value *cra = LoadCRBit(b, instr.crba);
        Value *crb = LoadCRBit(b, instr.crbb);
        Value *notB = b.Xor(crb, b.LoadConstantInt8(1));
        Value *result = b.And(cra, notB);
        StoreCRBit(b, instr.crbd, result);
        return 0;
      }

      int HIRInstrEmit_creqv(HIRBuilder &b, const uPPCInstr &instr) {
        // CR[crbD] <- CR[crbA] == CR[crbB] (XNOR)
        Value *cra = LoadCRBit(b, instr.crba);
        Value *crb = LoadCRBit(b, instr.crbb);
        // XNOR = NOT(XOR) = 1 XOR (a XOR b)
        Value *xorResult = b.Xor(cra, crb);
        Value *result = b.Xor(xorResult, b.LoadConstantInt8(1));
        StoreCRBit(b, instr.crbd, result);
        return 0;
      }

      int HIRInstrEmit_crnand(HIRBuilder &b, const uPPCInstr &instr) {
        // CR[crbD] <- ~(CR[crbA] & CR[crbB])
        Value *cra = LoadCRBit(b, instr.crba);
        Value *crb = LoadCRBit(b, instr.crbb);
        Value *andResult = b.And(cra, crb);
        Value *result = b.Xor(andResult, b.LoadConstantInt8(1));
        StoreCRBit(b, instr.crbd, result);
        return 0;
      }

      int HIRInstrEmit_crnor(HIRBuilder &b, const uPPCInstr &instr) {
        // CR[crbD] <- ~(CR[crbA] | CR[crbB])
        Value *cra = LoadCRBit(b, instr.crba);
        Value *crb = LoadCRBit(b, instr.crbb);
        Value *orResult = b.Or(cra, crb);
        Value *result = b.Xor(orResult, b.LoadConstantInt8(1));
        StoreCRBit(b, instr.crbd, result);
        return 0;
      }

      int HIRInstrEmit_cror(HIRBuilder &b, const uPPCInstr &instr) {
        // CR[crbD] <- CR[crbA] | CR[crbB]
        Value *cra = LoadCRBit(b, instr.crba);
        Value *crb = LoadCRBit(b, instr.crbb);
        Value *result = b.Or(cra, crb);
        StoreCRBit(b, instr.crbd, result);
        return 0;
      }

      int HIRInstrEmit_crorc(HIRBuilder &b, const uPPCInstr &instr) {
        // CR[crbD] <- CR[crbA] | ~CR[crbB]
        Value *cra = LoadCRBit(b, instr.crba);
        Value *crb = LoadCRBit(b, instr.crbb);
        Value *notB = b.Xor(crb, b.LoadConstantInt8(1));
        Value *result = b.Or(cra, notB);
        StoreCRBit(b, instr.crbd, result);
        return 0;
      }

      int HIRInstrEmit_crxor(HIRBuilder &b, const uPPCInstr &instr) {
        // CR[crbD] <- CR[crbA] ^ CR[crbB]
        Value *cra = LoadCRBit(b, instr.crba);
        Value *crb = LoadCRBit(b, instr.crbb);
        Value *result = b.Xor(cra, crb);
        StoreCRBit(b, instr.crbd, result);
        return 0;
      }
    }
  }
}