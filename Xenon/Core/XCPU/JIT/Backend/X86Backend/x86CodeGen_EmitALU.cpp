/***************************************************************/
/* Copyright 2026 Xenon Emulator Project. All rights reserved. */
/***************************************************************/

#include "Base/Arch.h"

#if defined(ARCH_X86) || defined(ARCH_X86_64)

#include "Base/Logging/Log.h"
#include "Core/XCPU/PPU/PPCInternal.h"
#include "Core/XCPU/JIT/Backend/X86Backend/x86CodeGenBackend.h"
#include "Core/XCPU/JIT/Backend/X86Backend/x86CodeGenHelpers.h"
#include "Core/XCPU/JIT/x86VectorConstants.h"

namespace Xe {
  namespace XCPU {
    namespace JIT {

      // Helper utilities
      
      // Move an INT64 GP register (containing raw double bits) into an XMM.
      static x86::Xmm GpToXmmDouble(x86CodeGenBackend *b, x86::Gp src) {
        x86::Xmm xmm = b->GetCompiler()->newXmm();
        b->GetCompiler()->vmovq(xmm, src.r64());
        return xmm;
      }

      // Move an XMM double back to an INT64 GP register.
      static x86::Gp XmmDoubleToGp(x86CodeGenBackend *b, x86::Xmm src) {
        x86::Gp gp = b->GetCompiler()->newGpq();
        b->GetCompiler()->vmovq(gp, src);
        return gp;
      }


      //
      // Condition Register
      //

      // Emits a CR field comparison.
      // Uses jg/jl for signed, ja/jb for unsigned
      // When instr->flags == 1 (arithmetic Rc=1): checks MSR[SF] to pick 32/64-bit.
      // When instr->flags == 0 (explicit cmp/cmpl): compares at input's native size.
      static void EmitBuildCR(x86CodeGenBackend *b, const HIR::Instr *instr, bool isSigned) {
        x86::Gp lhs = LoadValueGp(b, instr->src1.value);
        x86::Gp rhs = LoadValueGp(b, instr->src2.value);

        x86::Gp crValue = newGP32();
        x86::Gp tmp = newGP8();

        COMP->xor_(crValue, crValue);

        Label end = COMP->newLabel();

        bool useMSRCheck = (instr->flags == 1);

        if (useMSRCheck) {
          // Arithmetic Rc=1 path: check MSR[SF] for comparison mode
          Label sfBitMode = COMP->newLabel();

          x86::Gp tempMSR = newGP64();
          COMP->mov(tempMSR, SPRPtr(MSR));
          COMP->bt(tempMSR, asmjit::Imm(63)); // SF is bit 63 on LE
          COMP->jc(sfBitMode);

          // 32-bit mode
          {
            Label gt32 = COMP->newLabel();
            Label lt32 = COMP->newLabel();

            COMP->cmp(lhs.r32(), rhs.r32());
            if (isSigned) { COMP->jg(gt32); COMP->jl(lt32); }
            else          { COMP->ja(gt32); COMP->jb(lt32); }
            COMP->mov(tmp, asmjit::Imm(2));
            COMP->or_(crValue.r8(), tmp.r8());
            COMP->jmp(end);

            COMP->bind(gt32);
            COMP->mov(tmp, asmjit::Imm(4));
            COMP->or_(crValue.r8(), tmp.r8());
            COMP->jmp(end);

            COMP->bind(lt32);
            COMP->mov(tmp, asmjit::Imm(8));
            COMP->or_(crValue.r8(), tmp.r8());
            COMP->jmp(end);
          }

          // 64-bit mode
          COMP->bind(sfBitMode);
          {
            Label gt64 = COMP->newLabel();
            Label lt64 = COMP->newLabel();

            COMP->cmp(lhs.r64(), rhs.r64());
            if (isSigned) { COMP->jg(gt64); COMP->jl(lt64); }
            else          { COMP->ja(gt64); COMP->jb(lt64); }
            COMP->mov(tmp, asmjit::Imm(2));
            COMP->or_(crValue.r8(), tmp.r8());
            COMP->jmp(end);

            COMP->bind(gt64);
            COMP->mov(tmp, asmjit::Imm(4));
            COMP->or_(crValue.r8(), tmp.r8());
            COMP->jmp(end);

            COMP->bind(lt64);
            COMP->mov(tmp, asmjit::Imm(8));
            COMP->or_(crValue.r8(), tmp.r8());
          }
        } else {
          // Explicit compare path (cmp/cmpl): compare at input's native size
          Label gt = COMP->newLabel();
          Label lt = COMP->newLabel();

          COMP->cmp(lhs, rhs);
          if (isSigned) { COMP->jg(gt); COMP->jl(lt); }
          else          { COMP->ja(gt); COMP->jb(lt); }
          COMP->mov(tmp, asmjit::Imm(2));
          COMP->or_(crValue.r8(), tmp.r8());
          COMP->jmp(end);

          COMP->bind(gt);
          COMP->mov(tmp, asmjit::Imm(4));
          COMP->or_(crValue.r8(), tmp.r8());
          COMP->jmp(end);

          COMP->bind(lt);
          COMP->mov(tmp, asmjit::Imm(8));
          COMP->or_(crValue.r8(), tmp.r8());
        }

        COMP->bind(end);

        // SO bit from XER
#ifdef __LITTLE_ENDIAN__
        COMP->mov(tmp.r32(), SPRPtr(XER));
        COMP->shr(tmp.r32(), asmjit::Imm(31));
#else
        COMP->mov(tmp.r32(), SPRPtr(XER));
        COMP->and_(tmp.r32(), asmjit::Imm(1));
#endif
        COMP->shl(tmp, asmjit::Imm(3 - CR_BIT_SO));
        COMP->or_(crValue.r8(), tmp.r8());

        TagStoreReg(instr->dest, crValue);
      }

      // Performs a signed comparison between values
      // CR field bits: LT=8, GT=4, EQ=2, SO=from XER
      // flags=1: check MSR[SF] for width (arithmetic Rc=1)
      // flags=0: compare at native input size (cmp/cmpi)
      REGISTER_EMITTER(OPCODE_BUILD_CR_SIGNED, Emit_BUILD_CR_SIGNED)
      static void Emit_BUILD_CR_SIGNED(x86CodeGenBackend *b, const HIR::Instr *instr) {
        EmitBuildCR(b, instr, true);
      }

      // Performs an unsigned comparison between values
      // flags=1: check MSR[SF] for width (arithmetic Rc=1)
      // flags=0: compare at native input size (cmpl/cmpli)
      REGISTER_EMITTER(OPCODE_BUILD_CR_UNSIGNED, Emit_BUILD_CR_UNSIGNED)
      static void Emit_BUILD_CR_UNSIGNED(x86CodeGenBackend *b, const HIR::Instr *instr) {
        EmitBuildCR(b, instr, false);
      }

      // Sets the specified CR field to the specified value.
      // sig: X_O_V  (no dest, src1 = CR field index, src2 = 4-bit field value)
      REGISTER_EMITTER(OPCODE_SET_CR_FIELD, Emit_SET_CR_FIELD)
      static void Emit_SET_CR_FIELD(x86CodeGenBackend *b, const HIR::Instr *instr) {
        const u32 index = static_cast<u32>(instr->src1.offset);
        const u32 sh = (7 - index) * 4;
        const u32 clearMask = ~(0xFu << sh);

        x86::Gp field = LoadValueGp(b, instr->src2.value);

        x86::Gp field32 = newGP32();
        COMP->movzx(field32, field.r8());

        x86::Gp tempCR = newGP32();
        COMP->mov(tempCR, CRValPtr());
        COMP->and_(tempCR, asmjit::Imm(clearMask));
        COMP->shl(field32, asmjit::Imm(sh));
        COMP->or_(tempCR, field32);
        COMP->mov(CRValPtr(), tempCR);
      }

      //
      // Assign
      //

      REGISTER_EMITTER(OPCODE_ASSIGN, Emit_ASSIGN)
      static void Emit_ASSIGN(x86CodeGenBackend *b, const HIR::Instr *instr) {
        switch (instr->dest->type) {
        case HIR::INT8_TYPE:  { x86::Gp s = LoadValueGp(b, instr->src1.value);  x86::Gp d = newGP8();  COMP->mov(d, s.r8());  TagStoreReg(instr->dest, d); } break;
        case HIR::INT16_TYPE: { x86::Gp s = LoadValueGp(b, instr->src1.value);  x86::Gp d = newGP16(); COMP->mov(d, s.r16()); TagStoreReg(instr->dest, d); } break;
        case HIR::INT32_TYPE: { x86::Gp s = LoadValueGp(b, instr->src1.value);  x86::Gp d = newGP32(); COMP->mov(d, s.r32()); TagStoreReg(instr->dest, d); } break;
        case HIR::INT64_TYPE: { x86::Gp s = LoadValueGp(b, instr->src1.value);  x86::Gp d = newGP64(); COMP->mov(d, s.r64()); TagStoreReg(instr->dest, d); } break;
        case HIR::FLOAT32_TYPE: { x86::Xmm s = LoadValueXmm(b, instr->src1.value); x86::Xmm d = allocXmm(); COMP->vmovaps(d, s); TagStoreReg(instr->dest, d); } break;
        case HIR::FLOAT64_TYPE: { x86::Xmm s = LoadValueXmm(b, instr->src1.value); x86::Xmm d = allocXmm(); COMP->vmovaps(d, s); TagStoreReg(instr->dest, d); } break;
        case HIR::VEC128_TYPE:  { x86::Xmm s = LoadValueXmm(b, instr->src1.value); x86::Xmm d = allocXmm(); COMP->vmovaps(d, s); TagStoreReg(instr->dest, d); } break;
        default: UNREACHABLE_MSG("Unimplemented ASSIGN type."); return;
        }
      }

      //
      // Cast
      //

      REGISTER_EMITTER(OPCODE_CAST, Emit_CAST)
        static void Emit_CAST(x86CodeGenBackend *b, const HIR::Instr *instr) {
        switch (instr->dest->type) {
        case HIR::INT32_TYPE: {
          x86::Gp dst = newGP32();
          x86::Vec src = LoadValueXmm(b, instr->src1.value);
          COMP->vmovd(dst, src);
          TagStoreReg(instr->dest, dst);
        } break;
        case HIR::INT64_TYPE: {
          x86::Gp dst = newGP64();
          x86::Vec src = LoadValueXmm(b, instr->src1.value);
          COMP->vmovq(dst, src);
          TagStoreReg(instr->dest, dst);
        } break;
        case HIR::FLOAT32_TYPE: {
          x86::Gp src = LoadValueGp(b, instr->src1.value);
          x86::Vec dst = allocXmm();
          COMP->vmovd(dst, src);
          TagStoreReg(instr->dest, dst);
        } break;
        case HIR::FLOAT64_TYPE: {
          x86::Gp src = LoadValueGp(b, instr->src1.value);
          x86::Vec dst = allocXmm();
          COMP->vmovq(dst, src);
          TagStoreReg(instr->dest, dst);
        } break;
        default: UNREACHABLE_MSG("Unimplemented CAST type."); return;
        }
      }

      //
      // Zero Extend
      //

      REGISTER_EMITTER(OPCODE_ZERO_EXTEND, Emit_ZERO_EXTEND)
        static void Emit_ZERO_EXTEND(x86CodeGenBackend *b, const HIR::Instr *instr) {
        x86::Gp src = LoadValueGp(b, instr->src1.value);
        x86::Gp dst;
        switch (instr->dest->type) {
        case HIR::INT16_TYPE: 
          dst = newGP16(); 
          COMP->movzx(dst, src.r8());  
          break;
        case HIR::INT32_TYPE:
          dst = newGP32();
          if (instr->src1.value->type == HIR::INT16_TYPE) {
            COMP->movzx(dst, src.r16());
          } else {
            COMP->movzx(dst, src.r8());
          }
          break;
        case HIR::INT64_TYPE:
          dst = newGP64();
          if (instr->src1.value->type == HIR::INT32_TYPE) {
            COMP->mov(dst.r32(), src.r32());
          } else if (instr->src1.value->type == HIR::INT16_TYPE) {
            COMP->movzx(dst.r32(), src.r16());
          } else {
            COMP->movzx(dst.r32(), src.r8());
          }
          break;
        default: UNREACHABLE_MSG("Unimplemented OPCODE_ZERO_EXTEND type."); return;
        }
        TagStoreReg(instr->dest, dst);
      }

      //
      // Sign Extend
      //

      REGISTER_EMITTER(OPCODE_SIGN_EXTEND, Emit_SIGN_EXTEND)
        static void Emit_SIGN_EXTEND(x86CodeGenBackend *b, const HIR::Instr *instr) {
        x86::Gp src = LoadValueGp(b, instr->src1.value);
        x86::Gp dst;
        switch (instr->dest->type) {
        case HIR::INT16_TYPE: 
          dst = newGP16(); 
          COMP->movsx(dst, src.r8());  
          break;
        case HIR::INT32_TYPE:
          dst = newGP32();
          if (instr->src1.value->type == HIR::INT16_TYPE) {
            COMP->movsx(dst, src.r16());
          } else {
            COMP->movsx(dst, src.r8());
          }
          break;
        case HIR::INT64_TYPE:
          dst = newGP64();
          if (instr->src1.value->type == HIR::INT32_TYPE) {
            COMP->movsxd(dst, src.r32());
          } else if (instr->src1.value->type == HIR::INT16_TYPE) {
            COMP->movsx(dst, src.r16());
          } else {
            COMP->movsx(dst, src.r8());
          }
          break;
        default: UNREACHABLE_MSG("Unimplemented OPCODE_SIGN_EXTEND type."); return;
        }
        TagStoreReg(instr->dest, dst);
      }

      //
      // Truncate
      //

      REGISTER_EMITTER(OPCODE_TRUNCATE, Emit_TRUNCATE)
        static void Emit_TRUNCATE(x86CodeGenBackend *b, const HIR::Instr *instr) {
        x86::Gp src = LoadValueGp(b, instr->src1.value);
        x86::Gp dst;
        switch (instr->dest->type) {
        case HIR::INT8_TYPE:  
          dst = newGP8();  
          if (instr->src1.value->type == HIR::INT16_TYPE ||
            instr->src1.value->type == HIR::INT32_TYPE ||
            instr->src1.value->type == HIR::INT64_TYPE) {
            COMP->movzx(dst.r32(), src.r8());
          }    
          break;
        case HIR::INT16_TYPE: 
          dst = newGP16();
          if (instr->src1.value->type == HIR::INT32_TYPE ||
            instr->src1.value->type == HIR::INT64_TYPE) {
            COMP->movzx(dst.r32(), src.r16());
          }
          break;
        case HIR::INT32_TYPE: 
          dst = newGP32(); 
          COMP->mov(dst, src.r32()); 
          break;
        default: UNREACHABLE_MSG("Unimplemented OPCODE_TRUNCATE type."); return;
        }
        TagStoreReg(instr->dest, dst);
      }

      //
      // Convert
      //

      REGISTER_EMITTER(OPCODE_CONVERT, Emit_CONVERT)
        static void Emit_CONVERT(x86CodeGenBackend *b, const HIR::Instr *instr) {
        // Get source type
        auto srcType = instr->src1.value->type;
        // Get destination type
        auto dstType = instr->dest->type;

        switch (dstType) {
        case HIR::INT32_TYPE: {
          x86::Gp dst = newGP32();
          x86::Xmm src1 = LoadValueXmm(b, instr->src1.value);
          // Source = F32
          if (srcType == HIR::FLOAT32_TYPE) {
            if (instr->flags == HIR::ROUND_TO_ZERO) {
              COMP->vcvttss2si(dst, src1);
            } else {
              COMP->vcvtss2si(dst, src1);
            }
          } else if (srcType == HIR::FLOAT64_TYPE) {
            x86::Xmm tmp = allocXmm();
            COMP->vminsd(tmp, src1, LoadXmmConst(b, XMMIntMaxPD));
            if (instr->flags == HIR::ROUND_TO_ZERO) {
              COMP->vcvttsd2si(dst, tmp);
            } else {
              COMP->vcvtsd2si(dst, tmp);
            }
          }

          // Store value back
          TagStoreReg(instr->dest, dst);
        } break;
        case HIR::INT64_TYPE: {
          x86::Gp dst = newGP64();
          x86::Xmm src1 = LoadValueXmm(b, instr->src1.value);
          if (srcType == HIR::FLOAT64_TYPE) {
            x86::Gp tmp = newGP64();
            x86::Gp tmp1 = newGP64();
            x86::Gp sat = newGP64();
            
            COMP->movq(tmp, src1);

            if (instr->flags == HIR::ROUND_TO_ZERO) {
              COMP->vcvttsd2si(dst, src1);
            } else {
              COMP->vcvtsd2si(dst, src1);
            }

            // Saturate positive overflow

            // Check if result = 0x8000000000000000
            COMP->mov(tmp1, imm(0x1));
            COMP->shl(tmp1, imm(63));
            COMP->cmp(tmp1, dst);
            COMP->sete(sat);
            COMP->movzx(tmp1, sat.r8());
            COMP->shr(tmp, imm(63));
            COMP->xor_(tmp, imm(1));
            COMP->and_(tmp1, tmp);
            COMP->sub(dst, tmp1);
          }
          // Store value back
          TagStoreReg(instr->dest, dst);
        } break;
        case HIR::FLOAT32_TYPE: {
          x86::Xmm dst = allocXmm();
          
          if (srcType == HIR::INT32_TYPE) {
            x86::Gp src1 = LoadValueGp(b, instr->src1.value);
            COMP->vcvtsi2ss(dst, dst, src1);
          } else if (srcType == HIR::FLOAT64_TYPE) {
            x86::Xmm src1 = LoadValueXmm(b, instr->src1.value);
            COMP->vcvtsd2ss(dst, dst, src1);
          }

          // Store value back
          TagStoreReg(instr->dest, dst);
        } break;
        case HIR::FLOAT64_TYPE: {
          x86::Xmm dst = allocXmm();

          if (srcType == HIR::INT64_TYPE) {
            x86::Gp src1 = LoadValueGp(b, instr->src1.value);
            COMP->vcvtsi2sd(dst, dst, src1);
          }
          else if (srcType == HIR::FLOAT32_TYPE) {
            x86::Xmm src1 = LoadValueXmm(b, instr->src1.value);
            COMP->vcvtss2sd(dst, dst, src1);
          }

          // Store value back
          TagStoreReg(instr->dest, dst);
        } break;
        default: UNREACHABLE_MSG("Unimplemented OPCODE_CONVERT type."); return;
        }
      }

      //
      // Round
      // 
      
      REGISTER_EMITTER(OPCODE_ROUND, Emit_ROUND)
        static void Emit_ROUND(x86CodeGenBackend *b, const HIR::Instr *instr) {
        // Destination
        x86::Xmm dst = allocXmm();
        // Get source
        x86::Xmm src1 = LoadValueXmm(b, instr->src1.value);
        // Rounding Mode
        u8 roundMode = 0;

        switch (instr->flags) {
        case HIR::ROUND_TO_ZERO: roundMode = 0b00000011; break;
        case HIR::ROUND_TO_NEAREST: roundMode = 0b00000000; break;
        case HIR::ROUND_TO_MINUS_INFINITY: roundMode = 0b00000001; break;
        case HIR::ROUND_TO_POSITIVE_INFINITY: roundMode = 0b00000010; break;
        }

        switch (instr->dest->type) {
        case HIR::FLOAT32_TYPE: {
          COMP->vroundss(dst, dst, src1, imm(roundMode));
          TagStoreReg(instr->dest, dst);
        } break;
        case HIR::FLOAT64_TYPE: {
          COMP->vroundsd(dst, dst, src1, imm(roundMode));
          TagStoreReg(instr->dest, dst);
        } break;
        case HIR::VEC128_TYPE: {
          COMP->vroundps(dst, src1, imm(roundMode));
          TagStoreReg(instr->dest, dst);
        } break;
        default: UNREACHABLE_MSG("Unimplemented OPCODE_ROUND type."); return;
        }
      }

      //
      // Max
      //

      REGISTER_EMITTER(OPCODE_MAX, Emit_MAX)
        static void Emit_MAX(x86CodeGenBackend *b, const HIR::Instr *instr) {
        // Destination
        x86::Xmm dst = allocXmm();
        // Get sources
        x86::Xmm src1 = LoadValueXmm(b, instr->src1.value);
        x86::Xmm src2 = LoadValueXmm(b, instr->src2.value);

        switch (instr->dest->type) {
        case HIR::FLOAT32_TYPE:
          COMP->vmaxss(dst, src1, src2);
          break;
        case HIR::FLOAT64_TYPE:   
          COMP->vmaxsd(dst, src1, src2);
          break;
        case HIR::VEC128_TYPE:
          COMP->vmaxps(dst, src1, src2);
          break;
        default: UNREACHABLE_MSG("Unimplemented OPCODE_MAX type."); return;
        }

        // Store result
        TagStoreReg(instr->dest, dst);
      }

      //
      // Min
      //

      REGISTER_EMITTER(OPCODE_MIN, Emit_MIN)
        static void Emit_MIN(x86CodeGenBackend *b, const HIR::Instr *instr) {
        switch (instr->dest->type) {
        case HIR::INT8_TYPE: {
          // Destination
          x86::Gp dst = newGP8();
          // Get sources
          x86::Gp src1 = LoadValueGp(b, instr->src1.value);
          x86::Gp src2 = LoadValueGp(b, instr->src2.value);
          // CMOV doesn't support 8-bit, widen to 32-bit
          x86::Gp src1_32 = newGP32();
          x86::Gp src2_32 = newGP32();
          COMP->movsx(src1_32, src1.r8());
          COMP->movsx(src2_32, src2.r8());
          COMP->cmp(src1_32, src2_32);
          COMP->cmovg(src1_32, src2_32);
          COMP->mov(dst, src1_32.r8());
          // Store result
          TagStoreReg(instr->dest, dst);
        } break;
        case HIR::FLOAT64_TYPE: {
          // Destination
          x86::Xmm dst = allocXmm();
          // Get sources
          x86::Xmm src1 = LoadValueXmm(b, instr->src1.value);
          x86::Xmm src2 = LoadValueXmm(b, instr->src2.value);
          // Operation
          COMP->vminsd(dst, src1, src2);
          // Store result
          TagStoreReg(instr->dest, dst);
        } break;
        case HIR::VEC128_TYPE: {
          // Destination
          x86::Xmm dst = allocXmm();
          x86::Xmm xmm2 = allocXmm();
          x86::Xmm xmm3 = allocXmm();
          // Get sources
          x86::Xmm src1 = LoadValueXmm(b, instr->src1.value);
          x86::Xmm src2 = LoadValueXmm(b, instr->src2.value);
          // Operation
          COMP->vminps(xmm2, src1, src2);
          COMP->vminps(xmm3, src2, src1);
          COMP->vorps(dst, xmm2, xmm3);
          // Store result
          TagStoreReg(instr->dest, dst);
        } break;
        default: UNREACHABLE_MSG("Unimplemented OPCODE_MIN type."); return;
        }
      }

      //
      // Select
      //

      REGISTER_EMITTER(OPCODE_SELECT, Emit_SELECT)
        static void Emit_SELECT(x86CodeGenBackend *b, const HIR::Instr *instr) {
        switch (instr->dest->type) {
        case HIR::INT8_TYPE: {
          // Destination
          x86::Gp dst = newGP8();
          // Get sources
          x86::Gp src1 = LoadValueGp(b, instr->src1.value);
          x86::Gp src2 = LoadValueGp(b, instr->src2.value);
          x86::Gp src3 = LoadValueGp(b, instr->src3.value);
          // Operation
          COMP->test(src1, src1);
          COMP->cmovnz(dst.r32(), src2.r32());
          COMP->cmovz(dst.r32(), src3.r32());
          // Store result
          TagStoreReg(instr->dest, dst);
        } break;
        case HIR::INT16_TYPE: {
          // Destination
          x86::Gp dst = newGP16();
          // Get sources
          x86::Gp src1 = LoadValueGp(b, instr->src1.value);
          x86::Gp src2 = LoadValueGp(b, instr->src2.value);
          x86::Gp src3 = LoadValueGp(b, instr->src3.value);
          // Operation
          COMP->test(src1, src1);
          COMP->cmovnz(dst.r32(), src2.r32());
          COMP->cmovz(dst.r32(), src3.r32());
          // Store result
          TagStoreReg(instr->dest, dst);
        } break;
        case HIR::INT32_TYPE: {
          // Destination
          x86::Gp dst = newGP32();
          // Get sources
          x86::Gp src1 = LoadValueGp(b, instr->src1.value);
          x86::Gp src2 = LoadValueGp(b, instr->src2.value);
          x86::Gp src3 = LoadValueGp(b, instr->src3.value);
          // Operation
          COMP->test(src1, src1);
          COMP->cmovnz(dst, src2);
          COMP->cmovz(dst, src3);
          // Store result
          TagStoreReg(instr->dest, dst);
        } break;
        case HIR::INT64_TYPE: {
          // Destination
          x86::Gp dst = newGP64();
          // Get sources
          x86::Gp src1 = LoadValueGp(b, instr->src1.value);
          x86::Gp src2 = LoadValueGp(b, instr->src2.value);
          x86::Gp src3 = LoadValueGp(b, instr->src3.value);
          // Operation
          COMP->test(src1, src1);
          COMP->cmovnz(dst, src2);
          COMP->cmovz(dst, src3);
          // Store result
          TagStoreReg(instr->dest, dst);
        } break;
        case HIR::FLOAT32_TYPE: {
          // Destination
          x86::Xmm dst = allocXmm();
          // Get sources
          x86::Gp src1 = LoadValueGp(b, instr->src1.value);
          x86::Xmm src2 = LoadValueXmm(b, instr->src2.value);
          x86::Xmm src3 = LoadValueXmm(b, instr->src3.value);
          // Operation
          x86::Gp tmp = newGP32();
          x86::Xmm tmpXmm1 = allocXmm();
          x86::Xmm tmpXmm0 = allocXmm();
          COMP->movzx(tmp, src1);
          COMP->vmovd(tmpXmm1, tmp);
          COMP->vxorps(tmpXmm0, tmpXmm0, tmpXmm0);
          COMP->vpcmpeqd(tmpXmm0, tmpXmm0, tmpXmm1);
          COMP->vpandn(tmpXmm1, tmpXmm0, src2);
          COMP->vpand(dst, tmpXmm0, src3);
          COMP->vpor(dst, dst, tmpXmm1);

          // Store result
          TagStoreReg(instr->dest, dst);
        } break;
        case HIR::FLOAT64_TYPE: {
          // Destination
          x86::Xmm dst = allocXmm();
          // Get sources
          x86::Gp src1 = LoadValueGp(b, instr->src1.value);
          x86::Xmm src2 = LoadValueXmm(b, instr->src2.value);
          x86::Xmm src3 = LoadValueXmm(b, instr->src3.value);
          // Operation
          x86::Gp tmp = newGP32();
          x86::Xmm tmpXmm1 = allocXmm();
          x86::Xmm tmpXmm0 = allocXmm();
          COMP->movzx(tmp, src1);
          COMP->vmovd(tmpXmm1, tmp);
          COMP->vpxor(tmpXmm0, tmpXmm0, tmpXmm0);
          COMP->vpcmpeqq(tmpXmm0, tmpXmm0, tmpXmm1);
          COMP->vpandn(tmpXmm1, tmpXmm0, src2);
          COMP->vpand(dst, tmpXmm0, src3);
          COMP->vpor(dst, dst, tmpXmm1);

          // Store result
          TagStoreReg(instr->dest, dst);
        } break;
        case HIR::VEC128_TYPE: {
          // Destination
          x86::Xmm dst = allocXmm();
          
          if (instr->src1.value->type == HIR::INT8_TYPE) {
            // Get sources
            x86::Gp src1 = LoadValueGp(b, instr->src1.value);
            x86::Xmm src2 = LoadValueXmm(b, instr->src2.value);
            x86::Xmm src3 = LoadValueXmm(b, instr->src3.value);
            // Operation
            x86::Gp tmp = newGP32();
            x86::Xmm tmpXmm1 = allocXmm();
            x86::Xmm tmpXmm0 = allocXmm();
            COMP->movzx(tmp, src1);
            COMP->vmovd(tmpXmm1, tmp);
            COMP->vpbroadcastd(tmpXmm1, tmpXmm1);
            COMP->vxorps(tmpXmm0, tmpXmm0, tmpXmm0);
            COMP->vpcmpeqd(tmpXmm0, tmpXmm0, tmpXmm1);
            COMP->vpandn(tmpXmm1, tmpXmm0, src2);
            COMP->vpand(dst, tmpXmm0, src3);
            COMP->vpor(dst, dst, tmpXmm1);
          } else if (instr->src1.value->type == HIR::VEC128_TYPE) {
            // Get sources
            x86::Xmm src1 = LoadValueXmm(b, instr->src1.value);
            x86::Xmm src2 = LoadValueXmm(b, instr->src2.value);
            x86::Xmm src3 = LoadValueXmm(b, instr->src3.value);
            // Operation
            x86::Xmm tmpXmm0 = allocXmm();
            COMP->vpandn(tmpXmm0, src1, src2);
            COMP->vpand(dst, src1, src3);
            COMP->vpor(dst, dst, tmpXmm0);
          }

          // Store result
          TagStoreReg(instr->dest, dst);
        } break;
        default: UNREACHABLE_MSG("Unimplemented OPCODE_SELECT type."); return;
        }
      }

      //
      // Is True
      //

      REGISTER_EMITTER(OPCODE_IS_TRUE, Emit_IS_TRUE)
        static void Emit_IS_TRUE(x86CodeGenBackend *b, const HIR::Instr *instr) {
        // Destination
        x86::Gp dst = newGP8();

        switch (instr->src1.value->type) {
        case HIR::INT8_TYPE: {
          // Get sources
          x86::Gp src1 = LoadValueGp(b, instr->src1.value);
          COMP->test(src1, src1);
        } break;
        case HIR::INT16_TYPE: {
          // Get sources
          x86::Gp src1 = LoadValueGp(b, instr->src1.value);
          COMP->test(src1, src1);
        } break;
        case HIR::INT32_TYPE: {
          // Get sources
          x86::Gp src1 = LoadValueGp(b, instr->src1.value);
          COMP->test(src1, src1);
        } break;
        case HIR::INT64_TYPE: {
          // Get sources
          x86::Gp src1 = LoadValueGp(b, instr->src1.value);
          COMP->test(src1, src1);
        } break;
        case HIR::FLOAT32_TYPE: {
          // Get sources
          x86::Xmm src1 = LoadValueXmm(b, instr->src1.value);
          COMP->vptest(src1, src1);
        } break;
        case HIR::FLOAT64_TYPE: {
          // Get sources
          x86::Xmm src1 = LoadValueXmm(b, instr->src1.value);
          COMP->vptest(src1, src1);
        } break;
        case HIR::VEC128_TYPE: {
          // Get sources
          x86::Xmm src1 = LoadValueXmm(b, instr->src1.value);
          COMP->vptest(src1, src1);
        } break;
        default: UNREACHABLE_MSG("Unimplemented IS_TRUE type."); return;
        }

        // Set depending on test result
        COMP->setnz(dst);
        // Store result
        TagStoreReg(instr->dest, dst);
      }

      //
      // Is False
      //
      
      REGISTER_EMITTER(OPCODE_IS_FALSE, Emit_IS_FALSE)
        static void Emit_IS_FALSE(x86CodeGenBackend *b, const HIR::Instr *instr) {
        // Destination
        x86::Gp dst = newGP8();

        switch (instr->src1.value->type) {
        case HIR::INT8_TYPE: {
          // Get sources
          x86::Gp src1 = LoadValueGp(b, instr->src1.value);
          COMP->test(src1, src1);
        } break;
        case HIR::INT16_TYPE: {
          // Get sources
          x86::Gp src1 = LoadValueGp(b, instr->src1.value);
          COMP->test(src1, src1);
        } break;
        case HIR::INT32_TYPE: {
          // Get sources
          x86::Gp src1 = LoadValueGp(b, instr->src1.value);
          COMP->test(src1, src1);
        } break;
        case HIR::INT64_TYPE: {
          // Get sources
          x86::Gp src1 = LoadValueGp(b, instr->src1.value);
          COMP->test(src1, src1);
        } break;
        case HIR::FLOAT32_TYPE: {
          // Get sources
          x86::Xmm src1 = LoadValueXmm(b, instr->src1.value);
          COMP->vptest(src1, src1);
        } break;
        case HIR::FLOAT64_TYPE: {
          // Get sources
          x86::Xmm src1 = LoadValueXmm(b, instr->src1.value);
          COMP->vptest(src1, src1);
        } break;
        case HIR::VEC128_TYPE: {
          // Get sources
          x86::Xmm src1 = LoadValueXmm(b, instr->src1.value);
          COMP->vptest(src1, src1);
        } break;
        default: UNREACHABLE_MSG("Unimplemented IS_FALSE type."); return;
        }

        // Set depending on test result
        COMP->setz(dst);
        // Store result
        TagStoreReg(instr->dest, dst);
      }

      //
      // Is NaN
      //

      REGISTER_EMITTER(OPCODE_IS_NAN, Emit_IS_NAN)
        static void Emit_IS_NAN(x86CodeGenBackend *b, const HIR::Instr *instr) {
        // Destination
        x86::Gp dst = newGP8();
        // Get sources
        x86::Xmm src1 = LoadValueXmm(b, instr->src1.value);

        switch (instr->src1.value->type) {
        case HIR::FLOAT32_TYPE: {
          COMP->vucomiss(src1, src1);
        } break;
        case HIR::FLOAT64_TYPE: {
          COMP->vucomisd(src1, src1);
        } break;
        default: UNREACHABLE_MSG("Unimplemented IS_NAN type."); return;
        }

        // Set depending on test result
        COMP->setp(dst);
        // Store result
        TagStoreReg(instr->dest, dst);
      }

      //
      // Compare EQ
      //

      REGISTER_EMITTER(OPCODE_COMPARE_EQ, Emit_COMPARE_EQ)
        static void Emit_COMPARE_EQ(x86CodeGenBackend *b, const HIR::Instr *instr) {
        // Destination
        x86::Gp dst = newGP8();

        switch (instr->src1.value->type) {
        case HIR::INT8_TYPE: {
          // Get sources
          x86::Gp src1 = LoadValueGp(b, instr->src1.value);
          x86::Gp src2 = LoadValueGp(b, instr->src2.value);
          COMP->cmp(src1, src2);
        } break;
        case HIR::INT16_TYPE: {
          // Get sources
          x86::Gp src1 = LoadValueGp(b, instr->src1.value);
          x86::Gp src2 = LoadValueGp(b, instr->src2.value);
          COMP->cmp(src1, src2);
        } break;
        case HIR::INT32_TYPE: {
          // Get sources
          x86::Gp src1 = LoadValueGp(b, instr->src1.value);
          x86::Gp src2 = LoadValueGp(b, instr->src2.value);
          COMP->cmp(src1, src2);
        } break;
        case HIR::INT64_TYPE: {
          // Get sources
          x86::Gp src1 = LoadValueGp(b, instr->src1.value);
          x86::Gp src2 = LoadValueGp(b, instr->src2.value);
          COMP->cmp(src1, src2);
        } break;
        case HIR::FLOAT32_TYPE: {
          // Get sources
          x86::Xmm src1 = LoadValueXmm(b, instr->src1.value);
          x86::Xmm src2 = LoadValueXmm(b, instr->src2.value);
          COMP->vcomiss(src1, src2);
        } break;
        case HIR::FLOAT64_TYPE: {
          // Get sources
          x86::Xmm src1 = LoadValueXmm(b, instr->src1.value);
          x86::Xmm src2 = LoadValueXmm(b, instr->src2.value);
          COMP->vcomisd(src1, src2);
        } break;
        default: UNREACHABLE_MSG("Unimplemented COMPARE_EQ type."); return;
        }

        // Set depending on test result
        COMP->sete(dst);
        // Store result
        TagStoreReg(instr->dest, dst);
      }

      //
      // Compare NE
      //

      REGISTER_EMITTER(OPCODE_COMPARE_NE, Emit_COMPARE_NE)
        static void Emit_COMPARE_NE(x86CodeGenBackend *b, const HIR::Instr *instr) {
        // Destination
        x86::Gp dst = newGP8();

        switch (instr->src1.value->type) {
        case HIR::INT8_TYPE: {
          // Get sources
          x86::Gp src1 = LoadValueGp(b, instr->src1.value);
          x86::Gp src2 = LoadValueGp(b, instr->src2.value);
          COMP->cmp(src1, src2);
        } break;
        case HIR::INT16_TYPE: {
          // Get sources
          x86::Gp src1 = LoadValueGp(b, instr->src1.value);
          x86::Gp src2 = LoadValueGp(b, instr->src2.value);
          COMP->cmp(src1, src2);
        } break;
        case HIR::INT32_TYPE: {
          // Get sources
          x86::Gp src1 = LoadValueGp(b, instr->src1.value);
          x86::Gp src2 = LoadValueGp(b, instr->src2.value);
          COMP->cmp(src1, src2);
        } break;
        case HIR::INT64_TYPE: {
          // Get sources
          x86::Gp src1 = LoadValueGp(b, instr->src1.value);
          x86::Gp src2 = LoadValueGp(b, instr->src2.value);
          COMP->cmp(src1, src2);
        } break;
        case HIR::FLOAT32_TYPE: {
          // Get sources
          x86::Xmm src1 = LoadValueXmm(b, instr->src1.value);
          x86::Xmm src2 = LoadValueXmm(b, instr->src2.value);
          COMP->vcomiss(src1, src2);
        } break;
        case HIR::FLOAT64_TYPE: {
          // Get sources
          x86::Xmm src1 = LoadValueXmm(b, instr->src1.value);
          x86::Xmm src2 = LoadValueXmm(b, instr->src2.value);
          COMP->vcomisd(src1, src2);
        } break;
        default: UNREACHABLE_MSG("Unimplemented COMPARE_NE type."); return;
        }

        // Set depending on test result
        COMP->setne(dst);
        // Store result
        TagStoreReg(instr->dest, dst);
      }

      //
      // Compare XX (Signed)
      //
      // Note: For floating-point comparisons, vcomisd/vcomiss set flags differently:
      //   - CF=1 if src1 < src2 OR unordered (NaN)
      //   - ZF=1 if src1 == src2
      //   - PF=1 if unordered (NaN)
      // So we use: setb (CF=1) for LT, seta (CF=0 && ZF=0) for GT, etc.
      // For integer comparisons, we use the standard signed condition codes.

      // COMPARE_SLT: src1 < src2 (signed)
      REGISTER_EMITTER(OPCODE_COMPARE_SLT, Emit_COMPARE_SLT)
      static void Emit_COMPARE_SLT(x86CodeGenBackend *b, const HIR::Instr *instr) {
        x86::Gp dst = newGP8();

        switch (instr->src1.value->type) {
        case HIR::INT8_TYPE:
        case HIR::INT16_TYPE:
        case HIR::INT32_TYPE:
        case HIR::INT64_TYPE: {
          x86::Gp src1 = LoadValueGp(b, instr->src1.value);
          x86::Gp src2 = LoadValueGp(b, instr->src2.value);
          switch (instr->src1.value->type) {
          case HIR::INT8_TYPE:  COMP->cmp(src1.r8(),  src2.r8());  break;
          case HIR::INT16_TYPE: COMP->cmp(src1.r16(), src2.r16()); break;
          case HIR::INT32_TYPE: COMP->cmp(src1.r32(), src2.r32()); break;
          case HIR::INT64_TYPE: COMP->cmp(src1.r64(), src2.r64()); break;
          default: break;
          }
          COMP->setl(dst);  // SF != OF
        } break;
        case HIR::FLOAT32_TYPE: {
          x86::Xmm src1 = LoadValueXmm(b, instr->src1.value);
          x86::Xmm src2 = LoadValueXmm(b, instr->src2.value);
          COMP->vcomiss(src1, src2);
          COMP->setb(dst);  // CF=1 (src1 < src2, but also set for unordered - caller handles NaN separately)
        } break;
        case HIR::FLOAT64_TYPE: {
          x86::Xmm src1 = LoadValueXmm(b, instr->src1.value);
          x86::Xmm src2 = LoadValueXmm(b, instr->src2.value);
          COMP->vcomisd(src1, src2);
          COMP->setb(dst);  // CF=1
        } break;
        default: UNREACHABLE_MSG("Unimplemented COMPARE_SLT type."); return;
        }
        TagStoreReg(instr->dest, dst);
      }

      // COMPARE_SLE: src1 <= src2 (signed)
      REGISTER_EMITTER(OPCODE_COMPARE_SLE, Emit_COMPARE_SLE)
      static void Emit_COMPARE_SLE(x86CodeGenBackend *b, const HIR::Instr *instr) {
        x86::Gp dst = newGP8();

        switch (instr->src1.value->type) {
        case HIR::INT8_TYPE:
        case HIR::INT16_TYPE:
        case HIR::INT32_TYPE:
        case HIR::INT64_TYPE: {
          x86::Gp src1 = LoadValueGp(b, instr->src1.value);
          x86::Gp src2 = LoadValueGp(b, instr->src2.value);
          switch (instr->src1.value->type) {
          case HIR::INT8_TYPE:  COMP->cmp(src1.r8(),  src2.r8());  break;
          case HIR::INT16_TYPE: COMP->cmp(src1.r16(), src2.r16()); break;
          case HIR::INT32_TYPE: COMP->cmp(src1.r32(), src2.r32()); break;
          case HIR::INT64_TYPE: COMP->cmp(src1.r64(), src2.r64()); break;
          default: break;
          }
          COMP->setle(dst);  // ZF=1 OR SF != OF
        } break;
        case HIR::FLOAT32_TYPE: {
          x86::Xmm src1 = LoadValueXmm(b, instr->src1.value);
          x86::Xmm src2 = LoadValueXmm(b, instr->src2.value);
          COMP->vcomiss(src1, src2);
          COMP->setbe(dst);  // CF=1 OR ZF=1
        } break;
        case HIR::FLOAT64_TYPE: {
          x86::Xmm src1 = LoadValueXmm(b, instr->src1.value);
          x86::Xmm src2 = LoadValueXmm(b, instr->src2.value);
          COMP->vcomisd(src1, src2);
          COMP->setbe(dst);  // CF=1 OR ZF=1
        } break;
        default: UNREACHABLE_MSG("Unimplemented COMPARE_SLE type."); return;
        }
        TagStoreReg(instr->dest, dst);
      }

      // COMPARE_SGT: src1 > src2 (signed)
      REGISTER_EMITTER(OPCODE_COMPARE_SGT, Emit_COMPARE_SGT)
      static void Emit_COMPARE_SGT(x86CodeGenBackend *b, const HIR::Instr *instr) {
        x86::Gp dst = newGP8();

        switch (instr->src1.value->type) {
        case HIR::INT8_TYPE:
        case HIR::INT16_TYPE:
        case HIR::INT32_TYPE:
        case HIR::INT64_TYPE: {
          x86::Gp src1 = LoadValueGp(b, instr->src1.value);
          x86::Gp src2 = LoadValueGp(b, instr->src2.value);
          switch (instr->src1.value->type) {
          case HIR::INT8_TYPE:  COMP->cmp(src1.r8(),  src2.r8());  break;
          case HIR::INT16_TYPE: COMP->cmp(src1.r16(), src2.r16()); break;
          case HIR::INT32_TYPE: COMP->cmp(src1.r32(), src2.r32()); break;
          case HIR::INT64_TYPE: COMP->cmp(src1.r64(), src2.r64()); break;
          default: break;
          }
          COMP->setg(dst);  // ZF=0 AND SF == OF
        } break;
        case HIR::FLOAT32_TYPE: {
          x86::Xmm src1 = LoadValueXmm(b, instr->src1.value);
          x86::Xmm src2 = LoadValueXmm(b, instr->src2.value);
          COMP->vcomiss(src1, src2);
          COMP->seta(dst);  // CF=0 AND ZF=0
        } break;
        case HIR::FLOAT64_TYPE: {
          x86::Xmm src1 = LoadValueXmm(b, instr->src1.value);
          x86::Xmm src2 = LoadValueXmm(b, instr->src2.value);
          COMP->vcomisd(src1, src2);
          COMP->seta(dst);  // CF=0 AND ZF=0
        } break;
        default: UNREACHABLE_MSG("Unimplemented COMPARE_SGT type."); return;
        }
        TagStoreReg(instr->dest, dst);
      }

      // COMPARE_SGE: src1 >= src2 (signed)
      REGISTER_EMITTER(OPCODE_COMPARE_SGE, Emit_COMPARE_SGE)
      static void Emit_COMPARE_SGE(x86CodeGenBackend *b, const HIR::Instr *instr) {
        x86::Gp dst = newGP8();

        switch (instr->src1.value->type) {
        case HIR::INT8_TYPE:
        case HIR::INT16_TYPE:
        case HIR::INT32_TYPE:
        case HIR::INT64_TYPE: {
          x86::Gp src1 = LoadValueGp(b, instr->src1.value);
          x86::Gp src2 = LoadValueGp(b, instr->src2.value);
          switch (instr->src1.value->type) {
          case HIR::INT8_TYPE:  COMP->cmp(src1.r8(),  src2.r8());  break;
          case HIR::INT16_TYPE: COMP->cmp(src1.r16(), src2.r16()); break;
          case HIR::INT32_TYPE: COMP->cmp(src1.r32(), src2.r32()); break;
          case HIR::INT64_TYPE: COMP->cmp(src1.r64(), src2.r64()); break;
          default: break;
          }
          COMP->setge(dst);  // SF == OF
        } break;
        case HIR::FLOAT32_TYPE: {
          x86::Xmm src1 = LoadValueXmm(b, instr->src1.value);
          x86::Xmm src2 = LoadValueXmm(b, instr->src2.value);
          COMP->vcomiss(src1, src2);
          COMP->setae(dst);  // CF=0
        } break;
        case HIR::FLOAT64_TYPE: {
          x86::Xmm src1 = LoadValueXmm(b, instr->src1.value);
          x86::Xmm src2 = LoadValueXmm(b, instr->src2.value);
          COMP->vcomisd(src1, src2);
          COMP->setae(dst);  // CF=0
        } break;
        default: UNREACHABLE_MSG("Unimplemented COMPARE_SGE type."); return;
        }
        TagStoreReg(instr->dest, dst);
      }

      // Integer unsigned comparisons (no FP support needed)
#define EMIT_COMPARE_UNSIGNED_INT(opname, setcc_fn)                                   \
      REGISTER_EMITTER(OPCODE_##opname, Emit_##opname)                                \
      static void Emit_##opname(x86CodeGenBackend *b, const HIR::Instr *instr) {      \
        x86::Gp dst = newGP8();                                                       \
                                                                                      \
        x86::Gp src1 = LoadValueGp(b, instr->src1.value);                             \
        x86::Gp src2 = LoadValueGp(b, instr->src2.value);                             \
                                                                                      \
        switch (instr->src1.value->type) {                                            \
        case HIR::INT8_TYPE:  COMP->cmp(src1.r8(),  src2.r8());  break;               \
        case HIR::INT16_TYPE: COMP->cmp(src1.r16(), src2.r16()); break;               \
        case HIR::INT32_TYPE: COMP->cmp(src1.r32(), src2.r32()); break;               \
        case HIR::INT64_TYPE: COMP->cmp(src1.r64(), src2.r64()); break;               \
        default: UNREACHABLE_MSG("Unimplemented COMPARE_XX type."); return;           \
        }                                                                             \
        COMP->setcc_fn(dst);                                                          \
        TagStoreReg(instr->dest, dst);                                                \
      }

      EMIT_COMPARE_UNSIGNED_INT(COMPARE_ULT, setb)
      EMIT_COMPARE_UNSIGNED_INT(COMPARE_ULE, setbe)
      EMIT_COMPARE_UNSIGNED_INT(COMPARE_UGT, seta)
      EMIT_COMPARE_UNSIGNED_INT(COMPARE_UGE, setae)

#undef EMIT_COMPARE_UNSIGNED_INT

      
      //
      // Did Saturate
      // 
      REGISTER_EMITTER(OPCODE_DID_SATURATE, Emit_DID_SATURATE)
      static void Emit_DID_SATURATE(x86CodeGenBackend *b, const HIR::Instr *instr) {
        // TODO(bitsh1ft3r): Implement actual saturation check!
        // Destination
        x86::Gp dst = newGP8();
        // Operation (Clear for now, no saturation)
        COMP->xor_(dst.r32(), dst.r32());
        // Store result
        TagStoreReg(instr->dest, dst);
      }


      //
      // Add
      //

      REGISTER_EMITTER(OPCODE_ADD, Emit_ADD)
        static void Emit_ADD(x86CodeGenBackend *b, const HIR::Instr *instr) {
        switch (instr->src1.value->type) {
        case HIR::INT8_TYPE: {
          // Destination
          x86::Gp dst = newGP8();
          // Get sources
          x86::Gp src1 = LoadValueGp(b, instr->src1.value);
          x86::Gp src2 = LoadValueGp(b, instr->src2.value);
          // Operation
          COMP->mov(dst, src1.r8());
          COMP->add(dst, src2.r8());
          // Store result
          TagStoreReg(instr->dest, dst);
        } break;
        case HIR::INT16_TYPE: {
          // Destination
          x86::Gp dst = newGP16();
          // Get sources
          x86::Gp src1 = LoadValueGp(b, instr->src1.value);
          x86::Gp src2 = LoadValueGp(b, instr->src2.value);
          // Operation
          COMP->mov(dst, src1.r16());
          COMP->add(dst, src2.r16());
          // Store result
          TagStoreReg(instr->dest, dst);
        } break;
        case HIR::INT32_TYPE: {
          // Destination
          x86::Gp dst = newGP32();
          // Get sources
          x86::Gp src1 = LoadValueGp(b, instr->src1.value);
          x86::Gp src2 = LoadValueGp(b, instr->src2.value);
          // Operation
          COMP->mov(dst, src1.r32());
          COMP->add(dst, src2.r32());
          // Store result
          TagStoreReg(instr->dest, dst);
        } break;
        case HIR::INT64_TYPE: {
          // Destination
          x86::Gp dst = newGP64();
          // Get sources
          x86::Gp src1 = LoadValueGp(b, instr->src1.value);
          x86::Gp src2 = LoadValueGp(b, instr->src2.value);
          // Operation
          COMP->mov(dst, src1.r64());
          COMP->add(dst, src2.r64());
          // Store result
          TagStoreReg(instr->dest, dst);
        } break;
        case HIR::FLOAT32_TYPE: {
          // Destination
          x86::Xmm dst = allocXmm();
          // Get sources
          x86::Xmm src1 = LoadValueXmm(b, instr->src1.value);
          x86::Xmm src2 = LoadValueXmm(b, instr->src2.value);
          COMP->vaddss(dst, src1, src2);
          // Store result
          TagStoreReg(instr->dest, dst);
        } break;
        case HIR::FLOAT64_TYPE: {
          // Destination
          x86::Xmm dst = allocXmm();
          // Get sources
          x86::Xmm src1 = LoadValueXmm(b, instr->src1.value);
          x86::Xmm src2 = LoadValueXmm(b, instr->src2.value);
          COMP->vaddsd(dst, src1, src2);
          // Store result
          TagStoreReg(instr->dest, dst);
        } break;
        case HIR::VEC128_TYPE: {
          // Destination
          x86::Xmm dst = allocXmm();
          // Get sources
          x86::Xmm src1 = LoadValueXmm(b, instr->src1.value);
          x86::Xmm src2 = LoadValueXmm(b, instr->src2.value);
          COMP->vaddps(dst, src1, src2);
          // Store result
          TagStoreReg(instr->dest, dst);
        } break;
        default: UNREACHABLE_MSG("Unimplemented ADD type."); return;
        }
      }

      //
      // Add with Carry
      //

      REGISTER_EMITTER(OPCODE_ADD_CARRY, Emit_ADD_CARRY)
      static void Emit_ADD_CARRY(x86CodeGenBackend *b, const HIR::Instr *instr) {
        // Destination
        x86::Gp dst;
        // Get sources
        x86::Gp src1 = LoadValueGp(b, instr->src1.value);
        x86::Gp src2 = LoadValueGp(b, instr->src2.value);
        x86::Gp carry = LoadValueGp(b, instr->src3.value);
        

        switch (instr->dest->type) {
        case HIR::INT8_TYPE:
          dst = newGP8();
          COMP->mov(dst, src1.r8());
          COMP->add(dst, src2.r8());
          COMP->add(dst, carry.r8());
          break;
        case HIR::INT16_TYPE: {
          dst = newGP16();
          x86::Gp cExt = newGP16();
          COMP->mov(dst, src1.r16());
          COMP->add(dst, src2.r16());
          COMP->movzx(cExt, carry.r8());
          COMP->add(dst, cExt);
        } break;
        case HIR::INT32_TYPE: {
          dst = newGP32();
          x86::Gp cExt = newGP32();
          COMP->mov(dst, src1.r32());
          COMP->add(dst, src2.r32());
          COMP->movzx(cExt, carry.r8());
          COMP->add(dst, cExt);
        } break;
        case HIR::INT64_TYPE: {
          dst = newGP64();
          x86::Gp cExt = newGP64();
          COMP->movzx(cExt.r32(), carry.r8());
          COMP->mov(dst, src1.r64());
          COMP->add(dst, src2.r64());
          COMP->add(dst, cExt);
        } break;
        default: UNREACHABLE_MSG("Unimplemented ADD_CARRY type."); return;
        }
        // Store result
        TagStoreReg(instr->dest, dst);
      }

      //
      // Subtract
      //

      REGISTER_EMITTER(OPCODE_SUB, Emit_SUB)
      static void Emit_SUB(x86CodeGenBackend *b, const HIR::Instr *instr) {
        switch (instr->src1.value->type) {
        case HIR::INT8_TYPE: {
          // Destination
          x86::Gp dst = newGP8();
          // Get sources
          x86::Gp src1 = LoadValueGp(b, instr->src1.value);
          x86::Gp src2 = LoadValueGp(b, instr->src2.value);
          // Operation
          COMP->mov(dst, src1.r8());
          COMP->sub(dst, src2.r8());
          // Store result
          TagStoreReg(instr->dest, dst);
        } break;
        case HIR::INT16_TYPE: {
          // Destination
          x86::Gp dst = newGP16();
          // Get sources
          x86::Gp src1 = LoadValueGp(b, instr->src1.value);
          x86::Gp src2 = LoadValueGp(b, instr->src2.value);
          // Operation
          COMP->mov(dst, src1.r16());
          COMP->sub(dst, src2.r16());
          // Store result
          TagStoreReg(instr->dest, dst);
        } break;
        case HIR::INT32_TYPE: {
          // Destination
          x86::Gp dst = newGP32();
          // Get sources
          x86::Gp src1 = LoadValueGp(b, instr->src1.value);
          x86::Gp src2 = LoadValueGp(b, instr->src2.value);
          // Operation
          COMP->mov(dst, src1.r32());
          COMP->sub(dst, src2.r32());
          // Store result
          TagStoreReg(instr->dest, dst);
        } break;
        case HIR::INT64_TYPE: {
          // Destination
          x86::Gp dst = newGP64();
          // Get sources
          x86::Gp src1 = LoadValueGp(b, instr->src1.value);
          x86::Gp src2 = LoadValueGp(b, instr->src2.value);
          // Operation
          COMP->mov(dst, src1.r64());
          COMP->sub(dst, src2.r64());
          // Store result
          TagStoreReg(instr->dest, dst);
        } break;
        case HIR::FLOAT32_TYPE: {
          // Destination
          x86::Xmm dst = allocXmm();
          // Get sources
          x86::Xmm src1 = LoadValueXmm(b, instr->src1.value);
          x86::Xmm src2 = LoadValueXmm(b, instr->src2.value);
          COMP->vsubss(dst, src1, src2);
          // Store result
          TagStoreReg(instr->dest, dst);
        } break;
        case HIR::FLOAT64_TYPE: {
          // Destination
          x86::Xmm dst = allocXmm();
          // Get sources
          x86::Xmm src1 = LoadValueXmm(b, instr->src1.value);
          x86::Xmm src2 = LoadValueXmm(b, instr->src2.value);
          COMP->vsubsd(dst, src1, src2);
          // Store result
          TagStoreReg(instr->dest, dst);
        } break;
        case HIR::VEC128_TYPE: {
          // Destination
          x86::Xmm dst = allocXmm();
          // Get sources
          x86::Xmm src1 = LoadValueXmm(b, instr->src1.value);
          x86::Xmm src2 = LoadValueXmm(b, instr->src2.value);
          COMP->vsubps(dst, src1, src2);
          // Store result
          TagStoreReg(instr->dest, dst);
        } break;
        default: UNREACHABLE_MSG("Unimplemented SUB type."); return;
        }
      }

      //
      // Multiply
      //

      REGISTER_EMITTER(OPCODE_MUL, Emit_MUL)
      static void Emit_MUL(x86CodeGenBackend *b, const HIR::Instr *instr) {       
        switch (instr->src1.value->type) {
        case HIR::INT8_TYPE: {
          // Destination
          x86::Gp dst = newGP16();
          // Get sources
          x86::Gp src1 = LoadValueGp(b, instr->src1.value);
          x86::Gp src2 = LoadValueGp(b, instr->src2.value);
          // Operation
          COMP->mov(dst, src1.r8());
          COMP->imul(dst, src2.r8());
          // Store result
          TagStoreReg(instr->dest, dst);
        } break;
        case HIR::INT16_TYPE: {
          // Destination
          x86::Gp dst = newGP16();
          // Get sources
          x86::Gp src1 = LoadValueGp(b, instr->src1.value);
          x86::Gp src2 = LoadValueGp(b, instr->src2.value);
          // Operation
          COMP->mov(dst, src1.r16());
          COMP->imul(dst, src2.r16());
          // Store result
          TagStoreReg(instr->dest, dst);
        } break;
        case HIR::INT32_TYPE: {
          // Destination
          x86::Gp dst = newGP32();
          // Get sources
          x86::Gp src1 = LoadValueGp(b, instr->src1.value);
          x86::Gp src2 = LoadValueGp(b, instr->src2.value);
          // Operation
          COMP->mov(dst, src1.r32());
          COMP->imul(dst, src2.r32());
          // Store result
          TagStoreReg(instr->dest, dst);
        } break;
        case HIR::INT64_TYPE: {
          // Destination
          x86::Gp dst = newGP64();
          // Get sources
          x86::Gp src1 = LoadValueGp(b, instr->src1.value);
          x86::Gp src2 = LoadValueGp(b, instr->src2.value);
          // Operation
          COMP->mov(dst, src1.r64());
          COMP->imul(dst, src2.r64());
          // Store result
          TagStoreReg(instr->dest, dst);
        } break;
        case HIR::FLOAT32_TYPE: {
          // Destination
          x86::Xmm dst = allocXmm();
          // Get sources
          x86::Xmm src1 = LoadValueXmm(b, instr->src1.value);
          x86::Xmm src2 = LoadValueXmm(b, instr->src2.value);
          COMP->vmulss(dst, src1, src2);
          // Store result
          TagStoreReg(instr->dest, dst);
        } break;
        case HIR::FLOAT64_TYPE: {
          // Destination
          x86::Xmm dst = allocXmm();
          // Get sources
          x86::Xmm src1 = LoadValueXmm(b, instr->src1.value);
          x86::Xmm src2 = LoadValueXmm(b, instr->src2.value);
          COMP->vmulsd(dst, src1, src2);
          // Store result
          TagStoreReg(instr->dest, dst);
        } break;
        case HIR::VEC128_TYPE: {
          // Destination
          x86::Xmm dst = allocXmm();
          // Get sources
          x86::Xmm src1 = LoadValueXmm(b, instr->src1.value);
          x86::Xmm src2 = LoadValueXmm(b, instr->src2.value);
          COMP->vmulps(dst, src1, src2);
          // Store result
          TagStoreReg(instr->dest, dst);
        } break;
        default: UNREACHABLE_MSG("Unimplemented MUL type."); return;
        }
      }

      //
      // Multiply High
      //

      REGISTER_EMITTER(OPCODE_MUL_HI, Emit_MUL_HI)
      static void Emit_MUL_HI(x86CodeGenBackend *b, const HIR::Instr *instr) {
        // Get sources
        x86::Gp src1 = LoadValueGp(b, instr->src1.value);
        x86::Gp src2 = LoadValueGp(b, instr->src2.value);

        switch (instr->dest->type) {
        case HIR::INT32_TYPE: {
          // Widen 32->64, multiply, shift right 32 to get high half.
          x86::Gp lhs64 = newGP64();
          x86::Gp rhs64 = newGP64();
          if (instr->flags & HIR::ARITHMETIC_UNSIGNED) {
            COMP->mov(lhs64.r32(), src1.r32()); // zero-extend
            COMP->mov(rhs64.r32(), src2.r32());
          } else {
            COMP->movsxd(lhs64, src1.r32()); // sign-extend
            COMP->movsxd(rhs64, src2.r32());
          }
          COMP->imul(lhs64, rhs64);
          COMP->shr(lhs64, asmjit::Imm(32));
          x86::Gp dst = newGP32();
          COMP->mov(dst, lhs64.r32());
          TagStoreReg(instr->dest, dst);
        } break;
        case HIR::INT64_TYPE: {
          // 64-bit mul_hi requires 1-operand mul/imul with implicit rdx:rax.
          x86::Gp lo = newGP64();
          x86::Gp hi = newGP64();
          COMP->mov(lo, src1.r64());
          if (instr->flags & HIR::ARITHMETIC_UNSIGNED)
            COMP->mul(hi, lo, src2.r64());
          else
            COMP->imul(hi, lo, src2.r64());
          x86::Gp dst = newGP64();
          COMP->mov(dst, hi);
          TagStoreReg(instr->dest, dst);
        } break;
        default: UNREACHABLE_MSG("Unimplemented MUL_HI type."); return;
        }
      }

      //
      // Divide
      //

      REGISTER_EMITTER(OPCODE_DIV, Emit_DIV)
      static void Emit_DIV(x86CodeGenBackend *b, const HIR::Instr *instr) {
        switch (instr->dest->type) {
        case HIR::INT32_TYPE: {
          // Destination
          x86::Gp dst = newGP32();

          // Zero out dest
          COMP->xor_(dst, dst);
          
          // Get sources
          x86::Gp src1 = LoadValueGp(b, instr->src1.value);
          x86::Gp src2 = LoadValueGp(b, instr->src2.value);

          // Skip label
          Label skip = COMP->newLabel();
          
          x86::Gp eax = newGP32();
          x86::Gp edx = newGP32();
          
          COMP->mov(eax, src1.r32());
          // Test src2, skip if it's zero.
          COMP->test(src2.r32(), src2.r32());
          COMP->jz(skip);
          
          if (instr->flags & HIR::ARITHMETIC_UNSIGNED) {
            COMP->xor_(edx, edx);
            COMP->div(edx, eax, src2.r32());
          } else {
            COMP->cdq(edx, eax);
            COMP->idiv(edx, eax, src2.r32());
          }

          COMP->mov(dst, eax);

          COMP->bind(skip);
          // Store result
          TagStoreReg(instr->dest, dst);
        } break;
        case HIR::INT64_TYPE: {
          // Destination
          x86::Gp dst = newGP64();

          // Zero out dest
          COMP->xor_(dst, dst);

          // Get sources
          x86::Gp src1 = LoadValueGp(b, instr->src1.value);
          x86::Gp src2 = LoadValueGp(b, instr->src2.value);
          
          // Skip label
          Label skip = COMP->newLabel();

          // Test src2, skip if it's zero.
          COMP->test(src2, src2);
          COMP->jz(skip);

          x86::Gp rax = newGP64();
          x86::Gp rdx = newGP64();

          COMP->mov(rax, src1.r64());

          if (instr->flags & HIR::ARITHMETIC_UNSIGNED) {
            COMP->xor_(rdx, rdx);
            COMP->div(rdx, rax, src2.r64());
          } else {
            COMP->cqo(rdx, rax);
            COMP->idiv(rdx, rax, src2.r64());
          }

          COMP->mov(dst, rax);

          COMP->bind(skip);
          // Store result
          TagStoreReg(instr->dest, dst);
        } break;
        case HIR::FLOAT32_TYPE: {
          // Destination
          x86::Xmm dst = allocXmm();
          // Get sources
          x86::Xmm src1 = LoadValueXmm(b, instr->src1.value);
          x86::Xmm src2 = LoadValueXmm(b, instr->src2.value);

          COMP->vdivss(dst, src1, src2);
          // Store result
          TagStoreReg(instr->dest, dst);
        } break;
        case HIR::FLOAT64_TYPE: {
          // Destination
          x86::Xmm dst = allocXmm();
          // Get sources
          x86::Xmm src1 = LoadValueXmm(b, instr->src1.value);
          x86::Xmm src2 = LoadValueXmm(b, instr->src2.value);

          COMP->vdivsd(dst, src1, src2);
          // Store result
          TagStoreReg(instr->dest, dst);
        } break;
        case HIR::VEC128_TYPE: {
          // Destination
          x86::Xmm dst = allocXmm();
          // Get sources
          x86::Xmm src1 = LoadValueXmm(b, instr->src1.value);
          x86::Xmm src2 = LoadValueXmm(b, instr->src2.value);

          COMP->vdivps(dst, src1, src2);
          // Store result
          TagStoreReg(instr->dest, dst);
        } break;
        default: UNREACHABLE_MSG("Unimplemented DIV type."); return;
        }
      }

      //
      // Multiply Add
      //

      REGISTER_EMITTER(OPCODE_MUL_ADD, Emit_MUL_ADD)
        static void Emit_MUL_ADD(x86CodeGenBackend *b, const HIR::Instr *instr) {
        switch (instr->dest->type) {
        case HIR::FLOAT32_TYPE: {
          // Get sources
          x86::Xmm s1 = LoadValueXmm(b, instr->src1.value);
          x86::Xmm s2 = LoadValueXmm(b, instr->src2.value);
          x86::Xmm s3 = LoadValueXmm(b, instr->src3.value);
          // Operation
          COMP->vfmadd213ss(s1, s2, s3);
          // Store result
          TagStoreReg(instr->dest, s1);
        } break;
        case HIR::FLOAT64_TYPE: {
          // Get sources
          x86::Xmm s1 = LoadValueXmm(b, instr->src1.value);
          x86::Xmm s2 = LoadValueXmm(b, instr->src2.value);
          x86::Xmm s3 = LoadValueXmm(b, instr->src3.value);
          // Operation
          COMP->vfmadd213sd(s1, s2, s3);
          // Store result
          TagStoreReg(instr->dest, s1);
        } break;
        case HIR::VEC128_TYPE: {
          // Destination
          x86::Xmm dst = allocXmm();
          // Get sources
          x86::Xmm src1 = LoadValueXmm(b, instr->src1.value);
          x86::Xmm src2 = LoadValueXmm(b, instr->src2.value);
          x86::Xmm src3 = LoadValueXmm(b, instr->src3.value);
          // Operation
          COMP->vmulps(dst, src1, src2);
          COMP->vaddps(dst, dst, src3);
          // Store result
          TagStoreReg(instr->dest, dst);
        } break;
        default: UNREACHABLE_MSG("Unimplemented MUL_ADD type."); return;
        }
      }

      //
      // Multiply Subtract
      //

      REGISTER_EMITTER(OPCODE_MUL_SUB, Emit_MUL_SUB)
        static void Emit_MUL_SUB(x86CodeGenBackend *b, const HIR::Instr *instr) {
        switch (instr->dest->type) {
        case HIR::FLOAT32_TYPE: {
          // Get sources
          x86::Xmm s1 = LoadValueXmm(b, instr->src1.value);
          x86::Xmm s2 = LoadValueXmm(b, instr->src2.value);
          x86::Xmm s3 = LoadValueXmm(b, instr->src3.value);
          // Operation
          COMP->vfmsub213ss(s1, s2, s3);
          // Store result
          TagStoreReg(instr->dest, s1);
        } break;
        case HIR::FLOAT64_TYPE: {
          // Get sources
          x86::Xmm s1 = LoadValueXmm(b, instr->src1.value);
          x86::Xmm s2 = LoadValueXmm(b, instr->src2.value);
          x86::Xmm s3 = LoadValueXmm(b, instr->src3.value);
          // Operation
          COMP->vfmsub213sd(s1, s2, s3);
          // Store result
          TagStoreReg(instr->dest, s1);
        } break;
        case HIR::VEC128_TYPE: {
          // Get sources
          x86::Xmm s1 = LoadValueXmm(b, instr->src1.value);
          x86::Xmm s2 = LoadValueXmm(b, instr->src2.value);
          x86::Xmm s3 = LoadValueXmm(b, instr->src3.value);
          // Operation
          COMP->vfmsub213ps(s1, s2, s3);
          // Store result
          TagStoreReg(instr->dest, s1);
        } break;
        default: UNREACHABLE_MSG("Unimplemented MUL_SUB type."); return;
        }
      }

      //
      // Negation
      //

      REGISTER_EMITTER(OPCODE_NEG, Emit_NEG)
      static void Emit_NEG(x86CodeGenBackend *b, const HIR::Instr *instr) {
        switch (instr->dest->type) {
        case HIR::INT8_TYPE: {
          // Destination
          x86::Gp dst = newGP8();
          // Get sources
          x86::Gp src1 = LoadValueGp(b, instr->src1.value);
          // Operation
          COMP->mov(dst, src1.r8());
          COMP->neg(dst);
          // Store result
          TagStoreReg(instr->dest, dst);
        } break;
        case HIR::INT16_TYPE: {
          // Destination
          x86::Gp dst = newGP16();
          // Get sources
          x86::Gp src1 = LoadValueGp(b, instr->src1.value);
          // Operation
          COMP->mov(dst, src1.r16());
          COMP->neg(dst);
          // Store result
          TagStoreReg(instr->dest, dst);
        } break;
        case HIR::INT32_TYPE: {
          // Destination
          x86::Gp dst = newGP32();
          // Get sources
          x86::Gp src1 = LoadValueGp(b, instr->src1.value);
          // Operation
          COMP->mov(dst, src1.r32());
          COMP->neg(dst);
          // Store result
          TagStoreReg(instr->dest, dst);
        } break;
        case HIR::INT64_TYPE: {
          // Destination
          x86::Gp dst = newGP64();
          // Get sources
          x86::Gp src1 = LoadValueGp(b, instr->src1.value);
          // Operation
          COMP->mov(dst, src1.r64());
          COMP->neg(dst);
          // Store result
          TagStoreReg(instr->dest, dst);
        } break;
        case HIR::FLOAT32_TYPE: {
          // Destination
          x86::Xmm dst = allocXmm();
          // Get sources
          x86::Xmm src1 = LoadValueXmm(b, instr->src1.value);
          // Operation
          COMP->vxorps(dst, src1, LoadXmmConst(b, XMMSignMaskPS));
          // Store result
          TagStoreReg(instr->dest, dst);
        } break;
        case HIR::FLOAT64_TYPE: {
          // Destination
          x86::Xmm dst = allocXmm();
          // Get sources
          x86::Xmm src1 = LoadValueXmm(b, instr->src1.value);
          // Operation
          COMP->vxorpd(dst, src1, LoadXmmConst(b, XMMSignMaskPD));
          // Store result
          TagStoreReg(instr->dest, dst);
        } break;
        case HIR::VEC128_TYPE: {
          // Destination
          x86::Xmm dst = allocXmm();
          // Get sources
          x86::Xmm src1 = LoadValueXmm(b, instr->src1.value);
          // Operation
          COMP->vxorps(dst, src1, LoadXmmConst(b, XMMSignMaskPS));
          // Store result
          TagStoreReg(instr->dest, dst);
        } break;
        default: UNREACHABLE_MSG("Unimplemented NEG type."); return;
        }
      }

      //
      // Absolute
      //

      REGISTER_EMITTER(OPCODE_ABS, Emit_ABS)
        static void Emit_ABS(x86CodeGenBackend *b, const HIR::Instr *instr) {
        switch (instr->dest->type) {
        case HIR::FLOAT32_TYPE: {
          // Destination
          x86::Xmm dst = allocXmm();
          // Get sources
          x86::Xmm src1 = LoadValueXmm(b, instr->src1.value);
          // Operation
          COMP->vpand(dst, src1, LoadXmmConst(b, XMMAbsMaskPS));
          // Store result
          TagStoreReg(instr->dest, dst);
        } break;
        case HIR::FLOAT64_TYPE: {
          // Destination
          x86::Xmm dst = allocXmm();
          // Get sources
          x86::Xmm src1 = LoadValueXmm(b, instr->src1.value);
          // Operation
          COMP->vpand(dst, src1, LoadXmmConst(b, XMMAbsMaskPD));
          // Store result
          TagStoreReg(instr->dest, dst);
        } break;
        case HIR::VEC128_TYPE: {
          // Destination
          x86::Xmm dst = allocXmm();
          // Get sources
          x86::Xmm src1 = LoadValueXmm(b, instr->src1.value);
          // Operation
          COMP->vpand(dst, src1, LoadXmmConst(b, XMMAbsMaskPS));
          // Store result
          TagStoreReg(instr->dest, dst);
        } break;
        default: UNREACHABLE_MSG("Unimplemented ABS type."); return;
        }
      }

      //
      // Square Root
      //

      REGISTER_EMITTER(OPCODE_SQRT, Emit_SQRT)
        static void Emit_SQRT(x86CodeGenBackend *b, const HIR::Instr *instr) {
        switch (instr->dest->type) {
        case HIR::FLOAT32_TYPE: {
          // Destination
          x86::Xmm dst = allocXmm();
          // Get sources
          x86::Xmm src1 = LoadValueXmm(b, instr->src1.value);
          // Operation
          COMP->vsqrtss(dst, dst, src1);
          // Store result
          TagStoreReg(instr->dest, dst);
        } break;
        case HIR::FLOAT64_TYPE: {
          // Destination
          x86::Xmm dst = allocXmm();
          // Get sources
          x86::Xmm src1 = LoadValueXmm(b, instr->src1.value);
          // Operation
          COMP->vsqrtsd(dst, dst, src1);
          // Store result
          TagStoreReg(instr->dest, dst);
        } break;
        case HIR::VEC128_TYPE: {
          // Destination
          x86::Xmm dst = allocXmm();
          // Get sources
          x86::Xmm src1 = LoadValueXmm(b, instr->src1.value);
          // Operation
          COMP->vsqrtps(dst, src1);
          // Store result
          TagStoreReg(instr->dest, dst);
        } break;
        default: UNREACHABLE_MSG("Unimplemented SQRT type."); return;
        }
      }

      //
      // Reciprocal Square Root
      // 
      
      // Note: 
      // Altivec guarantees an error of < 1/4096 for vrsqrtefp while AVX only gives
      // < 1.5*2^-12 ≈ 1/2730 for vrsqrtps.

      REGISTER_EMITTER(OPCODE_RSQRT, Emit_RSQRT)
        static void Emit_RSQRT(x86CodeGenBackend *b, const HIR::Instr *instr) {
        switch (instr->dest->type) {
        case HIR::FLOAT32_TYPE: {
          // Destination
          x86::Xmm dst = allocXmm();
          // Temps
          x86::Xmm xmm0 = allocXmm();
          x86::Xmm xmm1 = allocXmm();
          // Get sources
          x86::Xmm src1 = LoadValueXmm(b, instr->src1.value);
          // Operation
          COMP->vmovaps(xmm0, LoadXmmConst(b, XMMOne));
          COMP->vsqrtss(xmm1, src1, src1);
          COMP->vdivss(dst, xmm0, xmm1);
          // Store result
          TagStoreReg(instr->dest, dst);
        } break;
        case HIR::FLOAT64_TYPE: {
          // Destination
          x86::Xmm dst = allocXmm();
          // Temps
          x86::Xmm xmm0 = allocXmm();
          x86::Xmm xmm1 = allocXmm();
          // Get sources
          x86::Xmm src1 = LoadValueXmm(b, instr->src1.value);
          // Operation
          COMP->vmovapd(xmm0, LoadXmmConst(b, XMMOnePD));
          COMP->vsqrtsd(xmm1, src1, src1);
          COMP->vdivsd(dst, xmm0, xmm1);
          // Store result
          TagStoreReg(instr->dest, dst);
        } break;
        case HIR::VEC128_TYPE: {
          // Destination
          x86::Xmm dst = allocXmm();
          // Temps
          x86::Xmm xmm0 = allocXmm();
          x86::Xmm xmm1 = allocXmm();
          // Get sources
          x86::Xmm src1 = LoadValueXmm(b, instr->src1.value);
          // Operation
          COMP->vmovaps(xmm0, LoadXmmConst(b, XMMOne));
          COMP->vsqrtps(xmm1, src1);
          COMP->vdivps(dst, xmm0, xmm1);
          // Store result
          TagStoreReg(instr->dest, dst);
        } break;
        default: UNREACHABLE_MSG("Unimplemented RSQRT type."); return;
        }
      }

      //
      // Reciprocal
      //

      // Note:
      // Altivec guarantees an error of < 1/4096 for vrefp while AVX only gives
      // < 1.5*2^-12 ≈ 1/2730 for rcpps. This breaks camp, horse and random event
      // spawning, breaks cactus collision as well as flickering grass in 5454082B

      REGISTER_EMITTER(OPCODE_RECIP, Emit_RECIP)
        static void Emit_RECIP(x86CodeGenBackend *b, const HIR::Instr *instr) {
        switch (instr->dest->type) {
        case HIR::FLOAT32_TYPE: {
          // Destination
          x86::Xmm dst = allocXmm();
          // Temps
          x86::Xmm xmm0 = allocXmm();
          // Get sources
          x86::Xmm src1 = LoadValueXmm(b, instr->src1.value);
          // Operation
          COMP->vmovaps(xmm0, LoadXmmConst(b, XMMOne));
          COMP->vdivss(dst, xmm0, src1);
          // Store result
          TagStoreReg(instr->dest, dst);
        } break;
        case HIR::FLOAT64_TYPE: {
          // Destination
          x86::Xmm dst = allocXmm();
          // Temps
          x86::Xmm xmm0 = allocXmm();
          // Get sources
          x86::Xmm src1 = LoadValueXmm(b, instr->src1.value);
          // Operation
          COMP->vmovapd(xmm0, LoadXmmConst(b, XMMOnePD));
          COMP->vdivsd(dst, xmm0, src1);
          // Store result
          TagStoreReg(instr->dest, dst);
        } break;
        case HIR::VEC128_TYPE: {
          // Destination
          x86::Xmm dst = allocXmm();
          // Temps
          x86::Xmm xmm0 = allocXmm();
          // Get sources
          x86::Xmm src1 = LoadValueXmm(b, instr->src1.value);
          // Operation
          COMP->vmovaps(xmm0, LoadXmmConst(b, XMMOne));
          COMP->vdivps(dst, xmm0, src1);
          // Store result
          TagStoreReg(instr->dest, dst);
        } break;
        default: UNREACHABLE_MSG("Unimplemented RECIP type."); return;
        }
      }

      //
      // Pow2
      //

      REGISTER_EMITTER(OPCODE_POW2, Emit_POW2)
        static void Emit_POW2(x86CodeGenBackend *b, const HIR::Instr *instr) {
        // Build the signature manually: __m128i(void*, __m128i, __m128i)
        // asmjit has no TypeIdOfT for __m128i, so use explicit TypeId values.
        asmjit::FuncSignature sig(
          asmjit::CallConvId::kCDecl,
          asmjit::FuncSignature::kNoVarArgs,
          asmjit::TypeId::kInt32x4,   // return: __m128i
          asmjit::TypeId::kUIntPtr,   // arg0:   void*
          asmjit::TypeId::kInt32x4);   // arg1:   __m128i

        switch (instr->dest->type) {
        case HIR::FLOAT32_TYPE: {
          // Destination
          x86::Xmm dst = allocXmm();
          // Get sources
          x86::Xmm src1 = LoadValueXmm(b, instr->src1.value);
          // Operation
          InvokeNode *invokeNode = nullptr;
          COMP->invoke(&invokeNode, asmjit::Imm(reinterpret_cast<uintptr_t>(EmulatePow2Float)), sig);
          invokeNode->setArg(0, Imm(0)); // void* ctx = nullptr
          invokeNode->setArg(1, src1);
          invokeNode->setRet(0, dst);
          // Store result
          TagStoreReg(instr->dest, dst);
        } break;
        case HIR::FLOAT64_TYPE: {
          // Destination
          x86::Xmm dst = allocXmm();
          // Get sources
          x86::Xmm src1 = LoadValueXmm(b, instr->src1.value);
          // Operation
          InvokeNode *invokeNode = nullptr;
          COMP->invoke(&invokeNode, asmjit::Imm(reinterpret_cast<uintptr_t>(EmulatePow2Double)), sig);
          invokeNode->setArg(0, Imm(0)); // void* ctx = nullptr
          invokeNode->setArg(1, src1);
          invokeNode->setRet(0, dst);
          // Store result
          TagStoreReg(instr->dest, dst);
        } break;
        case HIR::VEC128_TYPE: {
          // Destination
          x86::Xmm dst = allocXmm();
          // Get sources
          x86::Xmm src1 = LoadValueXmm(b, instr->src1.value);
          // Operation
          InvokeNode *invokeNode = nullptr;
          COMP->invoke(&invokeNode, asmjit::Imm(reinterpret_cast<uintptr_t>(EmulatePow2Vec)), sig);
          invokeNode->setArg(0, Imm(0)); // void* ctx = nullptr
          invokeNode->setArg(1, src1);
          invokeNode->setRet(0, dst);
          // Store result
          TagStoreReg(instr->dest, dst);
        } break;
        default: UNREACHABLE_MSG("Unimplemented RECIP type."); return;
        }
      }

      //
      // Log2
      //

      REGISTER_EMITTER(OPCODE_LOG2, Emit_LOG2)
        static void Emit_LOG2(x86CodeGenBackend *b, const HIR::Instr *instr) {
        // Build the signature manually: __m128i(void*, __m128i, __m128i)
        // asmjit has no TypeIdOfT for __m128i, so use explicit TypeId values.
        asmjit::FuncSignature sig(
          asmjit::CallConvId::kCDecl,
          asmjit::FuncSignature::kNoVarArgs,
          asmjit::TypeId::kInt32x4,   // return: __m128i
          asmjit::TypeId::kUIntPtr,   // arg0:   void*
          asmjit::TypeId::kInt32x4);   // arg1:   __m128i

        switch (instr->dest->type) {
        case HIR::FLOAT32_TYPE: {
          // Destination
          x86::Xmm dst = allocXmm();
          // Get sources
          x86::Xmm src1 = LoadValueXmm(b, instr->src1.value);
          // Operation
          InvokeNode *invokeNode = nullptr;
          COMP->invoke(&invokeNode, asmjit::Imm(reinterpret_cast<uintptr_t>(EmulateLog2Float)), sig);
          invokeNode->setArg(0, Imm(0)); // void* ctx = nullptr
          invokeNode->setArg(1, src1);
          invokeNode->setRet(0, dst);
          // Store result
          TagStoreReg(instr->dest, dst);
        } break;
        case HIR::FLOAT64_TYPE: {
          // Destination
          x86::Xmm dst = allocXmm();
          // Get sources
          x86::Xmm src1 = LoadValueXmm(b, instr->src1.value);
          // Operation
          InvokeNode *invokeNode = nullptr;
          COMP->invoke(&invokeNode, asmjit::Imm(reinterpret_cast<uintptr_t>(EmulateLog2Double)), sig);
          invokeNode->setArg(0, Imm(0)); // void* ctx = nullptr
          invokeNode->setArg(1, src1);
          invokeNode->setRet(0, dst);
          // Store result
          TagStoreReg(instr->dest, dst);
        } break;
        case HIR::VEC128_TYPE: {
          // Destination
          x86::Xmm dst = allocXmm();
          // Get sources
          x86::Xmm src1 = LoadValueXmm(b, instr->src1.value);
          // Operation
          InvokeNode *invokeNode = nullptr;
          COMP->invoke(&invokeNode, asmjit::Imm(reinterpret_cast<uintptr_t>(EmulateLog2Vec)), sig);
          invokeNode->setArg(0, Imm(0)); // void* ctx = nullptr
          invokeNode->setArg(1, src1);
          invokeNode->setRet(0, dst);
          // Store result
          TagStoreReg(instr->dest, dst);
        } break;
        default: UNREACHABLE_MSG("Unimplemented RECIP type."); return;
        }
      }

      //
      // And
      //

      REGISTER_EMITTER(OPCODE_AND, Emit_AND)
      static void Emit_AND(x86CodeGenBackend *b, const HIR::Instr *instr) {
        switch (instr->dest->type) {
        case HIR::INT8_TYPE: {
          // Destination
          x86::Gp dst = newGP8();
          // Get sources
          x86::Gp src1 = LoadValueGp(b, instr->src1.value);
          x86::Gp src2 = LoadValueGp(b, instr->src2.value);
          // Operation
          COMP->mov(dst, src1.r8());
          COMP->and_(dst, src2.r8());
          // Store result
          TagStoreReg(instr->dest, dst);
        } break;
        case HIR::INT16_TYPE: {
          // Destination
          x86::Gp dst = newGP16();
          // Get sources
          x86::Gp src1 = LoadValueGp(b, instr->src1.value);
          x86::Gp src2 = LoadValueGp(b, instr->src2.value);
          // Operation
          COMP->mov(dst, src1.r16());
          COMP->and_(dst, src2.r16());
          // Store result
          TagStoreReg(instr->dest, dst);
        } break;
        case HIR::INT32_TYPE: {
          // Destination
          x86::Gp dst = newGP32();
          // Get sources
          x86::Gp src1 = LoadValueGp(b, instr->src1.value);
          x86::Gp src2 = LoadValueGp(b, instr->src2.value);
          // Operation
          COMP->mov(dst, src1.r32());
          COMP->and_(dst, src2.r32());
          // Store result
          TagStoreReg(instr->dest, dst);
        } break;
        case HIR::INT64_TYPE: {
          // Destination
          x86::Gp dst = newGP64();
          // Get sources
          x86::Gp src1 = LoadValueGp(b, instr->src1.value);
          x86::Gp src2 = LoadValueGp(b, instr->src2.value);
          // Operation
          COMP->mov(dst, src1.r64());
          COMP->and_(dst, src2.r64());
          // Store result
          TagStoreReg(instr->dest, dst);
        } break;
        case HIR::VEC128_TYPE: {
          // Destination
          x86::Xmm dst = allocXmm();
          // Get sources
          x86::Xmm src1 = LoadValueXmm(b, instr->src1.value);
          x86::Xmm src2 = LoadValueXmm(b, instr->src2.value);
          // Operation
          COMP->vpand(dst, src1, src2);
          // Store result
          TagStoreReg(instr->dest, dst);
        } break;
        default: UNREACHABLE_MSG("Unimplemented AND type."); return;
        }
      }

      //
      // And Not
      //

      REGISTER_EMITTER(OPCODE_AND_NOT, Emit_AND_NOT)
        static void Emit_AND_NOT(x86CodeGenBackend *b, const HIR::Instr *instr) {
        switch (instr->dest->type) {
        case HIR::INT8_TYPE: {
          // Destination
          x86::Gp dst = newGP8();
          // Get sources
          x86::Gp src1 = LoadValueGp(b, instr->src1.value);
          x86::Gp src2 = LoadValueGp(b, instr->src2.value);
          // Operation
          COMP->mov(dst, src2.r8());  
          COMP->not_(dst); 
          COMP->and_(dst, src1.r8());
          // Store result
          TagStoreReg(instr->dest, dst);
        } break;
        case HIR::INT16_TYPE: {
          // Destination
          x86::Gp dst = newGP16();
          // Get sources
          x86::Gp src1 = LoadValueGp(b, instr->src1.value);
          x86::Gp src2 = LoadValueGp(b, instr->src2.value);
          // Operation
          COMP->mov(dst, src2.r16());
          COMP->not_(dst);
          COMP->and_(dst, src1.r16());
          // Store result
          TagStoreReg(instr->dest, dst);
        } break;
        case HIR::INT32_TYPE: {
          // Destination
          x86::Gp dst = newGP32();
          // Get sources
          x86::Gp src1 = LoadValueGp(b, instr->src1.value);
          x86::Gp src2 = LoadValueGp(b, instr->src2.value);
          // Operation
          COMP->mov(dst, src2.r32());
          COMP->not_(dst);
          COMP->and_(dst, src1.r32());
          // Store result
          TagStoreReg(instr->dest, dst);
        } break;
        case HIR::INT64_TYPE: {
          // Destination
          x86::Gp dst = newGP64();
          // Get sources
          x86::Gp src1 = LoadValueGp(b, instr->src1.value);
          x86::Gp src2 = LoadValueGp(b, instr->src2.value);
          // Operation
          COMP->mov(dst, src2.r64());
          COMP->not_(dst);
          COMP->and_(dst, src1.r64());
          // Store result
          TagStoreReg(instr->dest, dst);
        } break;
        case HIR::VEC128_TYPE: {
          // Destination
          x86::Xmm dst = allocXmm();
          // Get sources
          x86::Xmm src1 = LoadValueXmm(b, instr->src1.value);
          x86::Xmm src2 = LoadValueXmm(b, instr->src2.value);
          // Operation
          COMP->vpandn(dst, src2, src1);
          // Store result
          TagStoreReg(instr->dest, dst);
        } break;
        default: UNREACHABLE_MSG("Unimplemented AND_NOT type."); return;
        }
      }

      //
      // OR
      //
      
      REGISTER_EMITTER(OPCODE_OR, Emit_OR)
      static void Emit_OR(x86CodeGenBackend *b, const HIR::Instr *instr) {
        switch (instr->dest->type) {
        case HIR::INT8_TYPE: {
          // Destination
          x86::Gp dst = newGP8();
          // Get sources
          x86::Gp src1 = LoadValueGp(b, instr->src1.value);
          x86::Gp src2 = LoadValueGp(b, instr->src2.value);
          // Operation
          COMP->mov(dst, src1.r8());
          COMP->or_(dst, src2.r8());
          // Store result
          TagStoreReg(instr->dest, dst);
        } break;
        case HIR::INT16_TYPE: {
          // Destination
          x86::Gp dst = newGP16();
          // Get sources
          x86::Gp src1 = LoadValueGp(b, instr->src1.value);
          x86::Gp src2 = LoadValueGp(b, instr->src2.value);
          // Operation
          COMP->mov(dst, src1.r16());
          COMP->or_(dst, src2.r16());
          // Store result
          TagStoreReg(instr->dest, dst);
        } break;
        case HIR::INT32_TYPE: {
          // Destination
          x86::Gp dst = newGP32();
          // Get sources
          x86::Gp src1 = LoadValueGp(b, instr->src1.value);
          x86::Gp src2 = LoadValueGp(b, instr->src2.value);
          // Operation
          COMP->mov(dst, src1.r32());
          COMP->or_(dst, src2.r32());
          // Store result
          TagStoreReg(instr->dest, dst);
        } break;
        case HIR::INT64_TYPE: {
          // Destination
          x86::Gp dst = newGP64();
          // Get sources
          x86::Gp src1 = LoadValueGp(b, instr->src1.value);
          x86::Gp src2 = LoadValueGp(b, instr->src2.value);
          // Operation
          COMP->mov(dst, src1.r64());
          COMP->or_(dst, src2.r64());
          // Store result
          TagStoreReg(instr->dest, dst);
        } break;
        case HIR::VEC128_TYPE: {
          // Destination
          x86::Xmm dst = allocXmm();
          // Get sources
          x86::Xmm src1 = LoadValueXmm(b, instr->src1.value);
          x86::Xmm src2 = LoadValueXmm(b, instr->src2.value);
          // Operation
          COMP->vpor(dst, src1, src2);
          // Store result
          TagStoreReg(instr->dest, dst);
        } break;
        default: UNREACHABLE_MSG("Unimplemented OR type."); return;
        }
      }

      //
      // XOR
      //

      REGISTER_EMITTER(OPCODE_XOR, Emit_XOR)
      static void Emit_XOR(x86CodeGenBackend *b, const HIR::Instr *instr) {
        switch (instr->dest->type) {
        case HIR::INT8_TYPE: {
          // Destination
          x86::Gp dst = newGP8();
          // Get sources
          x86::Gp src1 = LoadValueGp(b, instr->src1.value);
          x86::Gp src2 = LoadValueGp(b, instr->src2.value);
          // Operation
          COMP->mov(dst, src1.r8());
          COMP->xor_(dst, src2.r8());
          // Store result
          TagStoreReg(instr->dest, dst);
        } break;
        case HIR::INT16_TYPE: {
          // Destination
          x86::Gp dst = newGP16();
          // Get sources
          x86::Gp src1 = LoadValueGp(b, instr->src1.value);
          x86::Gp src2 = LoadValueGp(b, instr->src2.value);
          // Operation
          COMP->mov(dst, src1.r16());
          COMP->xor_(dst, src2.r16());
          // Store result
          TagStoreReg(instr->dest, dst);
        } break;
        case HIR::INT32_TYPE: {
          // Destination
          x86::Gp dst = newGP32();
          // Get sources
          x86::Gp src1 = LoadValueGp(b, instr->src1.value);
          x86::Gp src2 = LoadValueGp(b, instr->src2.value);
          // Operation
          COMP->mov(dst, src1.r32());
          COMP->xor_(dst, src2.r32());
          // Store result
          TagStoreReg(instr->dest, dst);
        } break;
        case HIR::INT64_TYPE: {
          // Destination
          x86::Gp dst = newGP64();
          // Get sources
          x86::Gp src1 = LoadValueGp(b, instr->src1.value);
          x86::Gp src2 = LoadValueGp(b, instr->src2.value);
          // Operation
          COMP->mov(dst, src1.r64());
          COMP->xor_(dst, src2.r64());
          // Store result
          TagStoreReg(instr->dest, dst);
        } break;
        case HIR::VEC128_TYPE: {
          // Destination
          x86::Xmm dst = allocXmm();
          // Get sources
          x86::Xmm src1 = LoadValueXmm(b, instr->src1.value);
          x86::Xmm src2 = LoadValueXmm(b, instr->src2.value);
          // Operation
          COMP->vpxor(dst, src1, src2);
          // Store result
          TagStoreReg(instr->dest, dst);
        } break;
        default: UNREACHABLE_MSG("Unimplemented XOR type."); return;
        }
      }

      //
      // NOT
      //

      REGISTER_EMITTER(OPCODE_NOT, Emit_NOT)
      static void Emit_NOT(x86CodeGenBackend *b, const HIR::Instr *instr) {
        switch (instr->dest->type) {
        case HIR::INT8_TYPE: {
          // Destination
          x86::Gp dst = newGP8();
          // Get sources
          x86::Gp src1 = LoadValueGp(b, instr->src1.value);
          // Operation
          COMP->mov(dst, src1.r8());
          COMP->not_(dst);
          // Store result
          TagStoreReg(instr->dest, dst);
        } break;
        case HIR::INT16_TYPE: {
          // Destination
          x86::Gp dst = newGP16();
          // Get sources
          x86::Gp src1 = LoadValueGp(b, instr->src1.value);
          // Operation
          COMP->mov(dst, src1.r16());
          COMP->not_(dst);
          // Store result
          TagStoreReg(instr->dest, dst);
        } break;
        case HIR::INT32_TYPE: {
          // Destination
          x86::Gp dst = newGP32();
          // Get sources
          x86::Gp src1 = LoadValueGp(b, instr->src1.value);
          // Operation
          COMP->mov(dst, src1.r32());
          COMP->not_(dst);
          // Store result
          TagStoreReg(instr->dest, dst);
        } break;
        case HIR::INT64_TYPE: {
          // Destination
          x86::Gp dst = newGP64();
          // Get sources
          x86::Gp src1 = LoadValueGp(b, instr->src1.value);
          // Operation
          COMP->mov(dst, src1.r64());
          COMP->not_(dst);
          // Store result
          TagStoreReg(instr->dest, dst);
        } break;
        case HIR::VEC128_TYPE: {
          // Destination
          x86::Xmm dst = allocXmm();
          // Get sources
          x86::Xmm src1 = LoadValueXmm(b, instr->src1.value);
          // Operation
          COMP->vpxor(dst, src1, LoadXmmConst(b, XMMFFFF));
          // Store result
          TagStoreReg(instr->dest, dst);
        } break;
        default: UNREACHABLE_MSG("Unimplemented NOT type."); return;
        }
      }
      
      //
      // Shift Left
      //

      REGISTER_EMITTER(OPCODE_SHL, Emit_SHL)
      static void Emit_SHL(x86CodeGenBackend *b, const HIR::Instr *instr) {
        switch (instr->dest->type) {
        case HIR::INT8_TYPE: {
          // Destination
          x86::Gp dst = newGP8();
          // Get sources
          x86::Gp src1 = LoadValueGp(b, instr->src1.value);
          x86::Gp src2 = LoadValueGp(b, instr->src2.value);
          // Operation
          COMP->mov(dst, src1.r8());
          COMP->shl(dst, src2.r8());
          // Store result
          TagStoreReg(instr->dest, dst);
        } break;
        case HIR::INT16_TYPE: {
          // Destination
          x86::Gp dst = newGP16();
          // Get sources
          x86::Gp src1 = LoadValueGp(b, instr->src1.value);
          x86::Gp src2 = LoadValueGp(b, instr->src2.value);
          // Operation
          COMP->mov(dst, src1.r16());
          COMP->shl(dst, src2.r8());
          // Store result
          TagStoreReg(instr->dest, dst);
        } break;
        case HIR::INT32_TYPE: {
          // Destination
          x86::Gp dst = newGP32();
          // Get sources
          x86::Gp src1 = LoadValueGp(b, instr->src1.value);
          x86::Gp src2 = LoadValueGp(b, instr->src2.value);
          // Operation
          COMP->mov(dst, src1.r32());
          COMP->shl(dst, src2.r8());
          // Store result
          TagStoreReg(instr->dest, dst);
        } break;
        case HIR::INT64_TYPE: {
          // Destination
          x86::Gp dst = newGP64();
          // Get sources
          x86::Gp src1 = LoadValueGp(b, instr->src1.value);
          x86::Gp src2 = LoadValueGp(b, instr->src2.value);
          // Operation
          COMP->mov(dst, src1.r64());
          COMP->shl(dst, src2.r8());
          // Store result
          TagStoreReg(instr->dest, dst);
        } break;
        case HIR::VEC128_TYPE: {
          // Destination
          x86::Xmm dst = allocXmm();
          // Get sources
          x86::Xmm src1 = LoadValueXmm(b, instr->src1.value);
          x86::Xmm src2 = LoadValueXmm(b, instr->src2.value);
          // Operation
          // Build the signature manually: __m128i(void*, __m128i, __m128i)
          // asmjit has no TypeIdOfT for __m128i, so use explicit TypeId values.
          asmjit::FuncSignature sig(
            asmjit::CallConvId::kCDecl,
            asmjit::FuncSignature::kNoVarArgs,
            asmjit::TypeId::kInt32x4,   // return: __m128i
            asmjit::TypeId::kUIntPtr,   // arg0:   void*
            asmjit::TypeId::kInt32x4,   // arg1:   __m128i
            asmjit::TypeId::kUInt8);    // arg2:   unsigned char

          asmjit::InvokeNode *invokeNode = nullptr;
          COMP->invoke(&invokeNode, asmjit::Imm(reinterpret_cast<uintptr_t>(EmulateShlV128)), sig);
          invokeNode->setArg(0, asmjit::Imm(0)); // void* ctx = nullptr
          invokeNode->setArg(1, src1);
          invokeNode->setArg(2, src2);
          invokeNode->setRet(0, dst);
          // Store result
          TagStoreReg(instr->dest, dst);
        } break;
        default: UNREACHABLE_MSG("Unimplemented SHL type."); return;
        }
      }

      //
      // Shift Right
      //
      
      REGISTER_EMITTER(OPCODE_SHR, Emit_SHR)
      static void Emit_SHR(x86CodeGenBackend *b, const HIR::Instr *instr) {
        switch (instr->dest->type) {
        case HIR::INT8_TYPE: {
          // Destination
          x86::Gp dst = newGP8();
          // Get sources
          x86::Gp src1 = LoadValueGp(b, instr->src1.value);
          x86::Gp src2 = LoadValueGp(b, instr->src2.value);
          // Operation
          COMP->mov(dst, src1.r8());
          COMP->shr(dst, src2.r8());
          // Store result
          TagStoreReg(instr->dest, dst);
        } break;
        case HIR::INT16_TYPE: {
          // Destination
          x86::Gp dst = newGP16();
          // Get sources
          x86::Gp src1 = LoadValueGp(b, instr->src1.value);
          x86::Gp src2 = LoadValueGp(b, instr->src2.value);
          // Operation
          COMP->mov(dst, src1.r16());
          COMP->shr(dst, src2.r8());
          // Store result
          TagStoreReg(instr->dest, dst);
        } break;
        case HIR::INT32_TYPE: {
          // Destination
          x86::Gp dst = newGP32();
          // Get sources
          x86::Gp src1 = LoadValueGp(b, instr->src1.value);
          x86::Gp src2 = LoadValueGp(b, instr->src2.value);
          // Operation
          COMP->mov(dst, src1.r32());
          COMP->shr(dst, src2.r8());
          // Store result
          TagStoreReg(instr->dest, dst);
        } break;
        case HIR::INT64_TYPE: {
          // Destination
          x86::Gp dst = newGP64();
          // Get sources
          x86::Gp src1 = LoadValueGp(b, instr->src1.value);
          x86::Gp src2 = LoadValueGp(b, instr->src2.value);
          // Operation
          COMP->mov(dst, src1.r64());
          COMP->shr(dst, src2.r8());
          // Store result
          TagStoreReg(instr->dest, dst);
        } break;
        case HIR::VEC128_TYPE: {
          // Destination
          x86::Xmm dst = allocXmm();
          // Get sources
          x86::Xmm src1 = LoadValueXmm(b, instr->src1.value);
          x86::Xmm src2 = LoadValueXmm(b, instr->src2.value);
          // Operation
          // Build the signature manually: __m128i(void*, __m128i, __m128i)
          // asmjit has no TypeIdOfT for __m128i, so use explicit TypeId values.
          asmjit::FuncSignature sig(
            asmjit::CallConvId::kCDecl,
            asmjit::FuncSignature::kNoVarArgs,
            asmjit::TypeId::kInt32x4,   // return: __m128i
            asmjit::TypeId::kUIntPtr,   // arg0:   void*
            asmjit::TypeId::kInt32x4,   // arg1:   __m128i
            asmjit::TypeId::kUInt8);    // arg2:   uint8_t

          asmjit::InvokeNode *invokeNode = nullptr;
          COMP->invoke(&invokeNode, asmjit::Imm(reinterpret_cast<uintptr_t>(EmulateShrV128)), sig);
          invokeNode->setArg(0, asmjit::Imm(0)); // void* ctx = nullptr
          invokeNode->setArg(1, src1);
          invokeNode->setArg(2, src2);
          invokeNode->setRet(0, dst);
          // Store result
          TagStoreReg(instr->dest, dst);
        } break;
        default: UNREACHABLE_MSG("Unimplemented SHR type."); return;
        }
      }

      //
      // Shift Right Arithmetic
      //
      
      REGISTER_EMITTER(OPCODE_SHA, Emit_SHA)
      static void Emit_SHA(x86CodeGenBackend *b, const HIR::Instr *instr) {
        switch (instr->dest->type) {
        case HIR::INT8_TYPE: {
          // Destination
          x86::Gp dst = newGP8();
          // Get sources
          x86::Gp src1 = LoadValueGp(b, instr->src1.value);
          x86::Gp src2 = LoadValueGp(b, instr->src2.value);
          // Operation
          COMP->mov(dst, src1.r8());
          COMP->sar(dst, src2.r8());
          // Store result
          TagStoreReg(instr->dest, dst);
        } break;
        case HIR::INT16_TYPE: {
          // Destination
          x86::Gp dst = newGP16();
          // Get sources
          x86::Gp src1 = LoadValueGp(b, instr->src1.value);
          x86::Gp src2 = LoadValueGp(b, instr->src2.value);
          // Operation
          COMP->mov(dst, src1.r16());
          COMP->sar(dst, src2.r8());
          // Store result
          TagStoreReg(instr->dest, dst);
        } break;
        case HIR::INT32_TYPE: {
          // Destination
          x86::Gp dst = newGP32();
          // Get sources
          x86::Gp src1 = LoadValueGp(b, instr->src1.value);
          x86::Gp src2 = LoadValueGp(b, instr->src2.value);
          // Operation
          COMP->mov(dst, src1.r32());
          COMP->sar(dst, src2.r8());
          // Store result
          TagStoreReg(instr->dest, dst);
        } break;
        case HIR::INT64_TYPE: {
          // Destination
          x86::Gp dst = newGP64();
          // Get sources
          x86::Gp src1 = LoadValueGp(b, instr->src1.value);
          x86::Gp src2 = LoadValueGp(b, instr->src2.value);
          // Operation
          COMP->mov(dst, src1.r64());
          COMP->sar(dst, src2.r8());
          // Store result
          TagStoreReg(instr->dest, dst);
        } break;
        default: UNREACHABLE_MSG("Unimplemented SHR type."); return;
        }
      }

      //
      // Rotate Left
      //

      REGISTER_EMITTER(OPCODE_ROTATE_LEFT, Emit_ROTATE_LEFT)
      static void Emit_ROTATE_LEFT(x86CodeGenBackend *b, const HIR::Instr *instr) {
        switch (instr->dest->type) {
        case HIR::INT8_TYPE: {
          // Destination
          x86::Gp dst = newGP8();
          // Get sources
          x86::Gp src1 = LoadValueGp(b, instr->src1.value);
          x86::Gp src2 = LoadValueGp(b, instr->src2.value);
          // Operation
          COMP->mov(dst, src1.r8());
          COMP->rol(dst, src2.r8());
          // Store result
          TagStoreReg(instr->dest, dst);
        } break;
        case HIR::INT16_TYPE: {
          // Destination
          x86::Gp dst = newGP16();
          // Get sources
          x86::Gp src1 = LoadValueGp(b, instr->src1.value);
          x86::Gp src2 = LoadValueGp(b, instr->src2.value);
          // Operation
          COMP->mov(dst, src1.r16());
          COMP->rol(dst, src2.r8());
          // Store result
          TagStoreReg(instr->dest, dst);
        } break;
        case HIR::INT32_TYPE: {
          // Destination
          x86::Gp dst = newGP32();
          // Get sources
          x86::Gp src1 = LoadValueGp(b, instr->src1.value);
          x86::Gp src2 = LoadValueGp(b, instr->src2.value);
          // Operation
          COMP->mov(dst, src1.r32());
          COMP->rol(dst, src2.r8());
          // Store result
          TagStoreReg(instr->dest, dst);
        } break;
        case HIR::INT64_TYPE: {
          // Destination
          x86::Gp dst = newGP64();
          // Get sources
          x86::Gp src1 = LoadValueGp(b, instr->src1.value);
          x86::Gp src2 = LoadValueGp(b, instr->src2.value);
          // Operation
          COMP->mov(dst, src1.r64());
          COMP->rol(dst, src2.r8());
          // Store result
          TagStoreReg(instr->dest, dst);
        } break;
        default: UNREACHABLE_MSG("Unimplemented ROTATE_LEFT type."); return;
        }
      }

      //
      // Type Conversions
      //

      // BYTE_SWAP: dest = bswap(src1)
      REGISTER_EMITTER(OPCODE_BYTE_SWAP, Emit_BYTE_SWAP)
      static void Emit_BYTE_SWAP(x86CodeGenBackend *b, const HIR::Instr *instr) {
        switch (instr->dest->type) {
        case HIR::INT16_TYPE: {
          // Destination
          x86::Gp dst = newGP16();
          // Get sources
          x86::Gp src1 = LoadValueGp(b, instr->src1.value);
          COMP->mov(dst, src1.r16());
          COMP->rol(dst, asmjit::Imm(8)); // bswap16 = rol16 by 8
          TagStoreReg(instr->dest, dst);
        } break;
        case HIR::INT32_TYPE: {
          // Destination
          x86::Gp dst = newGP32();
          // Get sources
          x86::Gp src1 = LoadValueGp(b, instr->src1.value);
          COMP->mov(dst, src1.r32());
          COMP->bswap(dst);
          TagStoreReg(instr->dest, dst);
        } break;
        case HIR::INT64_TYPE: {
          // Destination
          x86::Gp dst = newGP64();
          // Get sources
          x86::Gp src1 = LoadValueGp(b, instr->src1.value);
          COMP->mov(dst, src1.r64());
          COMP->bswap(dst);
          TagStoreReg(instr->dest, dst);
        } break;
        case HIR::VEC128_TYPE: {
          // Destination
          x86::Xmm dst = allocXmm();
          // Get sources
          x86::Xmm src1 = LoadValueXmm(b, instr->src1.value);
          COMP->vpshufb(dst, src1, LoadXmmConst(b, XMMByteSwapMask));
          TagStoreReg(instr->dest, dst);
          return;
        }  
        default: UNREACHABLE_MSG("Unimplemented BYTE_SWAP type."); return;
        }
      }

      //
      // Count Leading Zeroes
      //

      REGISTER_EMITTER(OPCODE_CNTLZ, Emit_CNTLZ)
      static void Emit_CNTLZ(x86CodeGenBackend *b, const HIR::Instr *instr) {
        // Sources
        x86::Gp src = LoadValueGp(b, instr->src1.value);
        // Destination
        x86::Gp dst = newGP8();
        x86::Gp tmp = newGP32();

        switch (instr->src1.value->type) {
        case HIR::INT32_TYPE:
          COMP->lzcnt(tmp.r32(), src.r32());
          break;
        case HIR::INT64_TYPE:
          COMP->lzcnt(tmp.r64(), src.r64());
          break;
        default: UNREACHABLE_MSG("Unimplemented LZCNT type."); return;
        }
        COMP->mov(dst, tmp.r8());
        TagStoreReg(instr->dest, dst);
      }

      //
      // Set Rounding Mode
      //

      REGISTER_EMITTER(OPCODE_SET_ROUNDING_MODE, Emit_SET_ROUNDING_MODE)
      static void Emit_SET_ROUNDING_MODE(x86CodeGenBackend *b, const HIR::Instr *instr) {
        // MXCST Table holding correct values that map to PPC's FPSCR[RM]
        static const u32 mxcsrTable[] = { 0x1F80, 0x7F80, 0x5F80, 0x3F80, 0x9F80, 0xFF80, 0xDF80, 0xBF80, };
        // New Mode
        x86::Gp mode = LoadValueGp(b, instr->src1.value);
        // Table address ptr
        x86::Gp tableAddr = newGP64();
        // Get table address
        COMP->mov(tableAddr, asmjit::Imm(reinterpret_cast<uintptr_t>(mxcsrTable)));
        // Get correct mapping based on index and set mxcsr
        x86::Gp tmp = newGP64();
        COMP->mov(tmp, mode);
        COMP->and_(tmp, Imm(0x7));
        COMP->imul(tmp, Imm(0x4));
        COMP->add(tableAddr, tmp);
        COMP->vldmxcsr(x86::ptr(tableAddr));
      }

    }
  }
}

#endif
