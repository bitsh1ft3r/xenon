/***************************************************************/
/* Copyright 2026 Xenon Emulator Project. All rights reserved. */
/***************************************************************/

#include "Base/Arch.h"

#if defined(ARCH_X86) || defined(ARCH_X86_64)

#include <cstring>

#include "Base/Logging/Log.h"
#include "Core/XCPU/JIT/x86VectorConstants.h"
#include "Core/XCPU/JIT/Backend/X86Backend/x86CodeGenBackend.h"
#include "Core/XCPU/JIT/Backend/X86Backend/x86CodeGenHelpers.h"

namespace Xe {
  namespace XCPU {
    namespace JIT {

      //
      // Convert Vector elements from Int to Float
      //

      REGISTER_EMITTER(OPCODE_VECTOR_CONVERT_I2F, Emit_VECTOR_CONVERT_I2F)
      static void Emit_VECTOR_CONVERT_I2F(x86CodeGenBackend *b, const HIR::Instr *instr) {
        // Destination
        x86::Xmm dst = allocXmm();
        // Get sources      
        x86::Xmm src1 = LoadValueXmm(b, instr->src1.value);

        // Unsigned?
        if (instr->flags & HIR::ARITHMETIC_UNSIGNED) {
          // Unsigned int32 to float with correct rounding for large values.
          // See Xenia comments about double rounding for values >= 0x80000000.
          x86::Xmm xmm0 = allocXmm();
          x86::Xmm xmm1 = allocXmm();

          // Round manually to (1.stored mantissa bits * 2^31) or to 2^32 to the
          // nearest even (the only rounding mode used on AltiVec) if the number is
          // 0x80000000 or greater, instead of converting src & 0x7FFFFFFF and then
          // adding 2147483648.0f, which results in double rounding that can give a
          // result larger than needed - see OPCODE_VECTOR_CONVERT_I2F notes.

          // [0x80000000, 0xFFFFFFFF] case:

          // Round to the nearest even, from (0x80000000 | 31 stored mantissa bits)
          // to ((-1 << 23) | 23 stored mantissa bits), or to 0 if the result should
          // be 4294967296.0f.
          COMP->vpaddd(xmm1, src1, LoadXmmConst(b, XMMInt127));
          COMP->vpslld(xmm0, src1, Imm(31 - 8));
          COMP->vpsrld(xmm0, xmm0, Imm(31));
          COMP->vpaddd(xmm0, xmm0, xmm1);
          // xmm0 = (0xFF800000 | 23 explicit mantissa bits), or 0 if overflowed
          COMP->vpsrad(xmm0, xmm0, Imm(8));
          // Calculate the result for the [0x80000000, 0xFFFFFFFF] case - take the
          // rounded mantissa, and add -1 or 0 to the exponent of 32, depending on
          // whether the number should be (1.stored mantissa bits * 2^31) or 2^32.
          // xmm0 = [0x80000000, 0xFFFFFFFF] case result
          COMP->vpaddd(xmm0, xmm0, LoadXmmConst(b, XMM2To32));

          // [0x00000000, 0x7FFFFFFF] case 
          // (during vblendvps reg -> vpaddd reg -> vpaddd mem dependency):

          // Convert from signed integer to float.
          // xmm1 = [0x00000000, 0x7FFFFFFF] case result
          COMP->vcvtdq2ps(xmm1, src1);

          // Merge the two ways depending on whether the number is >= 0x80000000 (has high bit set).
          COMP->vblendvps(dst, xmm1, xmm0, src1);
        } else {
          COMP->vcvtdq2ps(dst, src1);
        }

        // Store result
        TagStoreReg(instr->dest, dst);
      }

      //
      // Convert Vector elements from Float to Int
      //

      REGISTER_EMITTER(OPCODE_VECTOR_CONVERT_F2I, Emit_VECTOR_CONVERT_F2I)
      static void Emit_VECTOR_CONVERT_F2I(x86CodeGenBackend *b, const HIR::Instr *instr) {
        // Destination
        x86::Xmm dst = allocXmm();
        // Get sources      
        x86::Xmm src1 = LoadValueXmm(b, instr->src1.value);

        // Unsigned?
        if (instr->flags & HIR::ARITHMETIC_UNSIGNED) {
          x86::Xmm xmm0 = allocXmm();
          x86::Xmm xmm1 = allocXmm();
          x86::Xmm xmm2 = allocXmm();

          // Clamp to min 0
          COMP->vmaxps(xmm0, src1, LoadXmmConst(b, XMMZero));

          // Mask of values >= 2^31.
          COMP->vcmpps(xmm1, xmm0, LoadXmmConst(b, XMMPosIntMinPS), Imm(0x0D)); // _CMP_GE_OS
          
          // Scale values >= 2^31 back to [0, ...]
          COMP->vsubps(xmm2, xmm0, LoadXmmConst(b, XMMPosIntMinPS));
          COMP->vblendvps(xmm0, xmm0, xmm2, xmm1);

          // Convert [0, INT_MAX].
          COMP->vcvttps2dq(dst, xmm0);

          // Detect saturation (values that overflowed to 0x80000000).
          COMP->vpcmpeqd(xmm0, dst, LoadXmmConst(b, XMMIntMin));

          // Add INT_MIN back for originally-high values.
          COMP->vpand(xmm1, xmm1, LoadXmmConst(b, XMMIntMin));
          COMP->vpaddd(dst, dst, xmm1);

          // Saturate overflows to 0xFFFFFFFF.
          COMP->vpor(dst, dst, xmm0);
        } else {
          x86::Xmm xmm0 = allocXmm();
          x86::Xmm xmm1 = allocXmm();
          x86::Xmm xmm2 = allocXmm();

          // NaN mask.
          COMP->vcmpps(xmm2, src1, src1, Imm(0x03)); // _CMP_UNORD_Q

          // Convert.
          COMP->vcvttps2dq(xmm0, src1);

          // Detect indeterminate (overflow to 0x80000000) where src >= 0.
          COMP->vpcmpeqd(xmm1, xmm0, LoadXmmConst(b, XMMIntMin));
          COMP->vpandn(xmm1, src1, xmm1);

          // Saturate positive overflows to INT_MAX.
          COMP->vblendvps(dst, xmm0, LoadXmmConst(b, XMMIntMax), xmm1);

          // Zero out NaN results.
          COMP->vpandn(dst, xmm2, dst);
        }

        // Store result
        TagStoreReg(instr->dest, dst);
      }

      //
      // Load Vector Shift Left
      //

      REGISTER_EMITTER(OPCODE_LOAD_VECTOR_SHL, Emit_LOAD_VECTOR_SHL)
        static void Emit_LOAD_VECTOR_SHL(x86CodeGenBackend *b, const HIR::Instr *instr) {
        // Destination
        x86::Xmm dst = allocXmm();
        // Get sources
        x86::Gp src1 = LoadValueGp(b, instr->src1.value);
        // Table address (lvsl table)
        x86::Gp tableAddr = newGP64();
        // Table index
        x86::Gp idx = newGP64();

        COMP->movzx(idx.r32(), src1);
        COMP->and_(idx, 0xF);
        COMP->shl(idx, Imm(4));
        // Get final address: tableAddr = &table + idx
        COMP->mov(tableAddr, Imm(reinterpret_cast<uintptr_t>(&loadVectorShiftLeftTable)));
        COMP->add(tableAddr, idx);
        COMP->vmovaps(dst, x86::ptr(tableAddr));
        // Store result
        TagStoreReg(instr->dest, dst);
      }

      //
      // Load Vector Shift Right
      //

      REGISTER_EMITTER(OPCODE_LOAD_VECTOR_SHR, Emit_LOAD_VECTOR_SHR)
        static void Emit_LOAD_VECTOR_SHR(x86CodeGenBackend *b, const HIR::Instr *instr) {
        // Destination
        x86::Xmm dst = allocXmm();
        // Get sources
        x86::Gp src1 = LoadValueGp(b, instr->src1.value);
        // Table address (lvsl table)
        x86::Gp tableAddr = newGP64();
        // Table index
        x86::Gp idx = newGP64();

        COMP->movzx(idx.r32(), src1);
        COMP->and_(idx, 0xF);
        COMP->shl(idx, Imm(4));
        // Get final address: tableAddr = &table + idx
        COMP->mov(tableAddr, Imm(reinterpret_cast<uintptr_t>(&loadVectorShiftRightTable)));
        COMP->add(tableAddr, idx);
        COMP->vmovaps(dst, x86::ptr(tableAddr));
        // Store result
        TagStoreReg(instr->dest, dst);
      }

      //
      // Vector Max
      //

      REGISTER_EMITTER(OPCODE_VECTOR_MAX, Emit_VECTOR_MAX)
        static void Emit_VECTOR_MAX(x86CodeGenBackend *b, const HIR::Instr *instr) {
        // Destination
        x86::Xmm dst = allocXmm();
        // Get sources
        x86::Xmm src1 = LoadValueXmm(b, instr->src1.value);
        x86::Xmm src2 = LoadValueXmm(b, instr->src2.value);
       
        // Get part type
        HIR::TypeName partType = static_cast<HIR::TypeName>(instr->flags >> 8);
        
        // Unsigned?
        if (instr->flags & HIR::ARITHMETIC_UNSIGNED) {
          switch (partType) {
          case HIR::INT8_TYPE:
            COMP->vpmaxub(dst, src1, src2);
            break;
          case HIR::INT16_TYPE:
            COMP->vpmaxuw(dst, src1, src2);
            break;
          case HIR::INT32_TYPE:
            COMP->vpmaxud(dst, src1, src2);
            break;
          default: UNREACHABLE_MSG("Unimplemented VECTOR_MAX Unsigned type."); return;
          }
        } else {
          switch (partType) {
          case HIR::INT8_TYPE:
            COMP->vpmaxsb(dst, src1, src2);
            break;
          case HIR::INT16_TYPE:
            COMP->vpmaxsw(dst, src1, src2);
            break;
          case HIR::INT32_TYPE:
            COMP->vpmaxsd(dst, src1, src2);
            break;
          default: UNREACHABLE_MSG("Unimplemented VECTOR_MAX signed type."); return;
          }
        }
        // Store result
        TagStoreReg(instr->dest, dst);
      }

      // 
      // Vector Min
      // 
      
      REGISTER_EMITTER(OPCODE_VECTOR_MIN, Emit_VECTOR_MIN)
        static void Emit_VECTOR_MIN(x86CodeGenBackend *b, const HIR::Instr *instr) {
        // Destination
        x86::Xmm dst = allocXmm();
        // Get sources
        x86::Xmm src1 = LoadValueXmm(b, instr->src1.value);
        x86::Xmm src2 = LoadValueXmm(b, instr->src2.value);

        // Get part type
        HIR::TypeName partType = static_cast<HIR::TypeName>(instr->flags >> 8);

        // Unsigned?
        if (instr->flags & HIR::ARITHMETIC_UNSIGNED) {
          switch (partType) {
          case HIR::INT8_TYPE:
            COMP->vpminub(dst, src1, src2);
            break;
          case HIR::INT16_TYPE:
            COMP->vpminuw(dst, src1, src2);
            break;
          case HIR::INT32_TYPE:
            COMP->vpminud(dst, src1, src2);
            break;
          default: UNREACHABLE_MSG("Unimplemented VECTOR_MIX Unsigned type."); return;
          }
        } else {
          switch (partType) {
          case HIR::INT8_TYPE:
            COMP->vpminsb(dst, src1, src2);
            break;
          case HIR::INT16_TYPE:
            COMP->vpminsw(dst, src1, src2);
            break;
          case HIR::INT32_TYPE:
            COMP->vpminsd(dst, src1, src2);
            break;
          default: UNREACHABLE_MSG("Unimplemented VECTOR_MIX signed type."); return;
          }
        }
        // Store result
        TagStoreReg(instr->dest, dst);
      }


      //
      // Vector Compare Equal
      //

      REGISTER_EMITTER(OPCODE_VECTOR_COMPARE_EQ, Emit_VECTOR_COMPARE_EQ)
      static void Emit_VECTOR_COMPARE_EQ(x86CodeGenBackend *b, const HIR::Instr *instr) {
        // Destination
        x86::Xmm dst = allocXmm();
        // Get sources
        x86::Xmm src1 = LoadValueXmm(b, instr->src1.value);
        x86::Xmm src2 = LoadValueXmm(b, instr->src2.value);
        // Get part type
        HIR::TypeName partType = static_cast<HIR::TypeName>(instr->flags);
        
        switch (partType) {
        case HIR::INT8_TYPE:
          COMP->vpcmpeqb(dst, src1, src2);
          break;
        case HIR::INT16_TYPE:
          COMP->vpcmpeqw(dst, src1, src2);
          break;
        case HIR::INT32_TYPE:
          COMP->vpcmpeqd(dst, src1, src2);
          break;
        case HIR::FLOAT32_TYPE:
          COMP->vcmpps(dst, src1, src2, Imm(0x00)); // _CMP_EQ_OQ
          break;
        default: UNREACHABLE_MSG("Unimplemented VECTOR_COMPARE_EQ type."); return;
        }
        // Store result
        TagStoreReg(instr->dest, dst);
      }

      //
      // Vector Compare Signed Greater Than
      //

      REGISTER_EMITTER(OPCODE_VECTOR_COMPARE_SGT, Emit_VECTOR_COMPARE_SGT)
      static void Emit_VECTOR_COMPARE_SGT(x86CodeGenBackend *b, const HIR::Instr *instr) {
        // Destination
        x86::Xmm dst = allocXmm();
        // Get sources
        x86::Xmm src1 = LoadValueXmm(b, instr->src1.value);
        x86::Xmm src2 = LoadValueXmm(b, instr->src2.value);
        // Get part type
        HIR::TypeName partType = static_cast<HIR::TypeName>(instr->flags);

        switch (partType) {
        case HIR::INT8_TYPE:
          COMP->vpcmpgtb(dst, src1, src2);
          break;
        case HIR::INT16_TYPE:
          COMP->vpcmpgtw(dst, src1, src2);
          break;
        case HIR::INT32_TYPE:
          COMP->vpcmpgtd(dst, src1, src2);
          break;
        case HIR::FLOAT32_TYPE:
          COMP->vcmpps(dst, src1, src2, Imm(0x0E)); // _CMP_GT_OS
          break;
        default: UNREACHABLE_MSG("Unimplemented VECTOR_COMPARE_SGT type."); return;
        }
        // Store result
        TagStoreReg(instr->dest, dst);
      }

      //
      // Vector Compare Signed Greater Equal
      //

      REGISTER_EMITTER(OPCODE_VECTOR_COMPARE_SGE, Emit_VECTOR_COMPARE_SGE)
      static void Emit_VECTOR_COMPARE_SGE(x86CodeGenBackend *b, const HIR::Instr *instr) {
        // Destination
        x86::Xmm dst = allocXmm();
        // Get sources
        x86::Xmm src1 = LoadValueXmm(b, instr->src1.value);
        x86::Xmm src2 = LoadValueXmm(b, instr->src2.value);
        // Get part type
        HIR::TypeName partType = static_cast<HIR::TypeName>(instr->flags);
        // Temp
        x86::Xmm xmm0 = allocXmm();

        switch (partType) {
        case HIR::INT8_TYPE:{
          COMP->vpcmpeqb(xmm0, src1, src2);
          COMP->vpcmpgtb(dst, src1, src2);
          COMP->vpor(dst, dst, xmm0);
          break;
        }
        case HIR::INT16_TYPE: {
          COMP->vpcmpeqw(xmm0, src1, src2);
          COMP->vpcmpgtw(dst, src1, src2);
          COMP->vpor(dst, dst, xmm0);
          break;
        }
        case HIR::INT32_TYPE: {
          COMP->vpcmpeqd(xmm0, src1, src2);
          COMP->vpcmpgtd(dst, src1, src2);
          COMP->vpor(dst, dst, xmm0);
          break;
        }
        case HIR::FLOAT32_TYPE:
          COMP->vcmpps(dst, src1, src2, Imm(0x0D)); // _CMP_GE_OS
          break;
        default: UNREACHABLE_MSG("Unimplemented VECTOR_COMPARE_SGE type."); return;
        }
        // Store result
        TagStoreReg(instr->dest, dst);
      }

      //
      // Vector Compare Unsigned Greater Than
      //

      REGISTER_EMITTER(OPCODE_VECTOR_COMPARE_UGT, Emit_VECTOR_COMPARE_UGT)
      static void Emit_VECTOR_COMPARE_UGT(x86CodeGenBackend *b, const HIR::Instr *instr) {
        // Destination
        x86::Xmm dst = allocXmm();
        // Get sources
        x86::Xmm src1 = LoadValueXmm(b, instr->src1.value);
        x86::Xmm src2 = LoadValueXmm(b, instr->src2.value);
        // Temps
        x86::Xmm xmm0 = allocXmm();
        x86::Xmm xmm1 = allocXmm();

        HIR::TypeName partType = static_cast<HIR::TypeName>(instr->flags);
        switch (partType) {
        case HIR::INT8_TYPE: {
          x86::Xmm mask = LoadXmmConst(b, XMMSignMaskI8);
          COMP->vpxor(xmm0, src1, mask);
          COMP->vpxor(xmm1, src2, mask);
          COMP->vpcmpgtb(dst, xmm0, xmm1);
          break;
        }
        case HIR::INT16_TYPE: {
          x86::Xmm mask = LoadXmmConst(b, XMMSignMaskI16);
          COMP->vpxor(xmm0, src1, mask);
          COMP->vpxor(xmm1, src2, mask);
          COMP->vpcmpgtw(dst, xmm0, xmm1);
          break;
        }
        case HIR::INT32_TYPE: {
          x86::Xmm mask = LoadXmmConst(b, XMMSignMaskI32);
          COMP->vpxor(xmm0, src1, mask);
          COMP->vpxor(xmm1, src2, mask);
          COMP->vpcmpgtd(dst, xmm0, xmm1);
          break;
        }
        case HIR::FLOAT32_TYPE: {
          x86::Xmm mask = LoadXmmConst(b, XMMSignMaskF32);
          COMP->vpxor(xmm0, src1, mask);
          COMP->vpxor(xmm1, src2, mask);
          COMP->vcmpps(dst, xmm0, xmm1, Imm(0x0D)); // _CMP_GE_OS
          break;
        }
        default: UNREACHABLE_MSG("Unimplemented VECTOR_COMPARE_UGT type."); return;
        }
        // Store result
        TagStoreReg(instr->dest, dst);
      }

      //
      // Vector Compare Unsigned Greater Equal
      //

      REGISTER_EMITTER(OPCODE_VECTOR_COMPARE_UGE, Emit_VECTOR_COMPARE_UGE)
      static void Emit_VECTOR_COMPARE_UGE(x86CodeGenBackend *b, const HIR::Instr *instr) {
        // Destination
        x86::Xmm dst = allocXmm();
        // Get sources
        x86::Xmm src1 = LoadValueXmm(b, instr->src1.value);
        x86::Xmm src2 = LoadValueXmm(b, instr->src2.value);
      
        // Temps
        x86::Xmm xmm0 = allocXmm();
        x86::Xmm xmm1 = allocXmm();
        x86::Xmm xmm2 = allocXmm();

        // Get part type
        HIR::TypeName partType = static_cast<HIR::TypeName>(instr->flags);

        switch (partType) {
        case HIR::INT8_TYPE: {
          x86::Xmm mask = LoadXmmConst(b, XMMSignMaskI8);
          COMP->vpxor(xmm0, src1, mask);
          COMP->vpxor(xmm1, src2, mask);
          COMP->vpcmpeqb(xmm2, xmm0, xmm1);
          COMP->vpcmpgtb(dst, xmm0, xmm1);
          COMP->vpor(dst, dst, xmm2);
        } break;
        case HIR::INT16_TYPE: {
          x86::Xmm mask = LoadXmmConst(b, XMMSignMaskI16);
          COMP->vpxor(xmm0, src1, mask);
          COMP->vpxor(xmm1, src2, mask);
          COMP->vpcmpeqw(xmm2, xmm0, xmm1);
          COMP->vpcmpgtw(dst, xmm0, xmm1);
          COMP->vpor(dst, dst, xmm2);
        } break;
        case HIR::INT32_TYPE: {
          x86::Xmm mask = LoadXmmConst(b, XMMSignMaskI32);
          COMP->vpxor(xmm0, src1, mask);
          COMP->vpxor(xmm1, src2, mask);
          COMP->vpcmpeqd(xmm2, xmm0, xmm1);
          COMP->vpcmpgtd(dst, xmm0, xmm1);
          COMP->vpor(dst, dst, xmm2);
        } break;
        case HIR::FLOAT32_TYPE: {
          x86::Xmm mask = LoadXmmConst(b, XMMSignMaskF32);
          COMP->vpxor(xmm0, src1, mask);
          COMP->vpxor(xmm1, src2, mask);
          COMP->vcmpps(dst, src1, src2, Imm(0x0D)); // _CMP_GE_OS
        } break;
        default: UNREACHABLE_MSG("Unimplemented VECTOR_COMPARE_UGE type."); return;
        }
        // Store result
        TagStoreReg(instr->dest, dst);
      }

      //
      // Vector Add
      //

      REGISTER_EMITTER(OPCODE_VECTOR_ADD, Emit_VECTOR_ADD)
      static void Emit_VECTOR_ADD(x86CodeGenBackend *b, const HIR::Instr *instr) {
        // Destination
        x86::Xmm dst = allocXmm();
        // Get sources
        x86::Xmm src1 = LoadValueXmm(b, instr->src1.value);
        x86::Xmm src2 = LoadValueXmm(b, instr->src2.value);
        // Get part type
        HIR::TypeName partType = static_cast<HIR::TypeName>(instr->flags & 0xFF);
       // Get flags
        u32 flags = instr->flags >> 8;
        // Get unsignned and saturate flags
        bool isUnsigned = !!(flags & HIR::ARITHMETIC_UNSIGNED);
        bool saturate = !!(flags & HIR::ARITHMETIC_SATURATE);

        switch (partType) {
        case HIR::INT8_TYPE:
          if (saturate) {
            if (isUnsigned)
              COMP->vpaddusb(dst, src1, src2);
            else
              COMP->vpaddsb(dst, src1, src2);
          } else {
            COMP->vpaddb(dst, src1, src2);
          }
          break;
        case HIR::INT16_TYPE:
          if (saturate) {
            if (isUnsigned)
              COMP->vpaddusw(dst, src1, src2);
            else
              COMP->vpaddsw(dst, src1, src2);
          } else {
            COMP->vpaddw(dst, src1, src2);
          }
          break;
        case HIR::INT32_TYPE:
          if (saturate) {
            if (isUnsigned) {
              x86::Xmm xmm0 = allocXmm();
              x86::Xmm xmm1 = allocXmm();
              x86::Xmm xmm2 = allocXmm();

              // Unsigned saturate: add, then max with original (overflow wraps).
              COMP->vpaddd(xmm1, src1, src2);

              // If result is smaller than either of the inputs, we've
              // overflowed (only need to check one input)
              // if (src1 > res) then overflowed
              // http://locklessinc.com/articles/sat_arithmetic/

              COMP->vpxor(xmm2, src1, LoadXmmConst(b, XMMSignMaskI32));
              COMP->vpxor(xmm0, xmm1, LoadXmmConst(b, XMMSignMaskI32));
              COMP->vpcmpgtd(xmm0, xmm2, xmm0); 
              COMP->vpor(dst, xmm1, xmm0);
            } else {
              x86::Xmm xmm1 = allocXmm();
              x86::Xmm xmm2 = allocXmm();
              x86::Xmm xmm3 = allocXmm();

              COMP->vpaddd(xmm1, src1, src2);

              // Overflow results if two inputs are the same sign and the
              // result isn't the same sign. if ((s32b)(~(src1 ^ src2) &
              // (src1 ^ res)) < 0) then overflowed
              // http://locklessinc.com/articles/sat_arithmetic/
              COMP->vpxor(xmm2, src1, src2);
              COMP->vpxor(xmm3, src1, xmm1); 
              COMP->vpandn(xmm2, xmm2, xmm3);

              // Set any negative overflowed elements of src1 to INT_MIN
              COMP->vpand(xmm3, src1, xmm2);
              COMP->vblendvps(xmm1, xmm1, LoadXmmConst(b, XMMSignMaskI32), xmm3);
              // Set any positive overflowed elements of src1 to INT_MAX
              COMP->vpandn(xmm3, src1, xmm2);
              COMP->vblendvps(dst, xmm1, LoadXmmConst(b, XMMAbsMaskPS), xmm3);
            }
          } else {
            COMP->vpaddd(dst, src1, src2);
          }
          break;
        case HIR::FLOAT32_TYPE:
          if (isUnsigned || saturate) { UNREACHABLE_MSG("Invalid flags in VECTOR_ADD"); }
          COMP->vaddps(dst, src1, src2);
          break;
        default: UNREACHABLE_MSG("Unimplemented VECTOR_ADD type."); return;
        }
        // Store result
        TagStoreReg(instr->dest, dst);
      }

      //
      // Vector Subtract
      //

      REGISTER_EMITTER(OPCODE_VECTOR_SUB, Emit_VECTOR_SUB)
      static void Emit_VECTOR_SUB(x86CodeGenBackend *b, const HIR::Instr *instr) {
        // Destination
        x86::Xmm dst = allocXmm();
        // Get sources
        x86::Xmm src1 = LoadValueXmm(b, instr->src1.value);
        x86::Xmm src2 = LoadValueXmm(b, instr->src2.value);
        // Get part type
        HIR::TypeName partType = static_cast<HIR::TypeName>(instr->flags & 0xFF);
        // Get flags
        u32 flags = instr->flags >> 8;
        // Get unsignned and saturate flags
        bool isUnsigned = !!(flags & HIR::ARITHMETIC_UNSIGNED);
        bool saturate = !!(flags & HIR::ARITHMETIC_SATURATE);

        switch (partType) {
        case HIR::INT8_TYPE:
          if (saturate) {
            if (isUnsigned)
              COMP->vpsubusb(dst, src1, src2);
            else
              COMP->vpsubsb(dst, src1, src2);
          } else {
            COMP->vpsubb(dst, src1, src2);
          }
          break;
        case HIR::INT16_TYPE:
          if (saturate) {
            if (isUnsigned)
              COMP->vpsubusw(dst, src1, src2);
            else
              COMP->vpsubsw(dst, src1, src2);
          } else {
            COMP->vpsubw(dst, src1, src2);
          }
          break;
        case HIR::INT32_TYPE:
          if (saturate) {
            if (isUnsigned) {
              x86::Xmm xmm0 = allocXmm();
              x86::Xmm xmm1 = allocXmm();
              x86::Xmm xmm2 = allocXmm();

              // Unsigned saturate: add, then max with original (overflow wraps).
              COMP->vpsubd(xmm1, src1, src2);

              // If result is greater than either of the inputs, we've
              // underflowed (only need to check one input)
              // if (res > src1) then underflowed
              // http://locklessinc.com/articles/sat_arithmetic/

              COMP->vpxor(xmm2, src1, LoadXmmConst(b, XMMSignMaskI32));
              COMP->vpxor(xmm0, xmm1, LoadXmmConst(b, XMMSignMaskI32));
              COMP->vpcmpgtd(xmm0, xmm0, xmm2);
              COMP->vpandn(dst, xmm0, xmm1);
            } else {
              x86::Xmm xmm1 = allocXmm();
              x86::Xmm xmm2 = allocXmm();
              x86::Xmm xmm3 = allocXmm();

              COMP->vpsubd(xmm1, src1, src2);

              // We can only overflow if the signs of the operands are
              // opposite. If signs are opposite and result sign isn't the
              // same as src1's sign, we've overflowed. if ((s32b)((src1 ^
              // src2) & (src1 ^ res)) < 0) then overflowed
              // http://locklessinc.com/articles/sat_arithmetic/
              COMP->vpxor(xmm2, src1, src2);
              COMP->vpxor(xmm3, src1, xmm1);
              COMP->vpand(xmm2, xmm2, xmm3);

              // Set any negative overflowed elements of src1 to INT_MIN
              COMP->vpand(xmm3, src1, xmm2);
              COMP->vblendvps(xmm1, xmm1, LoadXmmConst(b, XMMSignMaskI32), xmm3);
              // Set any positive overflowed elements of src1 to INT_MAX
              COMP->vpandn(xmm3, src1, xmm2);
              COMP->vblendvps(dst, xmm1, LoadXmmConst(b, XMMAbsMaskPS), xmm3);
            }
          } else {
            COMP->vpsubd(dst, src1, src2);
          }
          break;
        case HIR::FLOAT32_TYPE:
          if (isUnsigned || saturate) { UNREACHABLE_MSG("Invalid flags in VECTOR_ADD"); }
          COMP->vsubps(dst, src1, src2);
          break;
        default: UNREACHABLE_MSG("Unimplemented VECTOR_ADD type."); return;
        }
        // Store result
        TagStoreReg(instr->dest, dst);
      }

      // ========================================================================
      // OPCODE_VECTOR_SHL
      // ========================================================================
      REGISTER_EMITTER(OPCODE_VECTOR_SHL, Emit_VECTOR_SHL)
      static void Emit_VECTOR_SHL(x86CodeGenBackend *b, const HIR::Instr *instr) {
        // Destination
        x86::Xmm dst = allocXmm();
        // Get sources
        x86::Xmm src1 = LoadValueXmm(b, instr->src1.value);
        x86::Xmm src2 = LoadValueXmm(b, instr->src2.value);
        // Get part type
        HIR::TypeName partType = static_cast<HIR::TypeName>(instr->flags);
        
        using ShiftFn = __m128i(*)(void *, __m128i, __m128i);

        // Build the signature manually: __m128i(void*, __m128i, __m128i)
        // asmjit has no TypeIdOfT for __m128i, so use explicit TypeId values.
        asmjit::FuncSignature sig(
          asmjit::CallConvId::kCDecl,
          asmjit::FuncSignature::kNoVarArgs,
          asmjit::TypeId::kInt32x4,   // return: __m128i
          asmjit::TypeId::kUIntPtr,   // arg0:   void*
          asmjit::TypeId::kInt32x4,   // arg1:   __m128i
          asmjit::TypeId::kInt32x4);  // arg2:   __m128i

        switch (partType) {
        case HIR::INT8_TYPE: {
          ShiftFn fn = static_cast<ShiftFn>(EmulateVectorShl<uint8_t>);
          InvokeNode *invokeNode = nullptr;
          COMP->invoke(&invokeNode, asmjit::Imm(reinterpret_cast<uintptr_t>(fn)), sig);
          invokeNode->setArg(0, Imm(0)); // void* ctx = nullptr
          invokeNode->setArg(1, src1);
          invokeNode->setArg(2, src2);
          invokeNode->setRet(0, dst);
        } break;
        case HIR::INT16_TYPE: {
          ShiftFn fn = static_cast<ShiftFn>(EmulateVectorShl<uint16_t>);
          InvokeNode *invokeNode = nullptr;
          COMP->invoke(&invokeNode, asmjit::Imm(reinterpret_cast<uintptr_t>(fn)), sig);
          invokeNode->setArg(0, Imm(0)); // void* ctx = nullptr
          invokeNode->setArg(1, src1);
          invokeNode->setArg(2, src2);
          invokeNode->setRet(0, dst);
        } break;
        case HIR::INT32_TYPE: {
          x86::Xmm xmm0 = allocXmm();
          COMP->vandps(xmm0, src2, LoadXmmConst(b, XMMShiftMaskPS));
          COMP->vpsllvd(dst, src1, xmm0);
        } break;
        default: UNREACHABLE_MSG("Unimplemented VECTOR_SHL type."); return;
        }
        // Store result
        TagStoreReg(instr->dest, dst);
      }

      // ========================================================================
      // OPCODE_VECTOR_SHR
      // ========================================================================
      REGISTER_EMITTER(OPCODE_VECTOR_SHR, Emit_VECTOR_SHR)
      static void Emit_VECTOR_SHR(x86CodeGenBackend *b, const HIR::Instr *instr) {
        // Destination
        x86::Xmm dst = allocXmm();
        // Get sources
        x86::Xmm src1 = LoadValueXmm(b, instr->src1.value);
        x86::Xmm src2 = LoadValueXmm(b, instr->src2.value);
        // Get part type
        HIR::TypeName partType = static_cast<HIR::TypeName>(instr->flags);

        using ShiftFn = __m128i(*)(void *, __m128i, __m128i);

        // Build the signature manually: __m128i(void*, __m128i, __m128i)
        // asmjit has no TypeIdOfT for __m128i, so use explicit TypeId values.
        asmjit::FuncSignature sig(
          asmjit::CallConvId::kCDecl,
          asmjit::FuncSignature::kNoVarArgs,
          asmjit::TypeId::kInt32x4,   // return: __m128i
          asmjit::TypeId::kUIntPtr,   // arg0:   void*
          asmjit::TypeId::kInt32x4,   // arg1:   __m128i
          asmjit::TypeId::kInt32x4);  // arg2:   __m128i

        switch (partType) {
        case HIR::INT8_TYPE: {
          ShiftFn fn = static_cast<ShiftFn>(EmulateVectorShr<uint8_t>);
          InvokeNode *invokeNode = nullptr;
          COMP->invoke(&invokeNode, asmjit::Imm(reinterpret_cast<uintptr_t>(fn)), sig);
          invokeNode->setArg(0, Imm(0)); // void* ctx = nullptr
          invokeNode->setArg(1, src1);
          invokeNode->setArg(2, src2);
          invokeNode->setRet(0, dst);
        } break;
        case HIR::INT16_TYPE: {
          ShiftFn fn = static_cast<ShiftFn>(EmulateVectorShr<uint16_t>);
          InvokeNode *invokeNode = nullptr;
          COMP->invoke(&invokeNode, asmjit::Imm(reinterpret_cast<uintptr_t>(fn)), sig);
          invokeNode->setArg(0, Imm(0)); // void* ctx = nullptr
          invokeNode->setArg(1, src1);
          invokeNode->setArg(2, src2);
          invokeNode->setRet(0, dst);
        } break;
        case HIR::INT32_TYPE: {
          x86::Xmm xmm0 = allocXmm();
          COMP->vandps(xmm0, src2, LoadXmmConst(b, XMMShiftMaskPS));
          COMP->vpsrlvd(dst, src1, xmm0);
        } break;
        default: UNREACHABLE_MSG("Unimplemented VECTOR_SHR type."); return;
        }
        // Store result
        TagStoreReg(instr->dest, dst);
      }

      // 
      // Vector Shift Arithmetic
      // 
      
      REGISTER_EMITTER(OPCODE_VECTOR_SHA, Emit_VECTOR_SHA)
      static void Emit_VECTOR_SHA(x86CodeGenBackend *b, const HIR::Instr *instr) {
        // Destination
        x86::Xmm dst = allocXmm();
        // Get sources
        x86::Xmm src1 = LoadValueXmm(b, instr->src1.value);
        x86::Xmm src2 = LoadValueXmm(b, instr->src2.value);
        // Get part type
        HIR::TypeName partType = static_cast<HIR::TypeName>(instr->flags);

        using ShiftFn = __m128i(*)(void *, __m128i, __m128i);

        // Build the signature manually: __m128i(void*, __m128i, __m128i)
        // asmjit has no TypeIdOfT for __m128i, so use explicit TypeId values.
        asmjit::FuncSignature sig(
          asmjit::CallConvId::kCDecl,
          asmjit::FuncSignature::kNoVarArgs,
          asmjit::TypeId::kInt32x4,   // return: __m128i
          asmjit::TypeId::kUIntPtr,   // arg0:   void*
          asmjit::TypeId::kInt32x4,   // arg1:   __m128i
          asmjit::TypeId::kInt32x4);  // arg2:   __m128i

        switch (partType) {
        case HIR::INT8_TYPE: {
          ShiftFn fn = static_cast<ShiftFn>(EmulateVectorShr<int8_t>);
          InvokeNode *invokeNode = nullptr;
          COMP->invoke(&invokeNode, asmjit::Imm(reinterpret_cast<uintptr_t>(fn)), sig);
          invokeNode->setArg(0, Imm(0)); // void* ctx = nullptr
          invokeNode->setArg(1, src1);
          invokeNode->setArg(2, src2);
          invokeNode->setRet(0, dst);
        } break;
        case HIR::INT16_TYPE: {
          ShiftFn fn = static_cast<ShiftFn>(EmulateVectorShr<int16_t>);
          InvokeNode *invokeNode = nullptr;
          COMP->invoke(&invokeNode, asmjit::Imm(reinterpret_cast<uintptr_t>(fn)), sig);
          invokeNode->setArg(0, Imm(0)); // void* ctx = nullptr
          invokeNode->setArg(1, src1);
          invokeNode->setArg(2, src2);
          invokeNode->setRet(0, dst);
        } break;
        case HIR::INT32_TYPE: {
          x86::Xmm xmm0 = allocXmm();
          COMP->vandps(xmm0, src2, LoadXmmConst(b, XMMShiftMaskPS));
          COMP->vpsravd(dst, src1, xmm0);
        } break;
        default: UNREACHABLE_MSG("Unimplemented VECTOR_SHA type."); return;
        }
        // Store result
        TagStoreReg(instr->dest, dst);
      }

      //
      // Vector Rotate Left
      //

      REGISTER_EMITTER(OPCODE_VECTOR_ROTATE_LEFT, Emit_VECTOR_ROTATE_LEFT)
      static void Emit_VECTOR_ROTATE_LEFT(x86CodeGenBackend *b, const HIR::Instr *instr) {
        // Destination
        x86::Xmm dst = allocXmm();
        // Get sources
        x86::Xmm src1 = LoadValueXmm(b, instr->src1.value);
        x86::Xmm src2 = LoadValueXmm(b, instr->src2.value);
        // Get part type
        HIR::TypeName partType = static_cast<HIR::TypeName>(instr->flags);

        using ShiftFn = __m128i(*)(void *, __m128i, __m128i);

        // Build the signature manually: __m128i(void*, __m128i, __m128i)
        // asmjit has no TypeIdOfT for __m128i, so use explicit TypeId values.
        asmjit::FuncSignature sig(
          asmjit::CallConvId::kCDecl,
          asmjit::FuncSignature::kNoVarArgs,
          asmjit::TypeId::kInt32x4,   // return: __m128i
          asmjit::TypeId::kUIntPtr,   // arg0:   void*
          asmjit::TypeId::kInt32x4,   // arg1:   __m128i
          asmjit::TypeId::kInt32x4);  // arg2:   __m128i

        switch (partType) {
        case HIR::INT8_TYPE: {
          ShiftFn fn = static_cast<ShiftFn>(EmulateVectorRotateLeft<uint8_t>);
          InvokeNode *invokeNode = nullptr;
          COMP->invoke(&invokeNode, asmjit::Imm(reinterpret_cast<uintptr_t>(fn)), sig);
          invokeNode->setArg(0, Imm(0)); // void* ctx = nullptr
          invokeNode->setArg(1, src1);
          invokeNode->setArg(2, src2);
          invokeNode->setRet(0, dst);
        } break;
        case HIR::INT16_TYPE: {
          ShiftFn fn = static_cast<ShiftFn>(EmulateVectorRotateLeft<uint16_t>);
          InvokeNode *invokeNode = nullptr;
          COMP->invoke(&invokeNode, asmjit::Imm(reinterpret_cast<uintptr_t>(fn)), sig);
          invokeNode->setArg(0, Imm(0)); // void* ctx = nullptr
          invokeNode->setArg(1, src1);
          invokeNode->setArg(2, src2);
          invokeNode->setRet(0, dst);
        } break;
        case HIR::INT32_TYPE: {
          x86::Xmm xmm0 = allocXmm();
          x86::Xmm xmm1 = allocXmm();
          x86::Xmm tmp = allocXmm();
          // Shift left (to get high bits):
          COMP->vpand(xmm0, src2, LoadXmmConst(b, XMMShiftMaskPS));
          COMP->vpsllvd(xmm1, src1, xmm0);
          // Shift right (to get low bits):
          COMP->vmovaps(tmp, LoadXmmConst(b, XMMPI32));
          COMP->vpsubd(tmp, tmp, xmm0);
          COMP->vpsrlvd(dst, src1, tmp);
          // Merge:
          COMP->vpor(dst, dst, xmm1);
        } break;
        default: UNREACHABLE_MSG("Unimplemented VECTOR_ROTATE_LEFT type."); return;
        }
        // Store result
        TagStoreReg(instr->dest, dst);
      }

      //
      // Vector Average
      //

      REGISTER_EMITTER(OPCODE_VECTOR_AVERAGE, Emit_VECTOR_AVERAGE)
      static void Emit_VECTOR_AVERAGE(x86CodeGenBackend *b, const HIR::Instr *instr) {
        // Destination
        x86::Xmm dst = allocXmm();
        // Get sources
        x86::Xmm src1 = LoadValueXmm(b, instr->src1.value);
        x86::Xmm src2 = LoadValueXmm(b, instr->src2.value);
        // Get part type
        HIR::TypeName partType = static_cast<HIR::TypeName>(instr->flags & 0xFF);
        // Get flags
        u8 flags = instr->flags >> 8;
        // Unsigned?
        bool isUnsigned = !!(flags & HIR::ARITHMETIC_UNSIGNED);

        switch (partType) {
        case HIR::INT8_TYPE:
          if (isUnsigned) {
            COMP->vpavgb(dst, src1, src2);
          } else {
            UNREACHABLE_MSG("VECTOR_AVERAGE: Signed INT8 not supported.");
          }
          break;
        case HIR::INT16_TYPE:
          if (isUnsigned) {
            COMP->vpavgw(dst, src1, src2);
          } else {
            UNREACHABLE_MSG("VECTOR_AVERAGE: Signed INT16 not supported.");
          }
          break;
        case HIR::INT32_TYPE: {
          // Sadly there's no 32 bit averages in AVX/2.
          // Fall back to scalar emulation via EmulateVectorAverage.
          using AvgFn = __m128i(*)(void *, __m128i, __m128i);
          AvgFn fn = isUnsigned
            ? static_cast<AvgFn>(EmulateVectorAverage<uint32_t>)
            : static_cast<AvgFn>(EmulateVectorAverage<int32_t>);

          // Build the signature manually: __m128i(void*, __m128i, __m128i)
          // asmjit has no TypeIdOfT for __m128i, so use explicit TypeId values.
          asmjit::FuncSignature sig(
            asmjit::CallConvId::kCDecl,
            asmjit::FuncSignature::kNoVarArgs,
            asmjit::TypeId::kInt32x4,   // return: __m128i
            asmjit::TypeId::kUIntPtr,   // arg0:   void*
            asmjit::TypeId::kInt32x4,   // arg1:   __m128i
            asmjit::TypeId::kInt32x4);  // arg2:   __m128i

          asmjit::InvokeNode *invokeNode = nullptr;
          COMP->invoke(&invokeNode, asmjit::Imm(reinterpret_cast<uintptr_t>(fn)), sig);
          invokeNode->setArg(0, asmjit::Imm(0)); // void* ctx = nullptr
          invokeNode->setArg(1, src1);
          invokeNode->setArg(2, src2);
          invokeNode->setRet(0, dst);
          break;
        }
        default: UNREACHABLE_MSG("Unimplemented VECTOR_AVERAGE type."); return;
        }
        // Store result
        TagStoreReg(instr->dest, dst);
      }

      
      // 
      // Insert
      // 

      // Stuck here!
      
      REGISTER_EMITTER(OPCODE_INSERT, Emit_INSERT)
      static void Emit_INSERT(x86CodeGenBackend *b, const HIR::Instr *instr) {
        x86::Xmm src1 = LoadValueXmm(b, instr->src1.value);
        x86::Xmm dst = allocXmm();
        COMP->vmovaps(dst, src1);

        u32 index = static_cast<u32>(instr->src3.offset);
        HIR::TypeName partType = static_cast<HIR::TypeName>(instr->flags);

        switch (partType) {
        case HIR::INT8_TYPE: {
          x86::Gp val = LoadValueGp(b, instr->src2.value);
          // XOR index with 3 for big-endian byte order within dword.
          COMP->vpinsrb(dst, dst, val.r32(), asmjit::Imm(index ^ 0x3));
          break;
        }
        case HIR::INT16_TYPE: {
          x86::Gp val = LoadValueGp(b, instr->src2.value);
          COMP->vpinsrw(dst, dst, val.r32(), asmjit::Imm(index ^ 0x1));
          break;
        }
        case HIR::INT32_TYPE: {
          x86::Gp val = LoadValueGp(b, instr->src2.value);
          COMP->vpinsrd(dst, dst, val.r32(), asmjit::Imm(index));
          break;
        }
        default: UNREACHABLE_MSG("Unimplemented INSERT type."); return;
        }
        // Store result
        TagStoreReg(instr->dest, dst);
      }

      // ========================================================================
      // OPCODE_EXTRACT
      // ========================================================================

      REGISTER_EMITTER(OPCODE_EXTRACT, Emit_EXTRACT)
        static void Emit_EXTRACT(x86CodeGenBackend *b, const HIR::Instr *instr) {
        x86::Xmm src1 = LoadValueXmm(b, instr->src1.value);

        HIR::TypeName partType = static_cast<HIR::TypeName>(instr->dest->type);

        switch (partType) {
        case HIR::INT8_TYPE: {
          x86::Gp dst = newGP32();
          if (instr->src2.value->IsConstant()) {
            // Constant path: direct vpextrb with big-endian byte XOR.
            COMP->vpextrb(dst, src1, asmjit::Imm(instr->src2.value->constant.u8 ^ 0x3));
            COMP->and_(dst, asmjit::Imm(0xFF));
          } else {
            // Non-constant path (Xenia-style):
            // Build a vpshufb selector that routes byte (idx ^ 3) into lane 0.
            x86::Gp index = LoadValueGp(b, instr->src2.value);
            x86::Gp eax = newGP32();
            // eax = 0x00000003 ^ (index & 0x1F), gives the rotated byte position.
            COMP->mov(eax, asmjit::Imm(0x00000003));
            COMP->xor_(eax.r8(), index.r8());
            COMP->and_(eax.r8(), asmjit::Imm(0x1F));
            // Broadcast selector into xmm0, then vpshufb routes src1[eax] -> lane 0.
            x86::Xmm shuf = allocXmm();
            COMP->vmovd(shuf, eax);
            COMP->vpshufb(shuf, src1, shuf);
            COMP->vmovd(dst, shuf);
            COMP->and_(dst, asmjit::Imm(0xFF));
          }
          TagStoreReg(instr->dest, dst);
          break;
        }
        case HIR::INT16_TYPE: {
          x86::Gp dst = newGP32();
          if (instr->src2.value->IsConstant()) {
            // Constant path: direct vpextrw with big-endian word XOR.
            COMP->vpextrw(dst, src1, asmjit::Imm(instr->src2.value->constant.u8 ^ 0x1));
            COMP->and_(dst, asmjit::Imm(0xFFFF));
          } else {
            // Non-constant path (Xenia-style):
            // Build a 2-byte vpshufb selector in eax[7:0] (lo byte) and eax[15:8] (hi byte).
            x86::Gp index = LoadValueGp(b, instr->src2.value);
            x86::Gp eax = newGP32();
            // al = (index ^ 0x01) << 1  => byte offset of the word's low byte.
            COMP->mov(eax.r8(), index.r8());
            COMP->xor_(eax.r8(), asmjit::Imm(0x01));
            COMP->shl(eax.r8(), asmjit::Imm(1));
            // ah = al + 1  => byte offset of the word's high byte.
            x86::Gp ah = newGP8();
            COMP->mov(ah, eax.r8());
            COMP->add(ah, asmjit::Imm(1));
            // Pack al/ah into eax low word: eax = (ah << 8) | al.
            COMP->movzx(eax, eax.r8());
            x86::Gp ahExt = newGP32();
            COMP->movzx(ahExt, ah);
            COMP->shl(ahExt, asmjit::Imm(8));
            COMP->or_(eax, ahExt);
            // vpshufb: routes src1[al] -> byte 0, src1[ah] -> byte 1 of lane 0.
            x86::Xmm shuf = allocXmm();
            COMP->vmovd(shuf, eax);
            COMP->vpshufb(shuf, src1, shuf);
            COMP->vmovd(eax, shuf);
            COMP->and_(eax, asmjit::Imm(0xFFFF));
            COMP->mov(dst, eax);
          }
          TagStoreReg(instr->dest, dst);
          break;
        }
        case HIR::INT32_TYPE: {
          x86::Gp dst = newGP32();
          if (instr->src2.value->IsConstant()) {
            // Constant path: direct vpextrd (no XOR needed for dwords).
            COMP->vpextrd(dst, src1, asmjit::Imm(instr->src2.value->constant.u8));
          } else {
            // Non-constant path (Xenia-style):
            // Build a 4-byte vpshufb selector routing dword[index] into lane 0.
            x86::Gp index = LoadValueGp(b, instr->src2.value);
            x86::Gp eax = newGP32();
            // base = index << 2 => byte offset of dword start.
            COMP->movzx(eax, index.r8());
            COMP->shl(eax, asmjit::Imm(2));
            // Build selector word: bytes 0..3 = base, base+1, base+2, base+3.
            x86::Gp tmp = newGP32();
            COMP->mov(tmp, eax);
            COMP->add(tmp, asmjit::Imm(1));
            COMP->shl(tmp, asmjit::Imm(8));
            COMP->or_(eax, tmp);
            COMP->mov(tmp, eax);
            // eax now has [base+1, base] in low 16 bits; build full 32-bit selector.
            x86::Gp hi = newGP32();
            COMP->movzx(hi, index.r8());
            COMP->shl(hi, asmjit::Imm(2));
            COMP->add(hi, asmjit::Imm(2));
            x86::Gp hi2 = newGP32();
            COMP->mov(hi2, hi);
            COMP->add(hi2, asmjit::Imm(1));
            COMP->shl(hi2, asmjit::Imm(8));
            COMP->or_(hi, hi2);
            COMP->shl(hi, asmjit::Imm(16));
            COMP->or_(eax, hi);
            // vpshufb routes dword bytes into lane 0.
            x86::Xmm shuf = allocXmm();
            COMP->vmovd(shuf, eax);
            COMP->vpshufb(shuf, src1, shuf);
            COMP->vmovd(dst, shuf);
          }
          TagStoreReg(instr->dest, dst);
          break;
        }
        default: UNREACHABLE_MSG("Unimplemented EXTRACT type."); return;
        }
      }

      // ========================================================================
      // OPCODE_SPLAT
      // ========================================================================
      REGISTER_EMITTER(OPCODE_SPLAT, Emit_SPLAT)
      static void Emit_SPLAT(x86CodeGenBackend *b, const HIR::Instr *instr) {
        x86::Xmm dst = allocXmm();

        switch (instr->src1.value->type) {
        case HIR::INT8_TYPE: {
          x86::Gp val = LoadValueGp(b, instr->src1.value);
          x86::Gp val32 = newGP32();
          COMP->movzx(val32, val.r8());
          COMP->vmovd(dst, val32);
          COMP->vpbroadcastb(dst, dst);
          break;
        }
        case HIR::INT16_TYPE: {
          x86::Gp val = LoadValueGp(b, instr->src1.value);
          x86::Gp val32 = newGP32();
          COMP->movzx(val32, val.r16());
          COMP->vmovd(dst, val32);
          COMP->vpbroadcastw(dst, dst);
          break;
        }
        case HIR::INT32_TYPE: {
          x86::Gp val = LoadValueGp(b, instr->src1.value);
          COMP->vmovd(dst, val.r32());
          COMP->vpbroadcastd(dst, dst);
          break;
        }
        case HIR::FLOAT32_TYPE: {
          x86::Xmm val = LoadValueXmm(b, instr->src1.value);
          COMP->vbroadcastss(dst, val);
          break;
        }
        default: UNREACHABLE_MSG("Unimplemented SPLAT type."); return;
        }
        TagStoreReg(instr->dest, dst);
      }

      // Permute across two source vectors using a control mask.
      // partType (in instr->flags) determines the granularity:
      //   INT8_TYPE:  byte-level (vperm). Control = VEC128 with byte indices 0..31.
      //   INT16_TYPE: halfword-level. Control = VEC128 with halfword indices 0..15.
      //   INT32_TYPE: dword-level. Control = constant u32 from MakePermuteMask().
      REGISTER_EMITTER(OPCODE_PERMUTE, Emit_PERMUTE)
        static void Emit_PERMUTE(x86CodeGenBackend *b, const HIR::Instr *instr) {

        // Switch Dest Type
        switch (instr->dest->type) {
        case HIR::VEC128_TYPE: {

          // Get Part Type
          HIR::TypeName partType = static_cast<HIR::TypeName>(instr->flags);

          // Switch Part Type
          switch (partType) {
          case HIR::INT32_TYPE: {
            if (instr->src1.value->IsConstant()) {
              u32 control = instr->src1.value->constant.u32;
              // Shuffle things into the right places in dest & tmp,
              // then we blend them together.
              u32 src_control = (((control >> 24) & 0x3) << 6) | (((control >> 16) & 0x3) << 4) |
                (((control >> 8) & 0x3) << 2) | (((control >> 0) & 0x3) << 0);

              u32 blend_control =
                (((control >> 26) & 0x1) << 3) | (((control >> 18) & 0x1) << 2) |
                (((control >> 10) & 0x1) << 1) | (((control >> 2) & 0x1) << 0);

              x86::Xmm src2 = LoadValueXmm(b, instr->src2.value);
              x86::Xmm src3 = LoadValueXmm(b, instr->src3.value);
              x86::Xmm dst = allocXmm();
              x86::Xmm tmp = allocXmm();

              COMP->vpshufd(dst, src2, asmjit::Imm(src_control));
              COMP->vpshufd(tmp, src3, asmjit::Imm(src_control));
              COMP->vpblendd(dst, dst, tmp, asmjit::Imm(blend_control));
              TagStoreReg(instr->dest, dst);
            } else {
              // Permute by non-constant.
              UNREACHABLE_MSG("PERMUTE Part INT32 by non constant");
            }

            break;
          }
          case HIR::INT16_TYPE: {
            // Destination
            x86::Xmm dst = allocXmm();
            // Get sources
            x86::Xmm src1 = LoadValueXmm(b, instr->src1.value);
            x86::Xmm src2 = LoadValueXmm(b, instr->src2.value);
            x86::Xmm src3 = LoadValueXmm(b, instr->src3.value);
            // Temps
            x86::Xmm xmm0 = allocXmm();
            x86::Xmm xmm1 = allocXmm();
            x86::Xmm xmm2 = allocXmm();

            Base::Vector128 perm = (instr->src1.value->constant.v128 & Base::Vector128s(0xF)) ^ Base::Vector128s(0x1);
            Base::Vector128 perm_ctrl = Base::Vector128b(0);
            for (int i = 0; i < 8; i++) {
              perm_ctrl.sword[i] = perm.sword[i] > 7 ? -1 : 0;

              auto v = u8(perm.word[i]);
              perm.bytes[i * 2] = v * 2;
              perm.bytes[i * 2 + 1] = v * 2 + 1;
            }


            // Store into the HIR value's constant storage so the pointer is stable.
            // TODO: Correctly implement dynamic constant system.
            instr->src1.value->constant.v128 = perm;

            COMP->vmovdqu(xmm0, LoadXmmConst(b, instr->src1.value->constant.v128));

            COMP->vmovdqa(xmm1, src2);
            COMP->vmovdqa(xmm2, src3);
            COMP->vpshufb(xmm1, xmm1, xmm0);
            COMP->vpshufb(xmm2, xmm2, xmm0);

            u8 mask = 0;
            for (int i = 0; i < 8; i++) {
              if (perm_ctrl.sword[i] == 0) {
                mask |= 1 << (7 - i);
              }
            }

            COMP->vpblendw(dst, xmm1, xmm2, Imm(mask));
            // Store result
            TagStoreReg(instr->dest, dst);
            break;
          }
          case HIR::INT8_TYPE: {
            // General permute.
            // Control mask needs to be shuffled.

            // Destination
            x86::Xmm dst = allocXmm();
            // Get sources
            x86::Xmm src1 = LoadValueXmm(b, instr->src1.value);
            x86::Xmm src2 = LoadValueXmm(b, instr->src2.value);
            x86::Xmm src3 = LoadValueXmm(b, instr->src3.value);
            // Temps
            x86::Xmm xmm2 = allocXmm();
            x86::Xmm src2Shuffled = allocXmm();
            x86::Xmm src3Shuffled = allocXmm();

            COMP->vxorps(xmm2, src1, LoadXmmConst(b, XMMSwapWordMask));
            COMP->vpand(xmm2, xmm2, LoadXmmConst(b, XMMPermuteByteMask));
            
            COMP->vpshufb(src2Shuffled, src2, xmm2);
            COMP->vpshufb(src3Shuffled, src3, xmm2);
            COMP->vpcmpgtb(dst, xmm2, LoadXmmConst(b, XMMPermuteControl15));
            COMP->vpblendvb(dst, src2Shuffled, src3Shuffled, dst);
            // Store result
            TagStoreReg(instr->dest, dst);
          }
            break;
          default: UNREACHABLE_MSG("Unimplemented PERMUTE part type."); return;
          }
          break;
        }
        default: UNREACHABLE_MSG("Unimplemented PERMUTE type."); return;
        }
      }

      // ========================================================================
      // OPCODE_SWIZZLE
      // ========================================================================
      REGISTER_EMITTER(OPCODE_SWIZZLE, Emit_SWIZZLE)
      static void Emit_SWIZZLE(x86CodeGenBackend *b, const HIR::Instr *instr) {        
        u32 elementType = instr->flags;

        if (elementType == HIR::INT32_TYPE || elementType == HIR::FLOAT32_TYPE) {
          x86::Xmm src = LoadValueXmm(b, instr->src1.value);
          u8 swizzleMask = static_cast<u8>(instr->src2.offset);
          x86::Xmm dst = allocXmm();
          COMP->vpshufd(dst, src, swizzleMask);
          TagStoreReg(instr->dest, dst);
        } else {
          UNREACHABLE_MSG("OPCODE_SWIZZLE currently only supports 32-bit element types (DWORD/FLOAT)");
        }
      }

      // ========================================================================
      // OPCODE_DOT_PRODUCT_3
      // ========================================================================
      REGISTER_EMITTER(OPCODE_DOT_PRODUCT_3, Emit_DOT_PRODUCT_3)
      static void Emit_DOT_PRODUCT_3(x86CodeGenBackend *b, const HIR::Instr *instr) {
        x86::Xmm src1 = LoadValueXmm(b, instr->src1.value);
        x86::Xmm src2 = LoadValueXmm(b, instr->src2.value);
        x86::Xmm dst = allocXmm();
        // dpps with mask 0x71: multiply X,Y,Z (bits 4,5,6), store to X (bit 0).
        // But PPC wants result in all lanes, so use 0x7F (store to all).
        COMP->vdpps(dst, src1, src2, asmjit::Imm(0x7F));
        TagStoreReg(instr->dest, dst);
      }

      // ========================================================================
      // OPCODE_DOT_PRODUCT_4
      // ========================================================================
      REGISTER_EMITTER(OPCODE_DOT_PRODUCT_4, Emit_DOT_PRODUCT_4)
      static void Emit_DOT_PRODUCT_4(x86CodeGenBackend *b, const HIR::Instr *instr) {
        x86::Xmm src1 = LoadValueXmm(b, instr->src1.value);
        x86::Xmm src2 = LoadValueXmm(b, instr->src2.value);
        x86::Xmm dst = allocXmm();
        // dpps with mask 0xFF: multiply all 4 lanes, store to all.
        COMP->vdpps(dst, src1, src2, asmjit::Imm(0xFF));
        TagStoreReg(instr->dest, dst);
      }

      // 
      // Vector Pack
      // 

      REGISTER_EMITTER(OPCODE_PACK, Emit_PACK)
      static void Emit_PACK(x86CodeGenBackend *b, const HIR::Instr *instr) {
        // Destination
        x86::Xmm dst = allocXmm();
        // Get sources
        x86::Xmm src1 = LoadValueXmm(b, instr->src1.value);
        x86::Xmm src2 = LoadValueXmm(b, instr->src2.value);
        // Get flags
        u32 flags = instr->flags;
        // Pack mode
        u32 mode = flags & HIR::PACK_TYPE_MODE;

        // Build the signature manually: __m128i(void*, __m128i, __m128i)
        // asmjit has no TypeIdOfT for __m128i, so use explicit TypeId values.
        asmjit::FuncSignature sig(
          asmjit::CallConvId::kCDecl,
          asmjit::FuncSignature::kNoVarArgs,
          asmjit::TypeId::kInt32x4,   // return: __m128i
          asmjit::TypeId::kUIntPtr,   // arg0:   void*
          asmjit::TypeId::kInt32x4,   // arg1:   __m128i
          asmjit::TypeId::kInt32x4);  // arg2:   __m128i

        switch (mode) {
        case HIR::PACK_TYPE_D3DCOLOR: {
          // Saturate to [3,3....] so that only values between 3...[00] and 3...[FF]
          // are valid - max before min to pack NaN as zero (5454082B is heavily
          // affected by the order - packs 0xFFFFFFFF in matrix code to get a 0
          // constant).
          COMP->vmaxps(dst, src1, LoadXmmConst(b, XMM3333));
          COMP->vminps(dst, dst, LoadXmmConst(b, XMMPackD3DCOLORSat));
          // Extract bytes.
          // RGBA (XYZW) -> ARGB (WXYZ)
          // w = ((src1.uw & 0xFF) << 24) | ((src1.ux & 0xFF) << 16) |
          //     ((src1.uy & 0xFF) << 8) | (src1.uz & 0xFF)
          COMP->vpshufb(dst, dst, LoadXmmConst(b, XMMPackD3DCOLOR));    
        } break;
        case HIR::PACK_TYPE_FLOAT16_2: {
          // 0|0|0|0|W|Z|Y|X
          COMP->vcvtps2ph(dst, src1, 0b00000011);
          // Shuffle to X|Y|0|0|0|0|0|0
          COMP->vpshufb(dst, dst, LoadXmmConst(b, XMMPackFLOAT16_2));
        } break;
        case HIR::PACK_TYPE_FLOAT16_4: {
          // 0|0|0|0|W|Z|Y|X
          COMP->vcvtps2ph(dst, src1, 0b00000011);
          // Shuffle to Z|W|X|Y|0|0|0|0
          COMP->vpshufb(dst, dst, LoadXmmConst(b, XMMPackFLOAT16_4));
        } break;
        case HIR::PACK_TYPE_SHORT_2: {
          // Saturate
          COMP->vmaxps(dst, src1, LoadXmmConst(b, XMMPackSHORT_Min));
          COMP->vminps(dst, dst, LoadXmmConst(b, XMMPackSHORT_Max));
          COMP->vcvttps2dq(dst, dst);
          // Pack
          COMP->vpshufb(dst, dst, LoadXmmConst(b, XMMPackSHORT_2));
        } break;
        case HIR::PACK_TYPE_SHORT_4: {
          // Saturate
          COMP->vmaxps(dst, src1, LoadXmmConst(b, XMMPackSHORT_Min));
          COMP->vminps(dst, dst, LoadXmmConst(b, XMMPackSHORT_Max));
          // Pack
          COMP->vpshufb(dst, dst, LoadXmmConst(b, XMMPackSHORT_4));
        } break;
        case HIR::PACK_TYPE_UINT_2101010: {
          // Temps
          x86::Xmm xmm0 = allocXmm();
          // Saturate.
          COMP->vmaxps(dst, src1, LoadXmmConst(b, XMMPackUINT_2101010_MinUnpacked));
          COMP->vminps(dst, dst, LoadXmmConst(b, XMMPackUINT_2101010_MaxUnpacked));
          // Remove the unneeded bits of the floats.
          COMP->vpand(dst, dst, LoadXmmConst(b, XMMPackUINT_2101010_MaskUnpacked));
          // Shift the components up.
          COMP->vpsllvd(dst, dst, LoadXmmConst(b, XMMPackUINT_2101010_Shift));
          // Combine the components.
          COMP->vshufps(xmm0, dst, dst, _MM_SHUFFLE(2, 3, 0, 1));
          COMP->vorps(dst,dst, xmm0);
          COMP->vshufps(xmm0, dst, dst, _MM_SHUFFLE(1, 0, 3, 2));
          COMP->vorps(dst, dst, xmm0);
        } break;
        case HIR::PACK_TYPE_ULONG_4202020: {
          // Temps
          x86::Xmm xmm0 = allocXmm();
          // Saturate.
          COMP->vmaxps(dst, src1, LoadXmmConst(b, XMMPackULONG_4202020_MinUnpacked));
          COMP->vminps(dst, dst, LoadXmmConst(b, XMMPackULONG_4202020_MaxUnpacked));
          // Remove the unneeded bits of the floats.
          COMP->vpand(dst, dst, LoadXmmConst(b, XMMPackULONG_4202020_MaskUnpacked));
          // Store Y and W shifted left by 4 so vpshufb can be used with them.
          COMP->vpslld(xmm0, dst, 4);
          // Place XZ where they're supposed to be.
          COMP->vpshufb(dst, dst, LoadXmmConst(b, XMMPackULONG_4202020_PermuteXZ));
          // Place YW.
          COMP->vpshufb(xmm0, xmm0, LoadXmmConst(b, XMMPackULONG_4202020_PermuteYW));
          // Merge
          COMP->vorps(dst, dst, xmm0);
        } break;
        case HIR::PACK_TYPE_8_IN_16: {
          if (HIR::IsPackInUnsigned(flags)) {
            if (HIR::IsPackOutUnsigned(flags)) {
              if (HIR::IsPackOutSaturate(flags)) {
                // unsigned -> unsigned + saturate
                asmjit::InvokeNode *invokeNode = nullptr;
                COMP->invoke(&invokeNode, asmjit::Imm(reinterpret_cast<uintptr_t>(EmulatePack8_IN_16_UN_UN_SAT)), sig);
                invokeNode->setArg(0, asmjit::Imm(0)); // void* ctx = nullptr
                invokeNode->setArg(1, src1);
                invokeNode->setArg(2, src2);
                invokeNode->setRet(0, dst);

                COMP->vpshufb(dst, dst, LoadXmmConst(b, XMMByteOrderMask));
              } else {
                // unsigned -> unsigned
                asmjit::InvokeNode *invokeNode = nullptr;
                COMP->invoke(&invokeNode, asmjit::Imm(reinterpret_cast<uintptr_t>(EmulatePack8_IN_16_UN_UN)), sig);
                invokeNode->setArg(0, asmjit::Imm(0)); // void* ctx = nullptr
                invokeNode->setArg(1, src1);
                invokeNode->setArg(2, src2);
                invokeNode->setRet(0, dst);

                COMP->vpshufb(dst, dst, LoadXmmConst(b, XMMByteOrderMask));
              }
            } else {
              if (HIR::IsPackOutSaturate(flags)) {
                // unsigned -> signed + saturate
                UNREACHABLE_MSG("Unimplemented PACK_TYPE_8_IN_16 type: unsigned -> signed + saturate.");
              } else {
                // unsigned -> signed
                UNREACHABLE_MSG("Unimplemented PACK_TYPE_8_IN_16 type: unsigned -> signed.");
              }
            }
          } else {
            if (HIR::IsPackOutUnsigned(flags)) {
              if (HIR::IsPackOutSaturate(flags)) {
                // signed -> unsigned + saturate
                // PACKUSWB / SaturateSignedWordToUnsignedByte
                COMP->vpackuswb(dst, src1, src2);
                COMP->vpshufb(dst, dst, LoadXmmConst(b, XMMByteOrderMask));
              } else {
                // signed -> unsigned
                UNREACHABLE_MSG("Unimplemented PACK_TYPE_8_IN_16 type: signed -> unsigned.");
              }
            } else {
              if (HIR::IsPackOutSaturate(flags)) {
                // signed -> signed + saturate
                // PACKSSWB / SaturateSignedWordToSignedByte
                COMP->vpacksswb(dst, src1, src2);
                COMP->vpshufb(dst, dst, LoadXmmConst(b, XMMByteOrderMask));
              } else {
                // signed -> signed
                UNREACHABLE_MSG("Unimplemented PACK_TYPE_8_IN_16 type: signed -> signed.");
              }
            }
          }
        } break;
        case HIR::PACK_TYPE_16_IN_32: {
          // Temps
          x86::Gp tmp = newGP32();
          x86::Xmm xmm0 = allocXmm();
          x86::Xmm xmm1 = allocXmm();

          if (HIR::IsPackInUnsigned(flags)) {
            if (HIR::IsPackOutUnsigned(flags)) {
              if (HIR::IsPackOutSaturate(flags)) {
                // unsigned -> unsigned + saturate
                // Construct a saturation max value
                COMP->mov(tmp, 0xFFFFu);
                COMP->vmovd(xmm0, tmp);
                COMP->vpshufd(xmm0, xmm0, 0b00000000);

                if (!instr->src1.value->IsConstant()) {
                  COMP->vpminud(xmm1, src1, xmm0);  // Saturate src1
                  COMP->vpshuflw(xmm1, xmm1, 0b00100010);
                  COMP->vpshufhw(xmm1, xmm1, 0b00100010);
                  COMP->vpshufd(xmm1, xmm1, 0b00001000);
                } else {
                  // TODO(DrChat): Non-zero constants
                  ASSERT(instr->src1.value->constant.v128.qword[0] == 0 && instr->src1.value->constant.v128.qword[1] == 0);
                  COMP->vpxor(xmm1, xmm1, xmm1);
                }

                if (!instr->src2.value->IsConstant()) {
                  COMP->vpminud(dst, src2, xmm0);  // Saturate src2
                  COMP->vpshuflw(dst, dst, 0b00100010);
                  COMP->vpshufhw(dst, dst, 0b00100010);
                  COMP->vpshufd(dst, dst, 0b10000000);
                } else {
                  // TODO(DrChat): Non-zero constants
                  ASSERT(instr->src1.value->constant.v128.qword[0] == 0 && instr->src1.value->constant.v128.qword[1] == 0);
                  COMP->vpxor(dst, dst, dst);
                }

                COMP->vpblendw(dst, dst, xmm1, 0b00001111);
              } else {
                // unsigned -> unsigned
                COMP->vmovaps(xmm0, src1);
                COMP->vpshuflw(xmm0, xmm0, 0b00100010);
                COMP->vpshufhw(xmm0, xmm0, 0b00100010);
                COMP->vpshufd(xmm0, xmm0, 0b00001000);

                COMP->vmovaps(dst, src2);
                COMP->vpshuflw(dst, dst, 0b00100010);
                COMP->vpshufhw(dst, dst, 0b00100010);
                COMP->vpshufd(dst, dst, 0b10000000);

                COMP->vpblendw(dst, dst, xmm0, 0b00001111);
              }
            } else {
              if (HIR::IsPackOutSaturate(flags)) {
                // unsigned -> signed + saturate
                UNREACHABLE_MSG("Unimplemented PACK_TYPE_16_IN_32 type: unsigned -> signed + saturate.");
              } else {
                // unsigned -> signed
                UNREACHABLE_MSG("Unimplemented PACK_TYPE_16_IN_32 type: unsigned -> signed.");
              }
            }
          } else {
            if (HIR::IsPackOutUnsigned(flags)) {
              if (HIR::IsPackOutSaturate(flags)) {
                // signed -> unsigned + saturate
                // PACKUSDW
                // TMP[15:0] <- (DEST[31:0] < 0) ? 0 : DEST[15:0];
                // DEST[15:0] <- (DEST[31:0] > FFFFH) ? FFFFH : TMP[15:0];
                COMP->vpackusdw(dst, src1, src2);
                COMP->vpshuflw(dst, dst, 0b10110001);
                COMP->vpshufhw(dst, dst, 0b10110001);
              } else {
                // signed -> unsigned
                UNREACHABLE_MSG("Unimplemented PACK_TYPE_16_IN_32 type: signed -> unsigned.");
              }
            } else {
              if (HIR::IsPackOutSaturate(flags)) {
                // signed -> signed + saturate
                // PACKSSDW / SaturateSignedDwordToSignedWord
                COMP->vpackssdw(dst, src1, src2);
                COMP->vpshuflw(dst, dst, 0b10110001);
                COMP->vpshufhw(dst, dst, 0b10110001);
              } else {
                // signed -> signed
                UNREACHABLE_MSG("Unimplemented PACK_TYPE_16_IN_32 type: signed -> signed.");
              }
            }
          }
        } break;
        default: UNREACHABLE_MSG("Unimplemented PACK type."); return;
        }
        // Store result
        TagStoreReg(instr->dest, dst);
      }

      // 
      // Vector Unpack
      // 

      REGISTER_EMITTER(OPCODE_UNPACK, Emit_UNPACK)
      static void Emit_UNPACK(x86CodeGenBackend *b, const HIR::Instr *instr) {
        // Destination
        x86::Xmm dst = allocXmm();
        // Get sources
        x86::Xmm src1 = LoadValueXmm(b, instr->src1.value);
        // Get flags
        u32 flags = instr->flags;
        // Pack mode
        u32 mode = flags & HIR::PACK_TYPE_MODE;

        switch (mode) {
        case HIR::PACK_TYPE_D3DCOLOR: {
          // ARGB (WXYZ) -> RGBA (XYZW)
          // src = ZZYYXXWW
          // Unpack to 000000ZZ,000000YY,000000XX,000000WW
          COMP->vpshufb(dst, src1, LoadXmmConst(b, XMMUnpackD3DCOLOR));
          // Add 1.0f to each.
          COMP->vpor(dst, dst, LoadXmmConst(b, XMMOne));
          // To convert to 0 to 1, games multiply by 0x47008081 and add 0xC7008081.
        } break;
        case HIR::PACK_TYPE_FLOAT16_2: {
          // 1 bit sign, 5 bit exponent, 10 bit mantissa
          // D3D10 half float format
          // TODO(benvanik):
          // http://blogs.msdn.com/b/chuckw/archive/2012/09/11/directxmath-f16c-and-fma.aspx
          // Use _mm_cvtph_ps -- requires very modern processors (SSE5+)
          // Unpacking half floats:
          // http://fgiesen.wordpress.com/2012/03/28/half-to-float-done-quic/
          // Packing half floats: https://gist.github.com/rygorous/2156668
          // Load source, move from tight pack of X16Y16.... to X16...Y16...
          // Also zero out the high end.
          // TODO(benvanik): special case constant unpacks that just get 0/1/etc.
        
          // sx = src.iw >> 16;
          // sy = src.iw & 0xFFFF;
          // dest = { XMConvertHalfToFloat(sx),
          //          XMConvertHalfToFloat(sy),
          //          0.0,
          //          1.0 };
          // Shuffle to 0|0|0|0|0|0|Y|X

          COMP->vpshufb(dst, src1, LoadXmmConst(b, XMMUnpackFLOAT16_2));
          COMP->vcvtph2ps(dst, dst);
          COMP->vpshufd(dst, dst, 0b10100100);
          COMP->vpor(dst, dst, LoadXmmConst(b, XMM0001));
        } break;
        case HIR::PACK_TYPE_FLOAT16_4: {
          // src = [(dest.x | dest.y), (dest.z | dest.w), 0, 0]
          // Shuffle to 0|0|0|0|W|Z|Y|X
          COMP->vpshufb(dst, src1, LoadXmmConst(b, XMMUnpackFLOAT16_4));
          COMP->vcvtph2ps(dst, dst);
        } break;
        case HIR::PACK_TYPE_SHORT_2: {
          // Temps
          x86::Xmm xmm0 = allocXmm();

          // (VD.x) = 3.0 + (VB.x>>16)*2^-22
          // (VD.y) = 3.0 + (VB.x)*2^-22
          // (VD.z) = 0.0
          // (VD.w) = 1.0 (games splat W after unpacking to get vectors of 1.0f)
          // src is (xx,xx,xx,VALUE)

          // Shuffle bytes.
          COMP->vpshufb(dst, src1, LoadXmmConst(b, XMMUnpackSHORT_2));
          // If negative, make smaller than 3 - sign extend before adding.
          COMP->vpslld(dst, dst, 16);
          COMP->vpsrad(dst,dst, 16);
          // Add 3,3,0,1.
          COMP->vpaddd(dst, dst, LoadXmmConst(b, XMM3301));
          // Return quiet NaNs in case of negative overflow.
          COMP->vcmpps(xmm0, dst, LoadXmmConst(b, XMMUnpackSHORT_Overflow), Imm(0x00)); // _CMP_EQ_OS
          COMP->vblendvps(dst, dst, LoadXmmConst(b, XMMQNaN), xmm0);
        } break;
        case HIR::PACK_TYPE_SHORT_4: {
          // Temps
          x86::Xmm xmm0 = allocXmm();
          
          // (VD.x) = 3.0 + (VB.x>>16)*2^-22
          // (VD.y) = 3.0 + (VB.x)*2^-22
          // (VD.z) = 3.0 + (VB.y>>16)*2^-22
          // (VD.w) = 3.0 + (VB.y)*2^-22
          // src is (xx,xx,VALUE,VALUE)

          // Shuffle bytes.
          COMP->vpshufb(dst, src1, LoadXmmConst(b, XMMUnpackSHORT_4));
          // If negative, make smaller than 3 - sign extend before adding.
          COMP->vpslld(dst, dst, 16);
          COMP->vpsrad(dst, dst, 16);
          // Add 3,3,3,3.
          COMP->vpaddd(dst, dst, LoadXmmConst(b, XMM3333));
          // Return quiet NaNs in case of negative overflow.
          COMP->vcmpps(xmm0, dst, LoadXmmConst(b, XMMUnpackSHORT_Overflow), Imm(0x00)); // _CMP_EQ_OS
          COMP->vblendvps(dst, dst, LoadXmmConst(b, XMMQNaN), xmm0);
        } break;
        case HIR::PACK_TYPE_UINT_2101010: { 
          // Temps
          x86::Xmm xmm0 = allocXmm();

          // Splat W.
          COMP->vshufps(dst, src1, src1, _MM_SHUFFLE(3, 3, 3, 3));
          // Keep only the needed components.
          // Red in 0-9 now, green in 10-19, blue in 20-29, alpha in 30-31.
          COMP->vpand(dst, dst, LoadXmmConst(b, XMMPackUINT_2101010_MaskPacked));
          // Shift the components down.
          COMP->vpsrlvd(dst, dst, LoadXmmConst(b, XMMPackUINT_2101010_Shift));
          // If XYZ are negative, make smaller than 3 - sign extend XYZ before adding.
          // W is unsigned.
          COMP->vpslld(dst, dst, 22);
          COMP->vpsrad(dst, dst, 22);
          // Add 3,3,3,1.
          COMP->vpaddd(dst, dst, LoadXmmConst(b, XMM3331));
          // Return quiet NaNs in case of negative overflow.
          COMP->vcmpps(xmm0, dst, LoadXmmConst(b, XMMUnpackUINT_2101010_Overflow), Imm(0x00)); // _CMP_EQ_OS
          COMP->vblendvps(dst, dst, LoadXmmConst(b, XMMQNaN), xmm0);
          // To convert XYZ to -1 to 1, games multiply by 0x46004020 & sub 0x46C06030.
          // For W to 0 to 1, they multiply by and subtract 0x4A2AAAAB.
        } break;
        case HIR::PACK_TYPE_ULONG_4202020: { 
          // Temps
          x86::Xmm xmm0 = allocXmm();

          // Extract pairs of nibbles to XZYW. XZ will have excess 4 upper bits, YW
          // will have excess 4 lower bits.
          COMP->vpshufb(dst, src1, LoadXmmConst(b, XMMUnpackULONG_4202020_Permute));
          // Drop the excess nibble of YW.
          COMP->vpsrld(xmm0, dst, 4);
          // Merge XZ and YW now both starting at offset 0.
          COMP->vshufps(dst, dst, xmm0, _MM_SHUFFLE(3, 2, 1, 0));
          // Reorder as XYZW.
          COMP->vshufps(dst, dst, dst, _MM_SHUFFLE(3, 1, 2, 0));
          // Drop the excess upper nibble in XZ and sign-extend XYZ.
          COMP->vpslld(dst, dst, 12);
          COMP->vpsrad(dst, dst, 12);
          // Add 3,3,3,1.
          COMP->vpaddd(dst, dst, LoadXmmConst(b, XMM3331));
          // Return quiet NaNs in case of negative overflow.
          COMP->vcmpps(xmm0, dst, LoadXmmConst(b, XMMUnpackULONG_4202020_Overflow), Imm(0x00)); // _CMP_EQ_OS
          COMP->vblendvps(dst, dst, LoadXmmConst(b, XMMQNaN), xmm0);
        } break;
        case HIR::PACK_TYPE_8_IN_16: {
          if (HIR::IsPackToLo(flags)) {
            // Unpack to LO.
            if (HIR::IsPackInUnsigned(flags)) {
              if (HIR::IsPackOutUnsigned(flags)) {
                // unsigned -> unsigned
                UNREACHABLE_MSG("Unimplemented UNPACK_TYPE_8_IN_16 type: unsigned -> unsigned.");
              } else {
                // unsigned -> signed
                UNREACHABLE_MSG("Unimplemented UNPACK_TYPE_8_IN_16 type: unsigned -> signed.");
              }
            } else {
              if (HIR::IsPackOutUnsigned(flags)) {
                // signed -> unsigned
                UNREACHABLE_MSG("Unimplemented UNPACK_TYPE_8_IN_16 type: signed -> unsigned.");
              } else {
                // signed -> signed
                COMP->vpshufb(dst, src1, LoadXmmConst(b, XMMByteOrderMask));
                COMP->vpunpckhbw(dst, dst, dst);
                COMP->vpsraw(dst, dst, 8);
              }
            }
          } else {
            // Unpack to HI.
            if (HIR::IsPackInUnsigned(flags)) {
              if (HIR::IsPackOutUnsigned(flags)) {
                // unsigned -> unsigned
                UNREACHABLE_MSG("Unimplemented UNPACK_TYPE_8_IN_16 type: unsigned -> unsigned.");
              } else {
                // unsigned -> signed
                UNREACHABLE_MSG("Unimplemented UNPACK_TYPE_8_IN_16 type: unsigned -> signed.");
              }
            } else {
              if (HIR::IsPackOutUnsigned(flags)) {
                // signed -> unsigned
                UNREACHABLE_MSG("Unimplemented UNPACK_TYPE_8_IN_16 type: signed -> unsigned.");
              } else {
                // signed -> signed
                COMP->vpshufb(dst, src1, LoadXmmConst(b, XMMByteOrderMask));
                COMP->vpunpcklbw(dst, dst, dst);
                COMP->vpsraw(dst, dst, 8);
              }
            }
          }
        } break;
        case HIR::PACK_TYPE_16_IN_32: {
          if (HIR::IsPackToLo(flags)) {
            // Unpack to LO.
            if (HIR::IsPackInUnsigned(flags)) {
              if (HIR::IsPackOutUnsigned(flags)) {
                // unsigned -> unsigned
                UNREACHABLE_MSG("Unimplemented UNPACK_TYPE_16_IN_32 type: unsigned -> unsigned.");
              } else {
                // unsigned -> signed
                UNREACHABLE_MSG("Unimplemented UNPACK_TYPE_16_IN_32 type: unsigned -> signed.");
              }
            } else {
              if (HIR::IsPackOutUnsigned(flags)) {
                // signed -> unsigned
                UNREACHABLE_MSG("Unimplemented UNPACK_TYPE_16_IN_32 type: signed -> unsigned.");
              } else {
                // signed -> signed
                COMP->vpunpckhwd(dst, src1, src1);
                COMP->vpsrad(dst, dst, 16);
              }
            }
          } else {
            // Unpack to HI.
            if (HIR::IsPackInUnsigned(flags)) {
              if (HIR::IsPackOutUnsigned(flags)) {
                // unsigned -> unsigned
                UNREACHABLE_MSG("Unimplemented UNPACK_TYPE_16_IN_32 type: unsigned -> unsigned.");
              } else {
                // unsigned -> signed
                UNREACHABLE_MSG("Unimplemented UNPACK_TYPE_16_IN_32 type: unsigned -> signed.");
              }
            } else {
              if (HIR::IsPackOutUnsigned(flags)) {
                // signed -> unsigned
                UNREACHABLE_MSG("Unimplemented UNPACK_TYPE_16_IN_32 type: signed -> unsigned.");
              } else {
                // signed -> signed
                COMP->vpunpcklwd(dst, src1, src1);
                COMP->vpsrad(dst, dst, 16);
              }
            }
          }
          COMP->vpshufd(dst, dst, 0xB1);
        } break;
        default: UNREACHABLE_MSG("Unimplemented UNPACK type."); return;
        }
        // Store result
        TagStoreReg(instr->dest, dst);
      }

    }
  }
}

#endif
