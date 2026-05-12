/***************************************************************/
/* Copyright 2026 Xenon Emulator Project. All rights reserved. */
/***************************************************************/

#pragma once

#include <cstring>

#include "Core/XCPU/JIT/HIR/Value.h"
#include "asmjit/x86.h"

namespace Xe {
  namespace XCPU {
    namespace JIT {

//
// Allocates a new general purpose x86 register
//
#define newGP64()  b->GetCompiler()->newGpq()
#define newGP32()  b->GetCompiler()->newGpd()
#define newGP16()  b->GetCompiler()->newGpw()
#define newGP8()   b->GetCompiler()->newGpb()
#define newGPptr() b->GetCompiler()->newGpz()

//
// Pointer Helpers
//

#define COMP                b->GetCompiler()
#define GPRPtr(x)           b->GetThreadContext()->array(&sPPUThread::GPR).Ptr(x)
#define SPRStruct(x)        b->GetThreadContext()->substruct(&sPPUThread::SPR).substruct(&sPPUThreadSPRs::x)
#define SPRPtr(x)           b->GetThreadContext()->substruct(&sPPUThread::SPR).scalar(&sPPUThreadSPRs::x)
#define SharedSPRStruct(x)  b->ppeState->substruct(&sPPEState::SPR).substruct(&sPPESPRs::x)
#define SharedSPRPtr(x)     b->ppeState->substruct(&sPPEState::SPR).scalar(&sPPUGlobalSPRs::x)
#define CRValPtr()          b->GetThreadContext()->scalar(&sPPUThread::CR)
#define CIAPtr()            b->GetThreadContext()->scalar(&sPPUThread::CIA)
#define NIAPtr()            b->GetThreadContext()->scalar(&sPPUThread::NIA)
#define EXPtr()             b->GetThreadContext()->scalar(&sPPUThread::exceptReg)
#define LRPtr()             SPRPtr(LR)

      //
      // Value <-> Virtual Register helpers.
      // Packs the asmjit register signature (u32) and id (u32) into the
      // Value::tag pointer so that downstream emitters can recover the
      // full virtual register from a prior instruction's result.
      //

      // Store a virtual register into a Value's tag.
      inline void TagStoreReg(HIR::Value *val, const asmjit::BaseReg &reg) {
        u64 packed = (u64(reg.signature()._bits) << 32) | u64(reg.id());
        val->tag = reinterpret_cast<void *>(packed);
      }

      // Recover a GP virtual register from a Value's tag.
      inline asmjit::x86::Gp TagLoadGp(const HIR::Value *val) {
        u64 packed = reinterpret_cast<u64>(val->tag);
        asmjit::x86::Gp reg;
        reg.setSignature(static_cast<u32>(packed >> 32));
        reg.setId(static_cast<u32>(packed & 0xFFFFFFFF));
        return reg;
      }

      // Recover an XMM virtual register from a Value's tag.
      inline asmjit::x86::Xmm TagLoadXmm(const HIR::Value *val) {
        u64 packed = reinterpret_cast<u64>(val->tag);
        asmjit::x86::Xmm reg;
        reg.setSignature(static_cast<u32>(packed >> 32));
        reg.setId(static_cast<u32>(packed & 0xFFFFFFFF));
        return reg;
      }

      // Load a HIR Value into an XMM virtual register.
      // Handles both constant values (materializes from memory) and SSA
      // values produced by prior instructions (recovers the virtual register).
      inline asmjit::x86::Xmm LoadValueXmm(x86CodeGenBackend *b, HIR::Value *val) {
        auto *comp = b->GetCompiler();
        if (val->IsConstant()) {
          switch (val->type) {
          case HIR::FLOAT32_TYPE: {
            auto reg = comp->newXmm();
            auto tmp = comp->newGpd();
            u32 bits;
            std::memcpy(&bits, &val->constant.f32, sizeof(bits));
            comp->mov(tmp, asmjit::Imm(bits));
            comp->vmovd(reg, tmp);
            return reg;
          }
          case HIR::FLOAT64_TYPE: {
            auto reg = comp->newXmm();
            auto tmp = comp->newGpq();
            u64 bits;
            std::memcpy(&bits, &val->constant.f64, sizeof(bits));
            comp->mov(tmp, asmjit::Imm(static_cast<s64>(bits)));
            comp->vmovq(reg, tmp);
            return reg;
          }
          case HIR::VEC128_TYPE: {
            auto reg = comp->newXmm();
            // Materialize 128-bit constant via two 64-bit halves
            auto tmpLo = comp->newGpq();
            auto tmpHi = comp->newGpq();
            u64 lo = val->constant.v128.qword[0];
            u64 hi = val->constant.v128.qword[1];
            comp->mov(tmpLo, asmjit::Imm(static_cast<s64>(lo)));
            comp->vmovq(reg, tmpLo);
            if (hi != 0) {
              auto regHi = comp->newXmm();
              comp->mov(tmpHi, asmjit::Imm(static_cast<s64>(hi)));
              comp->vmovq(regHi, tmpHi);
              comp->vpunpcklqdq(reg, reg, regHi);
            }
            return reg;
          }
          default:
            break;
          }
        }
        return TagLoadXmm(val);
      }

      // Allocates a new XMM virtual register
      #define allocXmm()   b->GetCompiler()->newXmm()

      // Load a HIR Value into a GP virtual register.
      // Handles both constant values (materializes an immediate) and SSA
      // values produced by prior instructions (recovers the virtual register).
      inline asmjit::x86::Gp LoadValueGp(x86CodeGenBackend *b, HIR::Value *val) {
        if (val->IsConstant()) {
          switch (val->type) {
          case HIR::INT8_TYPE: {
            auto reg = b->GetCompiler()->newGpb();
            b->GetCompiler()->mov(reg, asmjit::Imm(val->constant.u8));
            return reg;
          }
          case HIR::INT16_TYPE: {
            auto reg = b->GetCompiler()->newGpw();
            b->GetCompiler()->mov(reg, asmjit::Imm(val->constant.u16));
            return reg;
          }
          case HIR::INT32_TYPE: {
            auto reg = b->GetCompiler()->newGpd();
            b->GetCompiler()->mov(reg, asmjit::Imm(val->constant.u32));
            return reg;
          }
          case HIR::INT64_TYPE: {
            auto reg = b->GetCompiler()->newGpq();
            b->GetCompiler()->mov(reg, asmjit::Imm(val->constant.i64));
            return reg;
          }
          default:
            break;
          }
        }
        return TagLoadGp(val);
      }

      // Helper: load a static Vector128 constant into an XMM register.
      static x86::Xmm LoadXmmConst(x86CodeGenBackend *b, const Base::Vector128 &v) {
        auto *c = b->GetCompiler();
        x86::Xmm reg = c->newXmm();
        x86::Gp addr = c->newGpq();
        c->mov(addr, asmjit::Imm(reinterpret_cast<uintptr_t>(&v)));
        c->vmovaps(reg, x86::ptr(addr));
        return reg;
      }
    }
  }
}

