/***************************************************************/
/* Copyright 2026 Xenon Emulator Project. All rights reserved. */
/***************************************************************/

#include "Base/Arch.h"

#if defined(ARCH_X86) || defined(ARCH_X86_64)

#include <cstring>

#include "Base/Logging/Log.h"
#include "Base/FastTranslationCache.h"
#include "Core/XCPU/PPU/PPCInternal.h"
#include "Core/XCPU/JIT/x86VectorConstants.h"
#include "Core/XCPU/JIT/Backend/X86Backend/x86CodeGenBackend.h"
#include "Core/XCPU/JIT/Backend/X86Backend/x86CodeGenHelpers.h"

#include "Core/XCPU/Interpreter/PPCInterpreter.h"

namespace Xe {
  namespace XCPU {
    namespace JIT {

      //
      // Generic LOAD / LOAD_OFFSET / STORE / STORE_OFFSET
      // These go through the full MMU Read/Write routines.
      //

      // MOVBE (move data after swapping bytes) collapses load+bswap (or
      // store+bswap) into a single uop on Haswell+, Atom, and Zen. Detected
      // once at startup; the codegen below picks between movbe and the
      // mov+rol/mov+bswap fallback at emission time.
      static const bool kHasMOVBE = []() noexcept {
        return asmjit::CpuInfo::host().features().x86().hasMOVBE();
      }();

      // Result of an inline fastDataCache probe.
      // On the hit fall-through, hostPtr addresses the guest byte directly.
      // On miss, control jumps to slowPath. After the slow-path code emits
      // its result it should jmp to done. The caller binds done.
      // |reused| is true when this probe consumed the previous probe's
      // hostPtr instead of emitting a fresh cache lookup.
      struct FastCacheProbe {
        Label    slowPath;
        Label    done;
        x86::Gp  eaQ;     // 64-bit working copy of EA
        x86::Gp  hostPtr; // valid in hit path; 0 on slow-path fall-through
        bool     reused = false;
      };

      // Emits a notify to RAM on writes
      static void EmitNotifyWrite(x86CodeGenBackend *b, x86::Gp hostPtr, u32 byteCount) {
        InvokeNode *notify = nullptr;
        COMP->invoke(&notify, imm((void *)PPCInterpreter::JITNotifyWrite),
          FuncSignature::build<void, u8 *, u32>());
        notify->setArg(0, hostPtr);
        notify->setArg(1, asmjit::Imm(byteCount));
      }

      // Caller invokes this immediately after binding probe.slowPath.
      // Stamps probe.hostPtr with a 0 sentinel so a *future* same-EA dedupe
      // attempt can detect that the previous access took the slow path and
      // re-probe instead of reading garbage from a never-set hostPtr.
      static inline void EmitProbeSlowEntry(x86CodeGenBackend *b, FastCacheProbe &probe) {
        COMP->xor_(probe.hostPtr.r32(), probe.hostPtr.r32()); // r32 zeros full r64
      }

      // Emit the inline cache lookup body: idx hash + tag compare + lea hostPtr.
      // Writes the resulting host byte pointer into |hostPtrOut| on hit;
      // jumps to |slowPath| on tag mismatch.
      static void EmitFreshProbeBody(x86CodeGenBackend *b, x86::Gp eaQ,
                                     x86::Gp hostPtrOut, Label slowPath) {
        auto cacheField = b->GetThreadContext()->scalar(&sPPUThread::fastDataCache);
        x86::Gp threadBase = cacheField.Base();
        const u64 cacheOff = cacheField.Offset();
        constexpr u64 kPageMask = FastTranslationCache::PAGE_MASK;
        constexpr u64 kIndexMask = FastTranslationCache::INDEX_MASK;

        x86::Gp shifted = newGP64();
        COMP->mov(shifted, eaQ);
        COMP->shr(shifted, 12);

        x86::Gp idx = newGP64();
        COMP->mov(idx, shifted);
        COMP->shr(idx, 13);
        COMP->xor_(idx, shifted);
        COMP->and_(idx, asmjit::Imm(kIndexMask));
        COMP->shl(idx, 4);

        x86::Gp page = newGP64();
        COMP->mov(page, eaQ);
        COMP->and_(page, asmjit::Imm(static_cast<s64>(~kPageMask)));

        x86::Gp entryAddr = newGP64();
        COMP->lea(entryAddr, x86::ptr(threadBase, idx, 0, static_cast<s32>(cacheOff)));

        COMP->cmp(x86::qword_ptr(entryAddr, 0), page);
        COMP->jne(slowPath);

        x86::Gp hostPage = newGP64();
        COMP->mov(hostPage, x86::qword_ptr(entryAddr, 8));

        x86::Gp pageOff = newGP64();
        COMP->mov(pageOff, eaQ);
        COMP->and_(pageOff, asmjit::Imm(kPageMask));

        COMP->lea(hostPtrOut, x86::ptr(hostPage, pageOff));
      }

      // Emit the inline fastDataCache probe shared by every LOAD/STORE shape.
      //
      // Layout:
      //   - Fresh probe: ~10 fused-uop cache lookup; slowPath on tag miss.
      //   - Reuse probe (when |eaSSA| matches the previous probe in this
      //     block): test the cached hostPtr against the 0 sentinel; if non-
      //     zero the cached value is reused directly; if zero the previous
      //     access took the slow path so we emit a fresh probe inline (the
      //     cache was populated by that slow path, so this fresh probe will
      //     usually hit), still falling through to slowPath only on cache
      //     miss.
      //
      // Caller emits the hit body, then jmp probe.done, then binds slowPath
      // and calls EmitProbeSlowEntry, then emits the slow body, then binds
      // probe.done.
      static FastCacheProbe EmitFastCacheProbe(x86CodeGenBackend *b, x86::Gp EA, HIR::Value *eaSSA) {
        FastCacheProbe p{};
        p.slowPath = COMP->newLabel();
        p.done = COMP->newLabel();
        p.eaQ = newGP64();
        COMP->mov(p.eaQ, EA.r64());

        if (eaSSA && b->lastProbe.valid && b->lastProbe.eaSSA == eaSSA) {
          p.reused = true;
          p.hostPtr = b->lastProbe.hostPtr;
          Label reuseHit = COMP->newLabel();

          COMP->test(p.hostPtr, p.hostPtr);
          COMP->jnz(reuseHit);
          // Cached hostPtr is the 0 sentinel (prior probe took slow path).
          // Re-issue a fresh inline probe writing into the same vreg.
          EmitFreshProbeBody(b, p.eaQ, p.hostPtr, p.slowPath);

          COMP->bind(reuseHit);
          // Memo already stores p.hostPtr; nothing to update.
          return p;
        }

        p.hostPtr = newGP64();
        EmitFreshProbeBody(b, p.eaQ, p.hostPtr, p.slowPath);

        b->lastProbe.eaSSA = eaSSA;
        b->lastProbe.hostPtr = p.hostPtr;
        b->lastProbe.valid = true;
        return p;
      }

      REGISTER_EMITTER(OPCODE_STORAGE_EXCEPTION_CHECK, Emit_STORAGE_EXCEPTION_CHECK)
        static void Emit_STORAGE_EXCEPTION_CHECK(x86CodeGenBackend *b, const HIR::Instr *instr) {
        Label endLabel = COMP->newLabel();
        x86::Gp exceptReg = newGP16();
        COMP->mov(exceptReg, EXPtr());
        COMP->and_(exceptReg, imm<u16>(0xC));
        COMP->test(exceptReg, exceptReg);
        COMP->jz(endLabel);
        COMP->ret();
        COMP->bind(endLabel);
      }

      // Helper: emit N back-to-back zeroing stores from |base| using the
      // already-zeroed XMM |z|. Each iteration walks 16 bytes via base+disp,
      // avoiding the redundant `add base, 16` between stores.
      static void EmitZeroStores(x86CodeGenBackend *b, x86::Gp base, x86::Xmm z, u32 byteCount) {
        for (u32 off = 0; off < byteCount; off += 16) {
          COMP->vmovaps(x86::ptr(base, static_cast<s32>(off)), z);
        }
      }

      // dcbz / dcbz128 land here. Inline cache probe + bulk vmovaps zero stores
      // on hit; slow path keeps the helper. The hit branch issues a single
      // JITNotifyWrite covering the whole cacheline.
      REGISTER_EMITTER(OPCODE_MEMSET, Emit_MEMSET)
        static void Emit_MEMSET(x86CodeGenBackend *b, const HIR::Instr *instr) {
        x86::Gp EA = LoadValueGp(b, instr->src1.value);
        const u32 byteCount = static_cast<u32>(instr->src3.value->constant.i16);
        if (byteCount != 32 && byteCount != 128) {
          LOG_CRITICAL(Xenon, "Unimplemented MEMSET byte count: {}", byteCount);
          return;
        }

        x86::Xmm zero = allocXmm();
        COMP->vpxor(zero, zero, zero);

        FastCacheProbe probe = EmitFastCacheProbe(b, EA, instr->src1.value);

        // Hit: cacheline is RAM-backed and contiguous (PPC cacheline alignment
        // guarantees the access never crosses a 4KB page).
        EmitZeroStores(b, probe.hostPtr, zero, byteCount);
        EmitNotifyWrite(b, probe.hostPtr, byteCount);
        COMP->jmp(probe.done);

        // Miss: full MMU translation, then bulk zero. JITTranslateAndGetHostPtr
        // with write=true already issues NotifyWrite for RAM-backed pages.
        COMP->bind(probe.slowPath);
        EmitProbeSlowEntry(b, probe);
        {
          x86::Gp slowAddr = newGP64();
          InvokeNode *xlat = nullptr;
          COMP->invoke(&xlat, imm((void *)PPCInterpreter::JITTranslateAndGetHostPtr),
            FuncSignature::build<u64, sPPEState *, u64, ePPUThreadID, bool>());
          xlat->setArg(0, b->ppeState->Base());
          xlat->setArg(1, probe.eaQ);
          xlat->setArg(2, ePPUThread_None);
          xlat->setArg(3, true);
          xlat->setRet(0, slowAddr);
          COMP->test(slowAddr, slowAddr);
          COMP->jz(probe.done);
          EmitZeroStores(b, slowAddr, zero, byteCount);
        }

        COMP->bind(probe.done);
      }

      REGISTER_EMITTER(OPCODE_LOAD, Emit_LOAD)
        static void Emit_LOAD(x86CodeGenBackend *b, const HIR::Instr *instr) {
        x86::Gp EA = LoadValueGp(b, instr->src1.value);
        HIR::TypeName type = instr->dest->type;

        // Integer LOAD: inline fastDataCache probe + slow-path fallback.
        // Hit: raw load + bswap. Miss: MMURead{8,16,32,64}.
        if (type == HIR::INT8_TYPE || type == HIR::INT16_TYPE ||
          type == HIR::INT32_TYPE || type == HIR::INT64_TYPE) {

          x86::Gp result = newGP64();
          FastCacheProbe probe = EmitFastCacheProbe(b, EA, instr->src1.value);

          switch (type) {
          case HIR::INT8_TYPE: {
            x86::Gp tmp8 = newGP8();
            COMP->mov(tmp8, x86::byte_ptr(probe.hostPtr));
            COMP->movzx(result, tmp8);
            break;
          }
          case HIR::INT16_TYPE: {
            x86::Gp tmp16 = newGP16();
            if (kHasMOVBE) {
              COMP->movbe(tmp16, x86::word_ptr(probe.hostPtr));
            } else {
              COMP->mov(tmp16, x86::word_ptr(probe.hostPtr));
              COMP->rol(tmp16, asmjit::Imm(8));
            }
            COMP->movzx(result, tmp16);
            break;
          }
          case HIR::INT32_TYPE: {
            if (kHasMOVBE) {
              // movbe r32, m32 zero-extends into the 64-bit reg implicitly.
              COMP->movbe(result.r32(), x86::dword_ptr(probe.hostPtr));
            } else {
              x86::Gp tmp32 = newGP32();
              COMP->mov(tmp32, x86::dword_ptr(probe.hostPtr));
              COMP->bswap(tmp32);
              COMP->mov(result.r32(), tmp32);
            }
            break;
          }
          case HIR::INT64_TYPE: {
            if (kHasMOVBE) {
              COMP->movbe(result, x86::qword_ptr(probe.hostPtr));
            } else {
              COMP->mov(result, x86::qword_ptr(probe.hostPtr));
              COMP->bswap(result);
            }
            break;
          }
          default: break;
          }
          COMP->jmp(probe.done);

          COMP->bind(probe.slowPath);
          EmitProbeSlowEntry(b, probe);
          InvokeNode *read = nullptr;
          switch (type) {
          case HIR::INT8_TYPE: {
            x86::Gp tmp8 = newGP8();
            COMP->invoke(&read, imm((void *)PPCInterpreter::MMURead8),
              FuncSignature::build<u8, sPPEState *, u64, ePPUThreadID>());
            read->setArg(0, b->ppeState->Base());
            read->setArg(1, probe.eaQ);
            read->setArg(2, ePPUThread_None);
            read->setRet(0, tmp8);
            COMP->movzx(result, tmp8);
            break;
          }
          case HIR::INT16_TYPE: {
            x86::Gp tmp16 = newGP16();
            COMP->invoke(&read, imm((void *)PPCInterpreter::MMURead16),
              FuncSignature::build<u16, sPPEState *, u64, ePPUThreadID>());
            read->setArg(0, b->ppeState->Base());
            read->setArg(1, probe.eaQ);
            read->setArg(2, ePPUThread_None);
            read->setRet(0, tmp16);
            COMP->movzx(result, tmp16);
            break;
          }
          case HIR::INT32_TYPE:
            COMP->invoke(&read, imm((void *)PPCInterpreter::MMURead32),
              FuncSignature::build<u32, sPPEState *, u64, ePPUThreadID>());
            read->setArg(0, b->ppeState->Base());
            read->setArg(1, probe.eaQ);
            read->setArg(2, ePPUThread_None);
            read->setRet(0, result.r32());
            break;
          case HIR::INT64_TYPE:
            COMP->invoke(&read, imm((void *)PPCInterpreter::MMURead64),
              FuncSignature::build<u64, sPPEState *, u64, ePPUThreadID>());
            read->setArg(0, b->ppeState->Base());
            read->setArg(1, probe.eaQ);
            read->setArg(2, ePPUThread_None);
            read->setRet(0, result);
            break;
          default: break;
          }

          COMP->bind(probe.done);
          TagStoreReg(instr->dest, result);
          return;
        }

        // Float/Vector LOAD: inline cache probe + raw vector mov on hit.
        // Byte-swap is a separate HIR op for these types, so the LOAD itself
        // moves raw bytes only.
        if (type == HIR::FLOAT32_TYPE || type == HIR::FLOAT64_TYPE ||
            type == HIR::VEC128_TYPE) {
          x86::Xmm dst = allocXmm();
          FastCacheProbe probe = EmitFastCacheProbe(b, EA, instr->src1.value);

          switch (type) {
          case HIR::FLOAT32_TYPE:
            COMP->vmovss(dst, x86::ptr(probe.hostPtr));
            break;
          case HIR::FLOAT64_TYPE:
            COMP->vmovsd(dst, x86::ptr(probe.hostPtr));
            break;
          case HIR::VEC128_TYPE:
            COMP->vmovups(dst, x86::ptr(probe.hostPtr));
            break;
          default: break;
          }
          COMP->jmp(probe.done);

          COMP->bind(probe.slowPath);
          EmitProbeSlowEntry(b, probe);
          {
            x86::Gp slowAddr = newGP64();
            InvokeNode *xlat = nullptr;
            COMP->invoke(&xlat, imm((void *)PPCInterpreter::JITTranslateAndGetHostPtr),
              FuncSignature::build<u64, sPPEState *, u64, ePPUThreadID, bool>());
            xlat->setArg(0, b->ppeState->Base());
            xlat->setArg(1, probe.eaQ);
            xlat->setArg(2, ePPUThread_None);
            xlat->setArg(3, false);
            xlat->setRet(0, slowAddr);
            // If translation produced no host pointer (DSI/ISI raised), skip.
            COMP->test(slowAddr, slowAddr);
            COMP->jz(probe.done);
            switch (type) {
            case HIR::FLOAT32_TYPE: COMP->vmovss(dst, x86::ptr(slowAddr)); break;
            case HIR::FLOAT64_TYPE: COMP->vmovsd(dst, x86::ptr(slowAddr)); break;
            case HIR::VEC128_TYPE:  COMP->vmovups(dst, x86::ptr(slowAddr)); break;
            default: break;
            }
          }

          COMP->bind(probe.done);
          TagStoreReg(instr->dest, dst);
          return;
        }

        LOG_CRITICAL(Xenon, "Unimplemented TYPE");
      }


      REGISTER_EMITTER(OPCODE_STORE, Emit_STORE)
        static void Emit_STORE(x86CodeGenBackend *b, const HIR::Instr *instr) {
        x86::Gp EA = LoadValueGp(b, instr->src1.value);
        HIR::TypeName type = instr->src2.value->type;

        // Integer STORE: inline fastDataCache probe + slow-path fallback.
        // Hit: bswap + raw store + JITNotifyWrite. Miss: MMUWrite{8,16,32,64}.
        if (type == HIR::INT8_TYPE || type == HIR::INT16_TYPE ||
            type == HIR::INT32_TYPE || type == HIR::INT64_TYPE) {
          x86::Gp storeVal = LoadValueGp(b, instr->src2.value);
          FastCacheProbe probe = EmitFastCacheProbe(b, EA, instr->src1.value);

          u32 byteCount = 0;
          switch (type) {
          case HIR::INT8_TYPE: {
            byteCount = 1;
            COMP->mov(x86::byte_ptr(probe.hostPtr), storeVal.r8());
            break;
          }
          case HIR::INT16_TYPE: {
            byteCount = 2;
            if (kHasMOVBE) {
              COMP->movbe(x86::word_ptr(probe.hostPtr), storeVal.r16());
            } else {
              x86::Gp tmp16 = newGP16();
              COMP->mov(tmp16, storeVal.r16());
              COMP->rol(tmp16, asmjit::Imm(8));
              COMP->mov(x86::word_ptr(probe.hostPtr), tmp16);
            }
            break;
          }
          case HIR::INT32_TYPE: {
            byteCount = 4;
            if (kHasMOVBE) {
              COMP->movbe(x86::dword_ptr(probe.hostPtr), storeVal.r32());
            } else {
              x86::Gp tmp32 = newGP32();
              COMP->mov(tmp32, storeVal.r32());
              COMP->bswap(tmp32);
              COMP->mov(x86::dword_ptr(probe.hostPtr), tmp32);
            }
            break;
          }
          case HIR::INT64_TYPE: {
            byteCount = 8;
            if (kHasMOVBE) {
              COMP->movbe(x86::qword_ptr(probe.hostPtr), storeVal.r64());
            } else {
              x86::Gp tmp64 = newGP64();
              COMP->mov(tmp64, storeVal.r64());
              COMP->bswap(tmp64);
              COMP->mov(x86::qword_ptr(probe.hostPtr), tmp64);
            }
            break;
          }
          default: break;
          }

          EmitNotifyWrite(b, probe.hostPtr, byteCount);
          COMP->jmp(probe.done);

          COMP->bind(probe.slowPath);
          EmitProbeSlowEntry(b, probe);
          {
            InvokeNode *write = nullptr;
            switch (type) {
            case HIR::INT8_TYPE:
              COMP->invoke(&write, imm((void *)PPCInterpreter::MMUWrite8),
                FuncSignature::build<void, sPPEState *, u64, u8, ePPUThreadID>());
              write->setArg(0, b->ppeState->Base());
              write->setArg(1, probe.eaQ);
              write->setArg(2, storeVal);
              write->setArg(3, ePPUThread_None);
              break;
            case HIR::INT16_TYPE:
              COMP->invoke(&write, imm((void *)PPCInterpreter::MMUWrite16),
                FuncSignature::build<void, sPPEState *, u64, u16, ePPUThreadID>());
              write->setArg(0, b->ppeState->Base());
              write->setArg(1, probe.eaQ);
              write->setArg(2, storeVal);
              write->setArg(3, ePPUThread_None);
              break;
            case HIR::INT32_TYPE:
              COMP->invoke(&write, imm((void *)PPCInterpreter::MMUWrite32),
                FuncSignature::build<void, sPPEState *, u64, u32, ePPUThreadID>());
              write->setArg(0, b->ppeState->Base());
              write->setArg(1, probe.eaQ);
              write->setArg(2, storeVal);
              write->setArg(3, ePPUThread_None);
              break;
            case HIR::INT64_TYPE:
              COMP->invoke(&write, imm((void *)PPCInterpreter::MMUWrite64),
                FuncSignature::build<void, sPPEState *, u64, u64, ePPUThreadID>());
              write->setArg(0, b->ppeState->Base());
              write->setArg(1, probe.eaQ);
              write->setArg(2, storeVal);
              write->setArg(3, ePPUThread_None);
              break;
            default: break;
            }
          }

          COMP->bind(probe.done);
          return;
        }

        // Float/Vector STORE: inline cache probe + raw vector mov on hit.
        // Byte-swap is a separate HIR op for these types, so the STORE itself
        // moves raw bytes only. JITNotifyWrite keeps RAM observers + reservations
        // in sync.
        if (type == HIR::FLOAT32_TYPE || type == HIR::FLOAT64_TYPE ||
            type == HIR::VEC128_TYPE) {
          x86::Xmm storeVec = LoadValueXmm(b, instr->src2.value);
          FastCacheProbe probe = EmitFastCacheProbe(b, EA, instr->src1.value);

          u32 byteCount = 0;
          switch (type) {
          case HIR::FLOAT32_TYPE:
            byteCount = 4;
            COMP->vmovss(x86::ptr(probe.hostPtr), storeVec);
            break;
          case HIR::FLOAT64_TYPE:
            byteCount = 8;
            COMP->vmovsd(x86::ptr(probe.hostPtr), storeVec);
            break;
          case HIR::VEC128_TYPE:
            byteCount = 16;
            COMP->vmovups(x86::ptr(probe.hostPtr), storeVec);
            break;
          default: break;
          }

          EmitNotifyWrite(b, probe.hostPtr, byteCount);
          COMP->jmp(probe.done);

          COMP->bind(probe.slowPath);
          EmitProbeSlowEntry(b, probe);
          {
            x86::Gp slowAddr = newGP64();
            InvokeNode *xlat = nullptr;
            COMP->invoke(&xlat, imm((void *)PPCInterpreter::JITTranslateAndGetHostPtr),
              FuncSignature::build<u64, sPPEState *, u64, ePPUThreadID, bool>());
            xlat->setArg(0, b->ppeState->Base());
            xlat->setArg(1, probe.eaQ);
            xlat->setArg(2, ePPUThread_None);
            xlat->setArg(3, true); // write
            xlat->setRet(0, slowAddr);
            COMP->test(slowAddr, slowAddr);
            COMP->jz(probe.done);
            switch (type) {
            case HIR::FLOAT32_TYPE: COMP->vmovss(x86::ptr(slowAddr), storeVec); break;
            case HIR::FLOAT64_TYPE: COMP->vmovsd(x86::ptr(slowAddr), storeVec); break;
            case HIR::VEC128_TYPE:  COMP->vmovups(x86::ptr(slowAddr), storeVec); break;
            default: break;
            }
            // JITTranslateAndGetHostPtr already issues NotifyWrite when called
            // with write=true on a RAM-backed page, so no extra notify here.
          }

          COMP->bind(probe.done);
          return;
        }

        LOG_CRITICAL(Xenon, "Unimplemented TYPE");
      }

      //
      // Atomic Reservation Instructions
      //

      REGISTER_EMITTER(OPCODE_LOAD_RESERVED, Emit_LOAD_RESERVED)
        static void Emit_LOAD_RESERVED(x86CodeGenBackend *b, const HIR::Instr *instr) {
        Label endLabel = COMP->newLabel();

        x86::Gp hostPtr = newGP64();      // Host memory pointer
        x86::Gp data64 = newGP64();       // Zero-extended result
        x86::Gp exceptReg = newGP16();    // Exception check

        x86::Gp EA = LoadValueGp(b, instr->src1.value);

        InvokeNode *mmuTranslation = nullptr;
        COMP->invoke(&mmuTranslation, imm((void *)PPCInterpreter::JITTranslateAndGetHostPtr),
          FuncSignature::build<u64, sPPEState *, u64, ePPUThreadID>());
        mmuTranslation->setArg(0, b->ppeState->Base());
        mmuTranslation->setArg(1, EA);
        mmuTranslation->setArg(2, ePPUThread_None);
        mmuTranslation->setRet(0, hostPtr);

        COMP->mov(exceptReg, EXPtr());
        COMP->and_(exceptReg, imm<u16>(0xC));
        COMP->test(exceptReg, exceptReg);
        COMP->jnz(endLabel);

        HIR::TypeName type = instr->dest->type;

        switch (type) {
        case HIR::INT32_TYPE:
          COMP->mov(data64.r32(), x86::dword_ptr(hostPtr));
          COMP->mov(b->GetThreadContext()->scalar(&sPPUThread::atomicResHostPtr), hostPtr);
          COMP->mov(b->GetThreadContext()->scalar(&sPPUThread::atomicResExpected), data64.r32());
          COMP->bswap(data64.r32());
          break;
        case HIR::INT64_TYPE:
          COMP->mov(data64, x86::qword_ptr(hostPtr));
          COMP->mov(b->GetThreadContext()->scalar(&sPPUThread::atomicResHostPtr), hostPtr);
          COMP->mov(b->GetThreadContext()->scalar(&sPPUThread::atomicResExpected), data64);
          COMP->bswap(data64);
          break;
        default:
          break;
        }

        TagStoreReg(instr->dest, data64);

        COMP->bind(endLabel);
      }

      REGISTER_EMITTER(OPCODE_STORE_RESERVED, Emit_STORE_RESERVED)
        static void Emit_STORE_RESERVED(x86CodeGenBackend *b, const HIR::Instr *instr) {
        Label successLabel = COMP->newLabel();
        Label failLabel = COMP->newLabel();

        x86::Gp hostPtr = newGP64();      // New Host Pointer
        x86::Gp resHostPtr = newGP64();   // Stored Reservation Pointer
        x86::Gp storeValue = newGP64();   // Value to store
        x86::Gp expectedValue = newGP64();// Expected value for CAS
        x86::Gp crValue = newGP32();      // CR0 value
        x86::Gp xerValue = newGP32();     // XER value
        x86::Gp zero64 = newGP64();       // For clearing reservation

        x86::Gp EA = LoadValueGp(b, instr->src1.value);

        // Translate to Host Pointer
        InvokeNode *mmuTranslation = nullptr;
        COMP->invoke(&mmuTranslation, imm((void *)PPCInterpreter::JITTranslateAndGetHostPtr),
          FuncSignature::build<u64, sPPEState *, u64, ePPUThreadID, bool>());
        mmuTranslation->setArg(0, b->ppeState->Base());
        mmuTranslation->setArg(1, EA);
        mmuTranslation->setArg(2, ePPUThread_None);
        mmuTranslation->setArg(3, true);
        mmuTranslation->setRet(0, hostPtr);

        // Check for DSI/ISI
        x86::Gp exceptReg = newGP16();
        COMP->mov(exceptReg, EXPtr());
        COMP->and_(exceptReg, imm<u16>(0xC)); // DStor | DSeg
        COMP->test(exceptReg, exceptReg);
        COMP->jnz(failLabel);

        // Load reservation pointer
        COMP->mov(resHostPtr, b->GetThreadContext()->scalar(&sPPUThread::atomicResHostPtr));

        // Verify address matches reservation
        COMP->cmp(hostPtr, resHostPtr);
        COMP->jne(failLabel);

        // Address matches, attempt CAS
        COMP->mov(expectedValue, b->GetThreadContext()->scalar(&sPPUThread::atomicResExpected));
        x86::Gp srcValue = LoadValueGp(b, instr->src2.value);
        COMP->mov(storeValue, srcValue);

        HIR::TypeName type = static_cast<HIR::TypeName>(instr->flags);

        switch (type) {
        case HIR::INT32_TYPE:
          COMP->bswap(storeValue.r32()); // Convert to LE for host memory
          COMP->lock();
          COMP->cmpxchg(x86::dword_ptr(hostPtr), storeValue.r32(), expectedValue.r32());
          break;
        case HIR::INT64_TYPE:
          COMP->bswap(storeValue); // Convert to LE for host memory
          COMP->lock();
          COMP->cmpxchg(x86::qword_ptr(hostPtr), storeValue, expectedValue);
          break;
        default:
          break;
        }
        
        COMP->jnz(failLabel); // CAS failed (ZF=0)

        // Success
        COMP->mov(crValue, imm(2)); // EQ bit set
        COMP->jmp(successLabel);

        // Fail (Address mismatch or CAS failure)
        COMP->bind(failLabel);
        COMP->xor_(crValue, crValue); // EQ bit clear

        COMP->bind(successLabel);

        // Clear reservation (consume it)
        COMP->xor_(zero64, zero64);
        COMP->mov(b->GetThreadContext()->scalar(&sPPUThread::atomicResHostPtr), zero64);

        // SO bit (summary overflow)
#ifdef __LITTLE_ENDIAN__
        COMP->mov(xerValue.r32(), SPRPtr(XER));
        COMP->shr(xerValue.r32(), imm(31));
#else
        COMP->mov(xerValue.r32(), SPRPtr(XER));
        COMP->and_(xerValue.r32(), imm(1));
#endif
        COMP->shl(xerValue, imm(3 - CR_BIT_SO));
        COMP->or_(crValue, xerValue);

        // Set CR0 field
        const u32 index = static_cast<u32>(0);
        const u32 sh = (7 - index) * 4;
        const u32 clearMask = ~(0xFu << sh);

        x86::Gp tempCR = newGP32();
        COMP->mov(tempCR, CRValPtr());
        COMP->and_(tempCR, asmjit::Imm(clearMask));
        COMP->shl(crValue, asmjit::Imm(sh));
        COMP->or_(tempCR, crValue);
        COMP->mov(CRValPtr(), tempCR);
      }

      REGISTER_EMITTER(OPCODE_ATOMIC_COMPARE_EXCHANGE, Emit_ATOMIC_COMPARE_EXCHANGE)
        static void Emit_ATOMIC_COMPARE_EXCHANGE(x86CodeGenBackend *b, const HIR::Instr *instr) {
        // End label
        Label endLabel = COMP->newLabel();
        // Result
        x86::Gp result = newGP8();
        COMP->xor_(result, result);

        // Get sources
        x86::Gp EA = LoadValueGp(b, instr->src1.value);
        x86::Gp oldValue = LoadValueGp(b, instr->src2.value);
        x86::Gp newValue = LoadValueGp(b, instr->src3.value);

        // Host memory pointer
        x86::Gp hostPtr = newGP64();      

        // Get the pointer
        InvokeNode *mmuTranslation = nullptr;
        COMP->invoke(&mmuTranslation, imm((void *)PPCInterpreter::JITTranslateAndGetHostPtr),
          FuncSignature::build<u64, sPPEState *, u64, ePPUThreadID>());
        mmuTranslation->setArg(0, b->ppeState->Base());
        mmuTranslation->setArg(1, EA);
        mmuTranslation->setArg(2, ePPUThread_None);
        mmuTranslation->setRet(0, hostPtr);

        // Exception check
        x86::Gp exceptReg = newGP16();    
        COMP->mov(exceptReg, EXPtr());
        COMP->and_(exceptReg, imm<u16>(0xC));
        COMP->test(exceptReg, exceptReg);
        COMP->jnz(endLabel);

        HIR::TypeName type = instr->src2.value->type;

        COMP->lock();

        switch (type) {
        case HIR::INT32_TYPE:
          COMP->cmpxchg(x86::dword_ptr(hostPtr), newValue, oldValue);
          break;
        case HIR::INT64_TYPE:
          COMP->cmpxchg(x86::qword_ptr(hostPtr), newValue, oldValue);
          break;
        default: LOG_CRITICAL(Xenon, "Unimplemented TYPE"); break;
        }

        COMP->sete(result);
        COMP->bind(endLabel);
        TagStoreReg(instr->dest, result);
      }
    }
  }
}

#endif