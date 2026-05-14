/***************************************************************/
/* Copyright 2026 Xenon Emulator Project. All rights reserved. */
/***************************************************************/

#include "Base/Arch.h"

#if defined(ARCH_X86) || defined(ARCH_X86_64)

#include "Base/Logging/Log.h"
#include "Core/XCPU/Interpreter/PPCInterpreter.h"
#include "Core/XCPU/JIT/Backend/X86Backend/x86CodeGenBackend.h"
#include "Core/XCPU/JIT/Backend/X86Backend/x86CodeGenHelpers.h"

namespace Xe {
  namespace XCPU {
    namespace JIT {

      void DumpHIR(HIR::HIRBlock *inBlock) {
        using namespace HIR;

        // Get current block
        HIRBlock *block = inBlock;

        if (!block) {
          LOG_INFO(Xenon, "[HIR Dump]: No HIR block generated");
          return;
        }

        // Formats a constant value as a decimal integer or float literal.
        auto fmtConst = [](const Value *v) -> std::string {
          switch (v->type) {
          case INT8_TYPE:    return FMT("{:#04x}", static_cast<u8>(v->constant.i8));
          case INT16_TYPE:   return FMT("{:#06x}", static_cast<u16>(v->constant.i16));
          case INT32_TYPE:   return FMT("{:#010x}", static_cast<u32>(v->constant.i32));
          case INT64_TYPE:   return FMT("{:#018x}", static_cast<u64>(v->constant.i64));
          case FLOAT32_TYPE: return FMT("{}", v->constant.f32);
          case FLOAT64_TYPE: return FMT("{}", v->constant.f64);
          case VEC128_TYPE:
            return FMT("[{:#010x}, {:#010x}, {:#010x}, {:#010x}]",
              v->constant.v128.dsword[0], v->constant.v128.dsword[1],
              v->constant.v128.dsword[2], v->constant.v128.dsword[3]);
          default: return "WTF?";
          }
          };

        // Formats a Value as "v<N>" for SSA values or the constant literal for constants.
        auto fmtVal = [&](const Value *v) -> std::string {
          if (!v) return "(null)";
          if (v->flags & VALUE_IS_CONSTANT) {
            return fmtConst(v);
          }
          return FMT("v{}", v->ordinal);
          };

        LOG_INFO(Xenon, "[HIR Dump]: === Begin HIR ===");

        u32 blockIdx = 0;
        u32 totalInstrCount = 0;

        // Walk the full block chain (per-function mode has multiple linked blocks).
        for (const HIRBlock *curBlock = block; curBlock; curBlock = curBlock->next, ++blockIdx) {
          LOG_INFO(Xenon, "[HIR Dump]: -- Block {} --", blockIdx);

          const Instr *instr = curBlock->instr_head;
          u32 instrCount = 0;
          while (instr) {
            if (!instr->opcode) {
              instr = instr->next;
              continue;
            }

            if (instr->opcode->num == OPCODE_COMMENT) {
              const char *text = reinterpret_cast<const char *>(instr->src1.offset);
              LOG_INFO(Xenon, "  // {}", text ? text : "");
            }
            else if (instr->opcode->num == OPCODE_NOP) {
              LOG_INFO(Xenon, "  nop");
            }
            else {
              const char *opName = instr->opcode->name ? instr->opcode->name : "???";

              u32 sig = instr->opcode->signature;
              u32 src1Type = (sig >> 3) & 0x7;
              u32 src2Type = (sig >> 6) & 0x7;
              u32 src3Type = (sig >> 9) & 0x7;

              std::string operands;
              auto appendOperand = [&](std::string_view op) {
                if (!operands.empty()) operands += ", ";
                operands += op;
                };

              if (src1Type == OPCODE_SIG_TYPE_V && instr->src1.value) {
                appendOperand(fmtVal(instr->src1.value));
              }
              else if (src1Type == OPCODE_SIG_TYPE_O) {
                appendOperand(FMT("+{:#x}", instr->src1.offset));
              }
              else if (src1Type == OPCODE_SIG_TYPE_S && instr->src1.label) {
                appendOperand(FMT("label@{}", static_cast<const void *>(instr->src1.label)));
              }
              else if (src1Type == OPCODE_SIG_TYPE_L) {
                appendOperand(FMT("label@{}", static_cast<const void *>(instr->src1.label)));
              }

              if (src2Type == OPCODE_SIG_TYPE_V && instr->src2.value) {
                appendOperand(fmtVal(instr->src2.value));
              }
              else if (src2Type == OPCODE_SIG_TYPE_O) {
                appendOperand(FMT("+{:#x}", instr->src2.offset));
              }
              else if (src2Type == OPCODE_SIG_TYPE_L) {
                appendOperand(FMT("label@{}", static_cast<const void *>(instr->src2.label)));
              }

              if (src3Type == OPCODE_SIG_TYPE_V && instr->src3.value) {
                appendOperand(fmtVal(instr->src3.value));
              }
              else if (src3Type == OPCODE_SIG_TYPE_O) {
                appendOperand(FMT("+{:#x}", instr->src3.offset));
              }

              if (instr->dest) {
                if (!operands.empty()) {
                  LOG_INFO(Xenon, "  v{} = {} {}", instr->dest->ordinal, opName, operands);
                }
                else {
                  LOG_INFO(Xenon, "  v{} = {}", instr->dest->ordinal, opName);
                }
              }
              else {
                if (!operands.empty()) {
                  LOG_INFO(Xenon, "  {} {}", opName, operands);
                }
                else {
                  LOG_INFO(Xenon, "  {}", opName);
                }
              }
            }

            ++instrCount;
            ++totalInstrCount;
            instr = instr->next;
          }
          LOG_INFO(Xenon, "[HIR Dump]: -- Block {} end ({} instructions) --", blockIdx, instrCount);
        }

        LOG_INFO(Xenon, "[HIR Dump]: === End HIR ({} blocks, {} total instructions) ===",
          blockIdx, totalInstrCount);
      }

      // Global emitter key list (filled at static-init by REGISTER_EMITTER)
      std::vector<x86EmitterKey> &GetEmitterKeyList() {
        static std::vector<x86EmitterKey> list;
        return list;
      }

      // 
      // Stub emitters for unused opcodes
      //

      REGISTER_EMITTER(OPCODE_COMMENT, Emit_COMMENT)
      static void Emit_COMMENT(x86CodeGenBackend *, const HIR::Instr *) {
        // Comments are annotation-only, no code emitted.
      }

      REGISTER_EMITTER(OPCODE_NOP, Emit_NOP)
      static void Emit_NOP(x86CodeGenBackend *, const HIR::Instr *) {
        // Intentional no-op.
      }

      REGISTER_EMITTER(OPCODE_SOURCE_OFFSET, Emit_SOURCE_OFFSET)
      static void Emit_SOURCE_OFFSET(x86CodeGenBackend *b, const HIR::Instr *instr) {
        // Source offset annotation emitted by PPCTranslator.
        // flags bit 0 = needs full CIA/NIA/CI write (branch or faulting instr)
        // Otherwise only CI is written.
        u64 addr = instr->src1.offset;
        u32 rawWord = instr->currentInstrData.opcode;

        if (instr->flags & 1) {
          // Full prologue: CIA, NIA, CI
          asmjit::x86::Gp tmp = newGP64();
          COMP->mov(tmp, asmjit::Imm(static_cast<int64_t>(addr)));
          COMP->mov(CIAPtr(), tmp);
          COMP->mov(tmp, asmjit::Imm(static_cast<int64_t>(addr + 4)));
          COMP->mov(NIAPtr(), tmp);
          COMP->mov(tmp, asmjit::Imm(static_cast<int64_t>(rawWord)));
          COMP->mov(b->GetThreadContext()->scalar(&sPPUThread::CI).Ptr<u32>(), tmp);
        } else {
          // Minimal prologue: CI only
          asmjit::x86::Gp tmp = newGP64();
          COMP->mov(tmp, asmjit::Imm(static_cast<int64_t>(rawWord)));
          COMP->mov(b->GetThreadContext()->scalar(&sPPUThread::CI).Ptr<u32>(), tmp);
        }
      }

      REGISTER_EMITTER(OPCODE_CONTEXT_BARRIER, Emit_CONTEXT_BARRIER)
      static void Emit_CONTEXT_BARRIER(x86CodeGenBackend *, const HIR::Instr *) {
        // Barrier for optimization passes, no code emitted.
      }

      REGISTER_EMITTER(OPCODE_SYSCALL, Emit_SYSCALL)
      static void Emit_SYSCALL(x86CodeGenBackend *b, const HIR::Instr *instr) {
        x86::Gp exReg = newGP16();
        COMP->mov(exReg, EXPtr());
        COMP->or_(exReg, imm<u16>(ppuSystemCallEx));
        COMP->mov(EXPtr(), exReg);
        COMP->mov(b->GetThreadContext()->scalar(&sPPUThread::exHVSysCall).Ptr(), imm<bool>(instr->currentInstrData.lev & 1));
      }

      REGISTER_EMITTER(OPCODE_RFID, Emit_RFID)
      static void Emit_RFID(x86CodeGenBackend *b, const HIR::Instr *instr) {
        x86::Gp srr1 = newGP64();
        x86::Gp msr = newGP64();
        x86::Gp tmp = newGP64();

        Label skipHVMESet = COMP->newLabel();
        Label use64 = COMP->newLabel();

        // MSR[1-2,4-32,37-41,49-50,52-57,60-63] <- SRR1[1-2,4-32,37-41,49-50,52-57,60-63]
        COMP->mov(srr1, SPRPtr(SRR1));
        COMP->mov(msr, srr1);

        // MSR[0] <- SRR1[0] | SRR1[1]

        Label setMSRSF = COMP->newLabel();
        Label doneMSRSF = COMP->newLabel();

        COMP->bt(srr1, 63); // SRR1[0]
        COMP->jc(setMSRSF);
        COMP->bt(srr1, 62); // SRR1[1]
        COMP->jc(setMSRSF);
        // SRR1[0] || SRR1[1] == 0
        COMP->btr(msr, 63);
        COMP->jmp(doneMSRSF);
        COMP->bind(setMSRSF);
        COMP->bts(msr, 63);
        // Check was done and MSR was set.
        COMP->bind(doneMSRSF);

        // MSR[58] = SRR1[58] | SRR1[49]

        Label setMSRIR = COMP->newLabel();
        Label doneMSRIR = COMP->newLabel();

        COMP->bt(srr1, 5); // SRR1[58]
        COMP->jc(setMSRIR);
        COMP->bt(srr1, 14); // SRR1[49]
        COMP->jc(setMSRIR);
        // SRR1[58] || SRR1[49] == 0
        COMP->btr(msr, 5);
        COMP->jmp(doneMSRIR);
        COMP->bind(setMSRIR);
        COMP->bts(msr, 5);
        // Check was done and MSR was set.
        COMP->bind(doneMSRIR);

        // MSR[59] = SRR1[59] | SRR1[49]

        Label setMSRDR = COMP->newLabel();
        Label doneMSRDR = COMP->newLabel();

        COMP->bt(srr1, 4); // SRR1[59]
        COMP->jc(setMSRDR);
        COMP->bt(srr1, 14); // SRR1[49]
        COMP->jc(setMSRDR);
        // SRR1[58] || SRR1[49] == 0
        COMP->btr(msr, 4);
        COMP->jmp(doneMSRDR);
        COMP->bind(setMSRDR);
        COMP->bts(msr, 4);
        // Check was done and MSR was set.
        COMP->bind(doneMSRDR);

        // Check for HV bit in current MSR to skip setting HV and ME bits
        x86::Gp currentMSR = newGP64();
        COMP->mov(currentMSR, SPRPtr(MSR));
        COMP->bt(currentMSR, 60); // MSR.HV
        COMP->jnc(skipHVMESet);   // Jump if not in HV mode.

        Label skipMSRSF = COMP->newLabel();
        Label skipMSRME = COMP->newLabel();

        // Set MSR[HV]
        COMP->bt(srr1, 60); // SRR1[3]
        COMP->jnc(skipMSRSF);
        COMP->bts(msr, 60);
        COMP->bind(skipMSRSF);
        // Set MSR[ME]
        COMP->bt(srr1, 12); // SRR1[51]
        COMP->jnc(skipMSRME);
        COMP->bts(msr, 12);
        COMP->bind(skipMSRME);

        // Skip setting HV and ME bits if not in HV mode
        COMP->bind(skipHVMESet);
        // Store composed MSR
        COMP->mov(SPRPtr(MSR), msr);

        // NIA <- SRR0 & ~3
        x86::Gp srr0 = newGP64();
        x86::Gp nia = newGP64();
        COMP->mov(srr0, SPRPtr(SRR0));
        COMP->mov(nia, srr0);
        COMP->and_(nia, imm<u64>(~3ULL));
        COMP->mov(NIAPtr(), nia);

        // If MSR.SF == 0 (32-bit mode), truncate NIA to 32 bits
        COMP->bt(msr, 63);  // MSR.SF
        COMP->jc(use64);    // if set -> 64-bit mode, skip truncation
        COMP->and_(nia, imm<u32>(0xFFFFFFFF));
        COMP->mov(NIAPtr(), nia);

        COMP->bind(use64);

        // Clear exception taken flag.
        COMP->mov(tmp.r8(), b->GetThreadContext()->scalar(&sPPUThread::interruptTaken));
        COMP->and_(tmp, 0);
        COMP->mov(b->GetThreadContext()->scalar(&sPPUThread::interruptTaken), tmp.r8());
      }

      REGISTER_EMITTER(OPCODE_MTMSRD, Emit_MTMSRD)
      static void Emit_MTMSRD(x86CodeGenBackend *b, const HIR::Instr *instr) {
        // Source
        x86::Gp rsValue = LoadValueGp(b, instr->src1.value);
        // Get MSR
        x86::Gp msrValue = newGP64();
        COMP->mov(msrValue, SPRPtr(MSR));

        if (instr->currentInstrData.l15) {
          /*
             Bits 48 and 62 of register RS are placed into the corresponding bits
             of the MSR. The remaining bits of the MSR are unchanged
          */

          Label skipMSREE = COMP->newLabel();
          Label skipMSRRI = COMP->newLabel();

          // Bit 48 = MSR[EE]

          // Clear MSR
          COMP->btr(msrValue, imm(63 - 48));  // 48
          // Check if rS has the bit set
          COMP->bt(rsValue, imm(63 - 48));    // 48
          // Jump otherwise
          COMP->jnc(skipMSREE);
          // Set the MSR bit
          COMP->bts(msrValue, imm(63 - 48));
          COMP->bind(skipMSREE);


          // Bit 62 = MSR[RI]
          COMP->btr(msrValue, imm(63 - 62));  // 62
          COMP->bt(rsValue, imm(63 - 62));    // 62
          COMP->jnc(skipMSRRI);
          COMP->bts(msrValue, imm(63 - 62));
          COMP->bind(skipMSRRI);

          // Store back MSR
          COMP->mov(SPRPtr(MSR), msrValue);
        } else {
          /*
             The result of ORing bits 00 and 01 of register RS is placed into MSR0
             The result of ORing bits 48 and 49 of register RS is placed into MSR48
             The result of ORing bits 58 and 49 of register RS is placed into MSR58
             The result of ORing bits 59 and 49 of register RS is placed into MSR59
             Bits 1:2, 4:47, 49:50, 52:57, and 60:63 of register RS are placed into
             the corresponding bits of the MSR
          */

          // MSR = rS
          COMP->mov(msrValue, rsValue);

          // MSR0 = (RS)0 | (RS)1

          Label setMSRSF = COMP->newLabel();
          Label doneMSRSF = COMP->newLabel();

          COMP->bt(rsValue, imm(63 - 0));
          COMP->jc(setMSRSF);
          COMP->bt(rsValue, imm(63 - 1));
          COMP->jc(setMSRSF);
          COMP->btr(msrValue, imm(63 - 0));
          COMP->jmp(doneMSRSF);
          COMP->bind(setMSRSF);
          COMP->bts(msrValue, imm(63 - 0));
          // Check was done and MSR was set.
          COMP->bind(doneMSRSF);

          // MSR48 = (RS)48 | (RS)49

          Label setMSREE = COMP->newLabel();
          Label doneMSREE = COMP->newLabel();

          COMP->bt(rsValue, imm(63 - 48));
          COMP->jc(setMSREE);
          COMP->bt(rsValue, imm(63 - 49));
          COMP->jc(setMSREE);
          COMP->btr(msrValue, imm(63 - 48));
          COMP->jmp(doneMSREE);
          COMP->bind(setMSREE);
          COMP->bts(msrValue, imm(63 - 48));
          // Check was done and MSR was set.
          COMP->bind(doneMSREE);

          // MSR58 = (RS)58 | (RS)49

          Label setMSRIR = COMP->newLabel();
          Label doneMSRIR = COMP->newLabel();

          COMP->bt(rsValue, imm(63 - 58));
          COMP->jc(setMSRIR);
          COMP->bt(rsValue, imm(63 - 49));
          COMP->jc(setMSRIR);
          COMP->btr(msrValue, imm(63 - 58));
          COMP->jmp(doneMSRIR);
          COMP->bind(setMSRIR);
          COMP->bts(msrValue, imm(63 - 58));
          // Check was done and MSR was set.
          COMP->bind(doneMSRIR);

          // MSR59 = (RS)59 | (RS)49

          Label setMSRDR = COMP->newLabel();
          Label doneMSRDR = COMP->newLabel();

          COMP->bt(rsValue, imm(63 - 59));
          COMP->jc(setMSRDR);
          COMP->bt(rsValue, imm(63 - 49));
          COMP->jc(setMSRDR);
          COMP->btr(msrValue, imm(63 - 59));
          COMP->jmp(doneMSRDR);
          COMP->bind(setMSRDR);
          COMP->bts(msrValue, imm(63 - 59));
          // Check was done and MSR was set.
          COMP->bind(doneMSRDR);

          // Store back MSR
          COMP->mov(SPRPtr(MSR), msrValue);
        }    
      }

      REGISTER_EMITTER(OPCODE_SYNC_EXCEPTION_CHECK, Emit_SYNC_EXCEPTION_CHECK)
        static void Emit_SYNC_EXCEPTION_CHECK(x86CodeGenBackend *b, const HIR::Instr *instr) {
        // Test for present exceptions and return if any is found.
        Label skipRet = COMP->newLabel();
        x86::Gp exceptReg = newGP16();
        COMP->mov(exceptReg, EXPtr());
        COMP->and_(exceptReg, imm(SyncExceptionMask));
        // Test
        COMP->test(exceptReg, exceptReg);
        COMP->jz(skipRet);
        COMP->ret();
        COMP->bind(skipRet);
      }

      REGISTER_EMITTER(OPCODE_MEMORY_BARRIER, Emit_MEMORY_BARRIER)
        static void Emit_MEMORY_BARRIER(x86CodeGenBackend *b, const HIR::Instr *instr) {
        COMP->mfence();
      }

      // Timebase trampolines
      u64 LoadTimeBase(PPU *ppuState) {
        return ppuState->GetCPUContext()->timeBase.ReadTB();
      }

      u32 LoadDec(PPU *ppuState) {
        return ppuState->GetCPUContext()->timeBase.ReadDEC(ppuState->GetPPUState()->ppuThread[curThreadId].SPR.PIR);
      }

      void WriteTimeBaseLower(PPU *ppuState, u32 timeBaseLower) {
        ppuState->GetCPUContext()->timeBase.WriteTBL(timeBaseLower);
      }

      void WriteTimeBaseUpper(PPU *ppuState, u32 timeBaseUpper) {
        ppuState->GetCPUContext()->timeBase.WriteTBU(timeBaseUpper);
      }

      void WriteDEC(PPU *ppuState, u32 newDec) {
        ppuState->GetCPUContext()->timeBase.WriteDEC(ppuState->GetPPUState()->ppuThread[curThreadId].SPR.PIR, newDec);
      }

      void WriteHDEC(PPU *ppuState, u32 newHDec) {
        ppuState->GetCPUContext()->timeBase.WriteHDEC(ppuState->GetPPUState()->ppuThread[curThreadId].SPR.PIR, newHDec);
      }

      REGISTER_EMITTER(OPCODE_MFSPR, Emit_MFSPR)
        static void Emit_MFSPR(x86CodeGenBackend *b, const HIR::Instr *instr) {
        u32 sprNum = instr->currentInstrData.spr;
        sprNum = ((sprNum & 0x1F) << 5) | ((sprNum >> 5) & 0x1F);

        x86::Gp rSValue = newGP64();

        switch (static_cast<eXenonSPR>(sprNum)) {
        case eXenonSPR::XER:
          COMP->mov(rSValue, SPRPtr(XER));
          break;
        case eXenonSPR::LR:
          COMP->mov(rSValue, SPRPtr(LR));
          break;
        case eXenonSPR::CTR:
          COMP->mov(rSValue, SPRPtr(CTR));
          break;
        case eXenonSPR::DSISR:
          COMP->mov(rSValue, SPRPtr(DSISR));
          break;
        case eXenonSPR::DAR:
          COMP->mov(rSValue, SPRPtr(DAR));
          break;
        case eXenonSPR::DEC:
        {
          asmjit::InvokeNode *invokeNode = nullptr;
          COMP->invoke(&invokeNode, reinterpret_cast<void *>(LoadDec), asmjit::FuncSignature::build<u32, PPU *>());
          invokeNode->setArg(0, b->ppuState->Base());
          invokeNode->setRet(0, rSValue);
        } break;
        case eXenonSPR::SDR1:
          COMP->mov(rSValue, SharedSPRPtr(SDR1));
          break;
        case eXenonSPR::SRR0:
          COMP->mov(rSValue, SPRPtr(SRR0));
          break;
        case eXenonSPR::SRR1:
          COMP->mov(rSValue, SPRPtr(SRR1));
          break;
        case eXenonSPR::CFAR:
          COMP->mov(rSValue, SPRPtr(CFAR));
          break;
        case eXenonSPR::CTRLRD:
          COMP->mov(rSValue, SharedSPRPtr(CTRL));
          break;
        case eXenonSPR::VRSAVE:
          COMP->mov(rSValue, SPRPtr(VRSAVE));
          break;
        case eXenonSPR::TBLRO:
        {
          asmjit::InvokeNode *invokeNode = nullptr;
          COMP->invoke(&invokeNode, reinterpret_cast<void *>(LoadTimeBase), asmjit::FuncSignature::build<u64, PPU *>());
          invokeNode->setArg(0, b->ppuState->Base());
          invokeNode->setRet(0, rSValue);
          COMP->and_(rSValue, 0x00000000FFFFFFFF);
        } break;
        case eXenonSPR::TBURO:
        {
          asmjit::InvokeNode *invokeNode = nullptr;
          COMP->invoke(&invokeNode, reinterpret_cast<void *>(LoadTimeBase), asmjit::FuncSignature::build<u64, PPU *>());
          invokeNode->setArg(0, b->ppuState->Base());
          invokeNode->setRet(0, rSValue);
          COMP->shl(rSValue, 32);
          COMP->and_(rSValue, 0xFFFFFFFF);
        } break;
        case eXenonSPR::SPRG0:
          COMP->mov(rSValue, SPRPtr(SPRG0));
          break;
        case eXenonSPR::SPRG1:
          COMP->mov(rSValue, SPRPtr(SPRG1));
          break;
        case eXenonSPR::SPRG2:
          COMP->mov(rSValue, SPRPtr(SPRG2));
          break;
        case eXenonSPR::SPRG3:
          COMP->mov(rSValue, SPRPtr(SPRG3));
          break;
        case eXenonSPR::PVR:
          COMP->mov(rSValue, SharedSPRPtr(PVR));
          break;
        case eXenonSPR::HSPRG0:
          COMP->mov(rSValue, SPRPtr(HSPRG0));
          break;
        case eXenonSPR::HSPRG1:
          COMP->mov(rSValue, SPRPtr(HSPRG1));
          break;
        case eXenonSPR::RMOR:
          COMP->mov(rSValue, SharedSPRPtr(RMOR));
          break;
        case eXenonSPR::HRMOR:
          COMP->mov(rSValue, SharedSPRPtr(HRMOR));
          break;
        case eXenonSPR::LPCR:
          COMP->mov(rSValue, SharedSPRPtr(LPCR));
          break;
        case eXenonSPR::TSCR:
          COMP->mov(rSValue, SharedSPRPtr(TSCR));
          break;
        case eXenonSPR::TTR:
          COMP->mov(rSValue, SharedSPRPtr(TTR));
          break;
        case eXenonSPR::PPE_TLB_Index_Hint:
          COMP->mov(rSValue, SPRPtr(PPE_TLB_Index_Hint));
          break;
        case eXenonSPR::HID0:
          COMP->mov(rSValue, SharedSPRPtr(HID0));
          break;
        case eXenonSPR::HID1:
          COMP->mov(rSValue, SharedSPRPtr(HID1));
          break;
        case eXenonSPR::HID4:
          COMP->mov(rSValue, SharedSPRPtr(HID4));
          break;
        case eXenonSPR::DABR:
          COMP->mov(rSValue, SPRPtr(DABR));
          break;
        case eXenonSPR::HID6:
          COMP->mov(rSValue, SharedSPRPtr(HID6));
          break;
        case eXenonSPR::PIR:
          COMP->mov(rSValue, SPRPtr(PIR));
          break;
        case eXenonSPR::SPR_DBG_1014:
          COMP->mov(rSValue, SPRPtr(SPR_DBG_1014));
          break;
        case eXenonSPR::SPR_DBG_1018:
          COMP->mov(rSValue, SPRPtr(SPR_DBG_1018));
          break;
        default:
          LOG_ERROR(Xenon, "(Thrd{:#d} mfspr: Unknown SPR: {:#x}", static_cast<u8>(curThreadId), sprNum);
          break;
        }

        
        TagStoreReg(instr->dest, rSValue);
      }


      void JITHelper_MTCTRLWR(sPPEState *ppeState) {
        uCTRL newCTRL;
        newCTRL.hexValue = static_cast<u32>(GPRi(rd));
        if (curThreadId == ePPUThread_Zero) {
          // Thread Zero
          if (ppeState->SPR.CTRL.TE1) {
            // TE1 is set, do not modify it.
            newCTRL.TE1 = 1;
          }
        } else {
          // Thread One
          if (ppeState->SPR.CTRL.TE0) {
            // TE0 is set, do not modify it.
            newCTRL.TE0 = 1;
          }
        }

        // TODO: Check this, reversing and docs suggests this is the correct behavior.
        // If a thread is being enabled, we must generate a reset interrupt on said thread.
        if (ppeState->SPR.CTRL.TE0 == 0 && newCTRL.TE0) { ppeState->ppuThread[0].exceptReg |= ppuSystemResetEx; }
        if (ppeState->SPR.CTRL.TE1 == 0 && newCTRL.TE1) { ppeState->ppuThread[1].exceptReg |= ppuSystemResetEx; }

        LOG_TRACE(Xenon, "{} (Thread{:#d}): Setting ctrl to {:#x}", ppeState->ppuName, (u8)curThreadId, newCTRL.hexValue);

        ppeState->SPR.CTRL = newCTRL;
      }

      REGISTER_EMITTER(OPCODE_MTSPR, Emit_MTSPR)
        static void Emit_MTSPR(x86CodeGenBackend *b, const HIR::Instr *instr) {
        u32 sprNum = instr->currentInstrData.spr;
        sprNum = ((sprNum & 0x1F) << 5) | ((sprNum >> 5) & 0x1F);

        x86::Gp rSValue = LoadValueGp(b, instr->src1.value);

        switch (static_cast<eXenonSPR>(sprNum)) {
        case eXenonSPR::XER:
          // Clear the unused bits in XER (35:56)
          COMP->and_(rSValue, imm(0xE000007F));
          COMP->mov(SPRPtr(XER), rSValue);
          break;
        case eXenonSPR::LR:
          COMP->mov(SPRPtr(LR), rSValue);
          break;
        case eXenonSPR::CTR:
          COMP->mov(SPRPtr(CTR), rSValue);
          break;
        case eXenonSPR::DSISR:
          COMP->mov(SPRPtr(DSISR), rSValue);
          break;
        case eXenonSPR::DAR:
          COMP->mov(SPRPtr(DAR), rSValue);
          break;
        case eXenonSPR::DEC: {
          asmjit::InvokeNode *invokeNode = nullptr;
          COMP->invoke(&invokeNode, reinterpret_cast<void *>(WriteDEC), asmjit::FuncSignature::build<void, PPU *, u32>());
          invokeNode->setArg(0, b->ppuState->Base());
          invokeNode->setArg(1, rSValue.r32());
        } break;
        case eXenonSPR::SDR1:
          COMP->mov(SharedSPRPtr(SDR1), rSValue);
          break;
        case eXenonSPR::SRR0:
          COMP->mov(SPRPtr(SRR0), rSValue);
          break;
        case eXenonSPR::SRR1:
          COMP->mov(SPRPtr(SRR1), rSValue);
          break;
        case eXenonSPR::CFAR:
          COMP->mov(SPRPtr(CFAR), rSValue);
          break;
        case eXenonSPR::CTRLWR: {
          // Invoke a helper to do the CTRL reg setting.
          InvokeNode *ctrlwrInv = nullptr;
          COMP->invoke(&ctrlwrInv, imm((void *)JITHelper_MTCTRLWR), FuncSignature::build<void, sPPEState *>());
          ctrlwrInv->setArg(0, b->ppeState->Base());
        } break;
        case eXenonSPR::VRSAVE:
          COMP->mov(SPRPtr(VRSAVE), rSValue);
          break;      
        case eXenonSPR::SPRG0:
          COMP->mov(SPRPtr(SPRG0), rSValue);
          break;
        case eXenonSPR::SPRG1:
          COMP->mov(SPRPtr(SPRG1), rSValue);
          break;
        case eXenonSPR::SPRG2:
          COMP->mov(SPRPtr(SPRG2), rSValue);
          break;
        case eXenonSPR::SPRG3:
          COMP->mov(SPRPtr(SPRG3), rSValue);
          break;
        case eXenonSPR::TBLWO: {
          asmjit::InvokeNode *invokeNode = nullptr;
          COMP->invoke(&invokeNode, reinterpret_cast<void *>(WriteTimeBaseLower), asmjit::FuncSignature::build<void, PPU *, u32>());
          invokeNode->setArg(0, b->ppuState->Base());
          invokeNode->setArg(1, rSValue.r32());
        } break;
        case eXenonSPR::TBUWO: {
          asmjit::InvokeNode *invokeNode = nullptr;
          COMP->invoke(&invokeNode, reinterpret_cast<void *>(WriteTimeBaseUpper), asmjit::FuncSignature::build<void, PPU *, u32>());
          invokeNode->setArg(0, b->ppuState->Base());
          invokeNode->setArg(1, rSValue.r32());
        } break;
        case eXenonSPR::HSPRG0:
          COMP->mov(SPRPtr(HSPRG0), rSValue);
          break;
        case eXenonSPR::HSPRG1:
          COMP->mov(SPRPtr(HSPRG1), rSValue);
          break;
        case eXenonSPR::HDEC: {
          //asmjit::InvokeNode *invokeNode = nullptr;
          //COMP->invoke(&invokeNode, reinterpret_cast<void *>(WriteHDEC), asmjit::FuncSignature::build<void, PPU *, u32>());
          //invokeNode->setArg(0, b->ppuState->Base());
          //invokeNode->setArg(1, rSValue.r32());
        } break;
        case eXenonSPR::RMOR:
          COMP->mov(SharedSPRPtr(RMOR), rSValue);
          break;
        case eXenonSPR::HRMOR:
          COMP->mov(SharedSPRPtr(HRMOR), rSValue);
          break;
        case eXenonSPR::LPCR:
          COMP->mov(SharedSPRPtr(LPCR), rSValue);
          break;
        case eXenonSPR::LPIDR:
          COMP->mov(SharedSPRPtr(LPIDR), rSValue.r32());
          break;
        case eXenonSPR::TSCR:
          COMP->mov(SharedSPRPtr(TSCR), rSValue.r32());
          break;
        case eXenonSPR::TTR:
          COMP->mov(SharedSPRPtr(TTR), rSValue);
          break;
        case eXenonSPR::PPE_TLB_Index:
          COMP->mov(SharedSPRPtr(PPE_TLB_Index), rSValue);
          break;
        case eXenonSPR::PPE_TLB_Index_Hint:
          COMP->mov(SPRPtr(PPE_TLB_Index_Hint), rSValue);
          break;
        case eXenonSPR::PPE_TLB_VPN: {
          COMP->mov(SharedSPRPtr(PPE_TLB_VPN), rSValue);
          // Add new tlb entry via function call
          InvokeNode *addTLBEntryInv = nullptr;
          COMP->invoke(&addTLBEntryInv, imm((void *)PPCInterpreter::mmuAddTlbEntry), FuncSignature::build<void, sPPEState *>());
          addTLBEntryInv->setArg(0, b->ppeState->Base());
        } break;
        case eXenonSPR::PPE_TLB_RPN:
          COMP->mov(SharedSPRPtr(PPE_TLB_RPN), rSValue);
          break;
        case eXenonSPR::HID0:
          COMP->mov(SharedSPRPtr(HID0), rSValue);
          break;
        case eXenonSPR::HID1:
          COMP->mov(SharedSPRPtr(HID1), rSValue);
          break;
        case eXenonSPR::HID4:
          COMP->mov(SharedSPRPtr(HID4), rSValue);
          break;
        case eXenonSPR::HID6:
          COMP->mov(SharedSPRPtr(HID6), rSValue);
          break;
        case eXenonSPR::DABR:
          COMP->mov(SPRPtr(DABR), rSValue);
          break;
        case eXenonSPR::DABRX:
          COMP->mov(SPRPtr(DABRX), rSValue);
          break;
        case eXenonSPR::SPR_DBG_1014:
          COMP->mov(SPRPtr(SPR_DBG_1014), rSValue);
          break;
        case eXenonSPR::SPR_DBG_1018:
          COMP->mov(SPRPtr(SPR_DBG_1018), rSValue);
          break;
        default:
          LOG_ERROR(Xenon, "(Thrd{:#d} mfspr: Unknown SPR: {:#x}", static_cast<u8>(curThreadId), sprNum);
          break;
        }
      }

      REGISTER_EMITTER(OPCODE_CALL_HLE_FUNCTION, Emit_CALL_HLE_FUNCTION)
        static void Emit_CALL_HLE_FUNCTION(x86CodeGenBackend *b, const HIR::Instr *instr) {
        void *fnPtr = reinterpret_cast<void *>(instr->src1.offset);
        InvokeNode *hleInv = nullptr;
        COMP->invoke(&hleInv, imm(fnPtr), FuncSignature::build<int, void *>());
        hleInv->setArg(0, b->ppeState->Base());
      }

      // Unconditional intra-function jump (per-function mode only).
      // src1.offset holds the HIRBlock* target cast to u64.
      REGISTER_EMITTER(OPCODE_BRANCH_LABEL, Emit_BRANCH_LABEL)
      static void Emit_BRANCH_LABEL(x86CodeGenBackend *b, const HIR::Instr *instr) {
        auto *targetBlock = reinterpret_cast<HIR::HIRBlock *>(instr->src1.offset);
        asmjit::Label lbl = b->GetFunctionBlockLabel(targetBlock);
        if (lbl.isValid()) {
          COMP->jmp(lbl);
        }
      }

      // Conditional intra-function jump (per-function mode only).
      // src1.value = INT8 condition; src2.offset holds the taken HIRBlock* cast to u64.
      // Fallthrough (condition false) continues to the immediately following block.
      REGISTER_EMITTER(OPCODE_BRANCH_TRUE_LABEL, Emit_BRANCH_TRUE_LABEL)
      static void Emit_BRANCH_TRUE_LABEL(x86CodeGenBackend *b, const HIR::Instr *instr) {
        auto *targetBlock = reinterpret_cast<HIR::HIRBlock *>(instr->src2.offset);
        asmjit::Label lbl = b->GetFunctionBlockLabel(targetBlock);
        if (lbl.isValid()) {
          x86::Gp cond = LoadValueGp(b, instr->src1.value);
          COMP->test(cond.r8(), cond.r8());
          COMP->jnz(lbl);
        }
      }

      //
      // Call Interpreter (system instruction fallback)
      //

      // Stores the raw PPC instruction into sPPUThread::CI, then invokes
      // the interpreter function with sPPEState* as its argument.
      REGISTER_EMITTER(OPCODE_CALL_INTERPRETER, Emit_CALL_INTERPRETER)
      static void Emit_CALL_INTERPRETER(x86CodeGenBackend *b, const HIR::Instr *instr) {
        // 1. Store the raw instruction data into sPPUThread::CI
        u32 rawInstr = instr->currentInstrData.opcode;
        COMP->mov(b->GetThreadContext()->scalar(&sPPUThread::CI).Ptr<u32>(), asmjit::Imm(rawInstr));

        // 2. Load the interpreter function pointer into a register
        x86::Gp fnReg = newGP64();
        COMP->mov(fnReg, asmjit::Imm(static_cast<int64_t>(instr->src1.offset)));

        // 3. Invoke the interpreter function: void(sPPEState*)
        asmjit::InvokeNode *invokeNode = nullptr;
        COMP->invoke(&invokeNode, fnReg,
          asmjit::FuncSignature::build<void, void *>());

        // Pass sPPEState* as the first argument
        invokeNode->setArg(0, b->ppeState->Base());
      }

      //
      // Trap
      //

      // Unconditional trap: set program exception and trap type.
      // sig: X  (no dest, no src, trapCode in instr->flags)
      REGISTER_EMITTER(OPCODE_TRAP, Emit_TRAP)
      static void Emit_TRAP(x86CodeGenBackend *b, const HIR::Instr *instr) {
        x86::Gp exceptReg = newGP16();
        COMP->mov(exceptReg, EXPtr());
        COMP->or_(exceptReg, asmjit::Imm(ppuProgramEx));
        COMP->mov(EXPtr(), exceptReg);
        x86::Gp trapType = newGP16();
        COMP->mov(trapType, asmjit::Imm(ppuProgExTypeTRAP));
        COMP->mov(b->GetThreadContext()->scalar(&sPPUThread::progExceptionType), trapType);
      }

      // Conditional trap: if cond != 0, set program exception and trap type.
      // sig: X_V  (no dest, src1 = condition, trapCode in instr->flags)
      REGISTER_EMITTER(OPCODE_TRAP_TRUE, Emit_TRAP_TRUE)
      static void Emit_TRAP_TRUE(x86CodeGenBackend *b, const HIR::Instr *instr) {
        x86::Gp cond = LoadValueGp(b, instr->src1.value);
        Label end = COMP->newLabel();

        COMP->test(cond.r8(), cond.r8());
        COMP->jz(end);

        // Trap: set program exception
        x86::Gp exceptReg = newGP16();
        COMP->mov(exceptReg, EXPtr());
        COMP->or_(exceptReg, asmjit::Imm(ppuProgramEx));
        COMP->mov(EXPtr(), exceptReg);
        x86::Gp trapType = newGP16();
        COMP->mov(trapType, asmjit::Imm(ppuProgExTypeTRAP));
        COMP->mov(b->GetThreadContext()->scalar(&sPPUThread::progExceptionType), trapType);

        COMP->bind(end);
      }

      // 
      // Context Load/Store
      //

      REGISTER_EMITTER(OPCODE_LOAD_CONTEXT, Emit_LOAD_CONTEXT)
      static void Emit_LOAD_CONTEXT(x86CodeGenBackend *b, const HIR::Instr *instr) {
        u64 offset = instr->src1.offset;
        auto *comp = b->GetCompiler();
        auto *ctx = b->GetThreadContext();

        // Create an adjusted ASMJitPtr at the target field offset.
        // The HIR offset comes from offsetof(sPPUThread, field), matching
        // how ASMJitPtr internally computes offsets via member pointers.
        ASMJitPtr<u8> field(ctx->Base(), ctx->Offset() + offset);

        switch (instr->dest->type) {
        case HIR::INT8_TYPE: {
          asmjit::x86::Gp dst = comp->newGpb();
          comp->mov(dst, field.Ptr<u8>());
          TagStoreReg(instr->dest, dst);
        } break;
        case HIR::INT16_TYPE: {
          asmjit::x86::Gp dst = comp->newGpw();
          comp->mov(dst, field.Ptr<u16>());
          TagStoreReg(instr->dest, dst);
        } break;
        case HIR::INT32_TYPE: {
          asmjit::x86::Gp dst = comp->newGpd();
          comp->mov(dst, field.Ptr<u32>());
          TagStoreReg(instr->dest, dst);
        } break;
        case HIR::INT64_TYPE: {
          asmjit::x86::Gp dst = comp->newGpq();
          comp->mov(dst, field.Ptr<u64>());
          TagStoreReg(instr->dest, dst);
        } break;
        case HIR::FLOAT32_TYPE: {
          asmjit::x86::Xmm dst = comp->newXmm();
          comp->vmovss(dst, field.Ptr<f32>());
          TagStoreReg(instr->dest, dst);
        } break;
        case HIR::FLOAT64_TYPE: {
          asmjit::x86::Xmm dst = comp->newXmm();
          comp->vmovsd(dst, field.Ptr<f64>());
          TagStoreReg(instr->dest, dst);
        } break;
        case HIR::VEC128_TYPE: {
          asmjit::x86::Xmm dst = comp->newXmm();
          comp->vmovdqu(dst, field.Ptr<u128>());
          TagStoreReg(instr->dest, dst);
        } break;
        default:
          LOG_WARNING(Xenon, "[x86Backend]: LOAD_CONTEXT unsupported type {}", static_cast<int>(instr->dest->type));
          break;
        }
      }

      REGISTER_EMITTER(OPCODE_STORE_CONTEXT, Emit_STORE_CONTEXT)
      static void Emit_STORE_CONTEXT(x86CodeGenBackend *b, const HIR::Instr *instr) {
        u64 offset = instr->src1.offset;
        HIR::Value *val = instr->src2.value;
        auto *comp = b->GetCompiler();
        auto *ctx = b->GetThreadContext();

        // Create an adjusted ASMJitPtr at the target field offset.
        ASMJitPtr<u8> field(ctx->Base(), ctx->Offset() + offset);

        if (val->IsConstant()) {
          // Store immediate constant directly to context memory.
          switch (val->type) {
          case HIR::INT8_TYPE:
            comp->mov(field.Ptr<u8>(), asmjit::Imm(val->constant.u8));
            break;
          case HIR::INT16_TYPE:
            comp->mov(field.Ptr<u16>(), asmjit::Imm(val->constant.u16));
            break;
          case HIR::INT32_TYPE:
            comp->mov(field.Ptr<u32>(), asmjit::Imm(val->constant.u32));
            break;
          case HIR::INT64_TYPE: {
            asmjit::x86::Gp tmp = comp->newGpq();
            comp->mov(tmp, asmjit::Imm(val->constant.i64));
            comp->mov(field.Ptr<u64>(), tmp);
          } break;
          case HIR::VEC128_TYPE: {
            // Store each 32-bit dword of the 128-bit vector constant individually.
            asmjit::x86::Gp tmp = comp->newGpd();
            for (u32 i = 0; i < 4; i++) {
              ASMJitPtr<u8> dwordField(ctx->Base(), ctx->Offset() + offset + i * sizeof(u32));
              comp->mov(tmp, asmjit::Imm(val->constant.v128.dsword[i]));
              comp->mov(dwordField.Ptr<u32>(), tmp);
            }
          } break;
          default:
            LOG_WARNING(Xenon, "[x86Backend]: STORE_CONTEXT constant type {} not yet handled",
              static_cast<int>(val->type));
            break;
          }
          return;
        }

        // Value produced by a prior instruction — recover the virtual register.
        switch (val->type) {
        case HIR::INT8_TYPE: {
          asmjit::x86::Gp src = TagLoadGp(val);
          comp->mov(field.Ptr<u8>(), src.r8());
        } break;
        case HIR::INT16_TYPE: {
          asmjit::x86::Gp src = TagLoadGp(val);
          comp->mov(field.Ptr<u16>(), src.r16());
        } break;
        case HIR::INT32_TYPE: {
          asmjit::x86::Gp src = TagLoadGp(val);
          comp->mov(field.Ptr<u32>(), src.r32());
        } break;
        case HIR::INT64_TYPE: {
          asmjit::x86::Gp src = TagLoadGp(val);
          comp->mov(field.Ptr<u64>(), src.r64());
        } break;
        case HIR::FLOAT32_TYPE: {
          asmjit::x86::Xmm src = TagLoadXmm(val);
          comp->vmovss(field.Ptr<f32>(), src);
        } break;
        case HIR::FLOAT64_TYPE: {
          asmjit::x86::Xmm src = TagLoadXmm(val);
          comp->vmovsd(field.Ptr<f64>(), src);
        } break;
        case HIR::VEC128_TYPE: {
          asmjit::x86::Xmm src = TagLoadXmm(val);
          comp->vmovdqu(field.Ptr<u128>(), src);
        } break;
        default:
          LOG_WARNING(Xenon, "[x86Backend]: STORE_CONTEXT SSA type {} not yet handled",
            static_cast<int>(val->type));
          break;
        }
      }

      REGISTER_EMITTER(OPCODE_LOAD_TIME_BASE, Emit_LOAD_TIME_BASE)
        static void Emit_LOAD_TIME_BASE(x86CodeGenBackend *b, const HIR::Instr *instr) {
        // Loads time base register
        x86::Gp timeBase = newGP64();
        asmjit::InvokeNode *invokeNode = nullptr;
        COMP->invoke(&invokeNode, reinterpret_cast<void *>(LoadTimeBase), asmjit::FuncSignature::build<u64, PPU *>());
        invokeNode->setArg(0, b->ppuState->Base());
        invokeNode->setRet(0, timeBase);
        TagStoreReg(instr->dest, timeBase);
      }

      // 
      // x86CodeGenBackend implementation
      //

      x86CodeGenBackend::x86CodeGenBackend(asmjit::JitRuntime *runtime, std::mutex *runtimeMutex)
        : jitRuntime(runtime), jitRuntimeMutex(runtimeMutex) {
        emittersTable.fill(nullptr);
      }

      bool x86CodeGenBackend::Initialize() {
        BuildEmitTable();
        return true;
      }

      void x86CodeGenBackend::BuildEmitTable() {
        emittersTable.fill(nullptr);
        for (const auto &key : GetEmitterKeyList()) {
          if (static_cast<u32>(key.opcode) < emittersTable.size()) {
            emittersTable[key.opcode] = key.handler;
          }
        }

        u32 registered = 0;
        for (auto fn : emittersTable) { if (fn) registered++; }
        LOG_DEBUG(Xenon, "[x86Backend]: {} / {} HIR opcodes registered",
          registered, (u32)HIR::__OPCODE_MAX_VALUE);
      }

      // Emits a signle HIR instruction as host code.
      void x86CodeGenBackend::EmitInstruction(const HIR::Instr *instr) {
        if (!instr->opcode) return;

        HIR::Opcode op = instr->opcode->num;
        
        // Check if the opcode is within the bounds of the emitters table before dispatching.
        if (static_cast<u32>(op) >= emittersTable.size()) {
          LOG_WARNING(Xenon, "[x86Backend]: Opcode {} out of range", static_cast<u32>(op));
          return;
        }

        // Memory probe memo lifetime: cleared on any opcode that is not one
        // of the dedupable memory ops. The matching emitter itself is
        // responsible for refreshing the memo with the current EA after a
        // probe. This keeps reuse safe across reorderings or unexpected ops.
        if (op != HIR::OPCODE_LOAD && op != HIR::OPCODE_STORE &&
            op != HIR::OPCODE_MEMSET) {
          ResetProbeMemo();
        }

        // Get emitter from the table
        x86HIREmitFn fn = emittersTable[op];

        // Execute emitter
        if (fn) {
          fn(this, instr);
        } else {
          // Unimplemented, skip silently for ignorable ops, warn for others.
          if (!(instr->opcode->flags & HIR::OPCODE_FLAG_IGNORE)) {
            LOG_WARNING(Xenon, "[x86Backend]: No emitter for HIR opcode '{}' ({})", 
              instr->opcode->name ? instr->opcode->name : "???", static_cast<u32>(op));
          }
        }
      }

      // Emit code for a full HIR block.
      bool x86CodeGenBackend::EmitBlock(HIR::HIRBlock *block, void **outCode, u64 *outCodeSize,
                                        ePPUThreadID threadId, void ***outChainSlot) {
        if (!block || !outCode || !outCodeSize) return false;

        *outCode = nullptr;
        *outCodeSize = 0;
        if (outChainSlot) *outChainSlot = nullptr;

        // Set up asmjit code holder for this block.
        codeHolder.reset();
        codeHolder.init(jitRuntime->environment(), jitRuntime->cpuFeatures());

        // Set logger if we need to do debug print
        if (printGeneratedCode) {
          codeHolder.setLogger(&asmLogger);
        }

        // Init compiler
        asmjit::x86::Compiler comp(&codeHolder);
        compiler = &comp;

        // Function signature: void(PPU*, sPPEState*, bool)
        // Matches JITFunc so HIR blocks can be called from the dispatch loop.
        // Arg0 (PPU*): received but unused by HIR code.
        // Arg1 (sPPEState*): used to derive thread context.
        // Arg2 (bool enableHalt): received but unused for now.
        asmjit::FuncNode *funcNode = nullptr;
        asmjit::Error funNodeInitResult = comp.addFuncNode(&funcNode, asmjit::FuncSignature::build<void, PPU *, sPPEState *, bool>());
        if (funNodeInitResult || !funcNode) {
          LOG_ERROR(Xenon, "[x86Backend]: addFuncNode failed: {}", asmjit::DebugUtils::errorAsString(funNodeInitResult));
          compiler = nullptr;
          return false;
        }

        asmjit::x86::Gp ppuBase = comp.newGpz("ppu");
        funcNode->setArg(0, ppuBase);

        asmjit::x86::Gp ppeBase = comp.newGpz("ppe");
        funcNode->setArg(1, ppeBase);

        asmjit::x86::Gp haltArg = comp.newGpb("halt_unused");
        funcNode->setArg(2, haltArg);

        // Enable AVX
        funcNode->frame().setAvxEnabled();

        // Compute thread context from sPPEState* + baked thread offset
        // This is identical to PPU_JIT::SetupContext.
        u64 threadOffset = static_cast<u8>(threadId) * sizeof(sPPUThread);
        asmjit::x86::Gp ctxBase = comp.newGpz("ctx");
        comp.lea(ctxBase, asmjit::x86::ptr(ppeBase, static_cast<s32>(threadOffset)));

        // Create the context pointer
        ASMJitPtr<sPPUThread> ctxPtr(ctxBase);
        ppuThreadCtx = &ctxPtr;

        // Create the PPU state pointer
        ASMJitPtr<PPU> ppuPtr(ppuBase);
        ppuState = &ppuPtr;

        // Create the PPE state pointer
        ASMJitPtr<sPPEState> ppePtr(ppeBase);
        ppeState = &ppePtr;

        // Reset the per-block probe memo so dedupe never bridges blocks.
        ResetProbeMemo();

        // Walk the instruction linked list and emit host code for each instruction.
        // After instructions that can set exceptReg, emit a bailout check.
        for (const HIR::Instr *instr = block->instr_head; instr; instr = instr->next) {
          EmitInstruction(instr);
        }

        // Block linking tail.
        // When the HIR translator proved the block ends with a branch to a
        // translate-time-constant guest address, allocate one `void *` slot
        // per chainable successor and emit a tail of the form:
        //   if (--thread.jitChainBudget < 0)        skip   // bound recursion
        //   load nia from thread context
        //   if (nia == takenConst && (t = *takenSlot)) call(t); skip
        //   if (nia == fallConst  && (t = *fallSlot )) call(t); skip
        //   skip: ret
        // The dispatcher patches each slot to the destination block's host
        // code pointer once that block is compiled, so the chain tail becomes
        // a direct call into the next block (skipping the dispatcher hashmap).
        // Note: this is a regular call+ret, NOT a true tail jump, so each
        // chain hop adds one host stack frame. The chain budget caps the
        // depth so cycles like B->C->B are bounded.
        void **chainSlot = nullptr;
        const bool hasTaken = outChainSlot && block->chainTargetGuestAddr != HIR::INVALID_CHAIN_TARGET;
        if (hasTaken) {
          chainSlot = new void *(nullptr);

          asmjit::Label skipChain = comp.newLabel();
          asmjit::Label doInvoke = comp.newLabel();

          // Budget gate: if the per-thread chain counter has reached zero,
          // return to the dispatcher instead of recursing further.
          asmjit::x86::Gp budgetReg = comp.newGpd("chain_budget");
          asmjit::x86::Mem budgetMem = ctxPtr.scalar(&sPPUThread::jitChainBudget);
          comp.mov(budgetReg, budgetMem);
          comp.test(budgetReg, budgetReg);
          comp.jz(skipChain);
          comp.dec(budgetReg);
          comp.mov(budgetMem, budgetReg);

          // Load the resolved NIA written by the branch lowering.
          asmjit::x86::Gp niaReg = comp.newGpz("chain_nia");
          comp.mov(niaReg, ctxPtr.scalar(&sPPUThread::NIA));

          asmjit::x86::Gp constReg = comp.newGpz("chain_const");
          asmjit::x86::Gp slotPtrReg = comp.newGpz("chain_slot_ptr");
          asmjit::x86::Gp targetReg = comp.newGpz("chain_target");

          comp.mov(constReg, asmjit::imm(block->chainTargetGuestAddr));
          comp.cmp(niaReg, constReg);
          comp.jne(skipChain);
          comp.mov(slotPtrReg, asmjit::imm((void *)chainSlot));
          comp.mov(targetReg, asmjit::x86::ptr(slotPtrReg));
          comp.test(targetReg, targetReg);
          comp.jz(skipChain);
          comp.jmp(doInvoke);

          comp.bind(doInvoke);
          asmjit::InvokeNode *chainCall = nullptr;
          comp.invoke(&chainCall, targetReg,
            asmjit::FuncSignature::build<void, PPU *, sPPEState *, bool>());
          chainCall->setArg(0, ppuBase);
          chainCall->setArg(1, ppeBase);
          chainCall->setArg(2, haltArg);

          comp.bind(skipChain);
        }

        // Epilogue.
        comp.ret();
        comp.endFunc();

        asmjit::Error err = comp.finalize();
        compiler = nullptr;
        ppuThreadCtx = nullptr;
        ppeState = nullptr;

        if (err) {
          LOG_ERROR(Xenon, "[x86Backend]: asmjit finalize error: {}", asmjit::DebugUtils::errorAsString(err));
          DumpHIR(block);
          delete chainSlot;
          return false;
        }

        if (printGeneratedCode) {
          codeHolder.setLogger(nullptr);
          if (!err) {
            const char *content = asmLogger.content().data();
            if (content && content[0] != '\0') {
              LOG_DEBUG(Xenon, "[x86Backend]: Generated assembly:\n{}", content);
            }
          }
        }



        // Add compiled code to the runtime. The pointer is now owned by the caller
        // via the shared JitRuntime; caller releases with ReleaseCode().
        void *codePtr = nullptr;
        {
          std::unique_lock<std::mutex> lock;
          if (jitRuntimeMutex) lock = std::unique_lock<std::mutex>(*jitRuntimeMutex);
          err = jitRuntime->add(&codePtr, &codeHolder);
        }
        if (err || !codePtr) {
          LOG_ERROR(Xenon, "[x86Backend]: asmjit runtime add error: {}", asmjit::DebugUtils::errorAsString(err));
          delete chainSlot;
          return false;
        }

        // Return generated code and code size
        *outCode = codePtr;
        *outCodeSize = codeHolder.codeSize();
        if (outChainSlot) *outChainSlot = chainSlot;
        return true;
      }

      asmjit::Label x86CodeGenBackend::GetFunctionBlockLabel(HIR::HIRBlock *block) const {
        auto it = functionBlockLabels_.find(block);
        if (it != functionBlockLabels_.end()) return it->second;
        return asmjit::Label();
      }

      // Emit native code for an entire multi-block function (per-function mode).
      // All blocks in the chain are compiled into one CodeHolder so intra-function
      // label jumps (OPCODE_BRANCH_LABEL / OPCODE_BRANCH_TRUE_LABEL) resolve
      // within a single asmjit finalize() without re-entering the dispatcher.
      bool x86CodeGenBackend::EmitFunction(HIR::HIRBlock *blockHead, void **outCode,
                                           u64 *outCodeSize, ePPUThreadID threadId,
                                           void ***outChainSlot) {
        if (!blockHead || !outCode || !outCodeSize) return false;

        *outCode = nullptr;
        *outCodeSize = 0;
        if (outChainSlot) *outChainSlot = nullptr;

        codeHolder.reset();
        codeHolder.init(jitRuntime->environment(), jitRuntime->cpuFeatures());
        if (printGeneratedCode) {
          codeHolder.setLogger(&asmLogger);
        }

        asmjit::x86::Compiler comp(&codeHolder);
        compiler = &comp;

        asmjit::FuncNode *funcNode = nullptr;
        asmjit::Error err = comp.addFuncNode(&funcNode,
          asmjit::FuncSignature::build<void, PPU *, sPPEState *, bool>());
        if (err || !funcNode) {
          LOG_ERROR(Xenon, "[x86Backend]: EmitFunction addFuncNode failed: {}",
            asmjit::DebugUtils::errorAsString(err));
          compiler = nullptr;
          return false;
        }

        asmjit::x86::Gp ppuBase = comp.newGpz("ppu");
        funcNode->setArg(0, ppuBase);
        asmjit::x86::Gp ppeBase = comp.newGpz("ppe");
        funcNode->setArg(1, ppeBase);
        asmjit::x86::Gp haltArg = comp.newGpb("halt_unused");
        funcNode->setArg(2, haltArg);
        funcNode->frame().setAvxEnabled();

        u64 threadOffset = static_cast<u8>(threadId) * sizeof(sPPUThread);
        asmjit::x86::Gp ctxBase = comp.newGpz("ctx");
        comp.lea(ctxBase, asmjit::x86::ptr(ppeBase, static_cast<s32>(threadOffset)));

        ASMJitPtr<sPPUThread> ctxPtr(ctxBase);
        ppuThreadCtx = &ctxPtr;
        ASMJitPtr<PPU> ppuPtr(ppuBase);
        ppuState = &ppuPtr;
        ASMJitPtr<sPPEState> ppePtr(ppeBase);
        ppeState = &ppePtr;

        // First pass: allocate one asmjit label per block so forward branches resolve.
        functionBlockLabels_.clear();
        for (HIR::HIRBlock *b = blockHead; b; b = b->next) {
          functionBlockLabels_[b] = comp.newLabel();
        }

        // Second pass: emit each block.
        for (HIR::HIRBlock *b = blockHead; b; b = b->next) {
          comp.bind(functionBlockLabels_[b]);
          ResetProbeMemo();

          const HIR::Instr *lastInstr = nullptr;
          for (const HIR::Instr *instr = b->instr_head; instr; instr = instr->next) {
            EmitInstruction(instr);
            if (instr->opcode) lastInstr = instr;
          }

          // Determine the block exit type.
          // Walk backward past any trailing SYNC_EXCEPTION_CHECK (which the outer
          // translation loop appends unconditionally for all branch-class instructions,
          // leaving dead code after unconditional jmps). The logical exit instruction
          // that matters is the one BEFORE the trailing checks.
          //
          // OPCODE_BRANCH_LABEL       → unconditional jmp already emitted; no extra ret.
          // OPCODE_BRANCH_TRUE_LABEL  → conditional jcc emitted; fallthrough goes to the
          //                             next block whose label is bound immediately after.
          // Everything else           → external exit (NIA stored); emit ret to dispatcher.
          const HIR::Instr *exitInstr = lastInstr;
          while (exitInstr && exitInstr->opcode &&
                 exitInstr->opcode->num == HIR::OPCODE_SYNC_EXCEPTION_CHECK) {
            exitInstr = exitInstr->prev;
          }
          bool needsRet = true;
          if (exitInstr && exitInstr->opcode) {
            HIR::Opcode exitOp = exitInstr->opcode->num;
            if (exitOp == HIR::OPCODE_BRANCH_LABEL || exitOp == HIR::OPCODE_BRANCH_TRUE_LABEL) {
              needsRet = false;
            }
          }
          if (needsRet) {
            comp.ret();
          }
        }

        comp.endFunc();

        // Labels are no longer needed after finalize; clear before releasing compiler.
        functionBlockLabels_.clear();

        err = comp.finalize();
        compiler = nullptr;
        ppuThreadCtx = nullptr;
        ppeState = nullptr;

        if (err) {
          LOG_ERROR(Xenon, "[x86Backend]: EmitFunction finalize error: {}",
            asmjit::DebugUtils::errorAsString(err));
          return false;
        }

        if (printGeneratedCode) {
          codeHolder.setLogger(nullptr);
          const char *content = asmLogger.content().data();
          if (content && content[0] != '\0') {
            LOG_DEBUG(Xenon, "[x86Backend]: EmitFunction generated assembly:\n{}", content);
          }
        }

        void *codePtr = nullptr;
        {
          std::unique_lock<std::mutex> lock;
          if (jitRuntimeMutex) lock = std::unique_lock<std::mutex>(*jitRuntimeMutex);
          err = jitRuntime->add(&codePtr, &codeHolder);
        }
        if (err || !codePtr) {
          LOG_ERROR(Xenon, "[x86Backend]: EmitFunction runtime add error: {}",
            asmjit::DebugUtils::errorAsString(err));
          return false;
        }

        *outCode = codePtr;
        *outCodeSize = codeHolder.codeSize();
        return true;
      }

      // Releases generated code
      void x86CodeGenBackend::ReleaseCode(void *codePtr) {
        if (codePtr && jitRuntime) {
          std::unique_lock<std::mutex> lock;
          if (jitRuntimeMutex) lock = std::unique_lock<std::mutex>(*jitRuntimeMutex);
          jitRuntime->release(codePtr);
        }
      }

      // Resets compiler, context and holder before each block/function compilation.
      void x86CodeGenBackend::Reset() {
        compiler = nullptr;
        ppuThreadCtx = nullptr;
        ppeState = nullptr;
        codeHolder.reset();
        functionBlockLabels_.clear();
      }

    } // namespace JIT
  } // namespace XCPU
} // namespace Xe

#endif // ARCH_X86 || ARCH_X86_64
