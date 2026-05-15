/***************************************************************/
/* Copyright 2026 Xenon Emulator Project. All rights reserved. */
/***************************************************************/

#include "Core/XCPU/JIT/Analysis/PPCScanner.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <mutex>
#include <set>
#include <unordered_map>

#include "Base/Logging/Log.h"
#include "Core/XCPU/Interpreter/PPCInterpreter.h"

namespace Xe {
  namespace XCPU {
    namespace JIT {
      namespace Analysis {

        namespace {

          // 
          // PPC instruction-word decoding helpers.
          //
          
          // The scanner only needs to recognize branch and a handful of marker instructions, so we decode raw words 
          // inline rather than using the usual decoder as in the HIR/JIT/Interpreter

          constexpr u32 kPrimaryBC   = 0x10;  // bc / bca / bcl / bcla
          constexpr u32 kPrimarySC   = 0x11;  // sc
          constexpr u32 kPrimaryB    = 0x12;  // b  / ba  / bl  / bla
          constexpr u32 kPrimaryX13  = 0x13;  // bclr/bcctr/rfid family
          constexpr u32 kPrimaryX1F  = 0x1F;  // mfspr/mtmsrd family

          constexpr u32 kXOBclr   = 0x010;
          constexpr u32 kXORfid   = 0x012;
          constexpr u32 kXOBcctr  = 0x210;
          constexpr u32 kXOMfspr  = 0x153;
          constexpr u32 kXOMtmsrd = 0x0B2;

          constexpr u32 kBlrEncoding  = 0x4E800020;  // bclr  with BO=20, BI=0, LK=0
          constexpr u32 kBctrEncoding = 0x4E800420;  // bcctr with BO=20, BI=0, LK=0

          // Helpers to get the primary and XO forms from a given instr data

          inline u32 PrimaryOpcode(u32 code) { return (code >> 26) & 0x3F; }
          inline u32 XOForm(u32 code) { return (code >> 1) & 0x3FF; }

          // Sign extension
          inline s64 SignExtend(u64 value, u32 bits) {
            const u64 mask = (1ULL << bits) - 1ULL;
            value &= mask;
            const u64 signBit = 1ULL << (bits - 1);
            return static_cast<s64>((value ^ signBit) - signBit);
          }

          // I-form target for b/bl/ba/bla
          inline u64 ResolveBranchITarget(u32 code, u64 address) {
            const s64 disp = SignExtend(static_cast<u64>(code & 0x03FFFFFC), 26);
            const bool aa = (code & 0x2) != 0;
            return aa ? static_cast<u64>(disp)
                      : static_cast<u64>(static_cast<s64>(address) + disp);
          }

          // B-form target for bc/bcl/bca/bcla
          inline u64 ResolveBranchBTarget(u32 code, u64 address) {
            const s64 disp = SignExtend(static_cast<u64>(code & 0x0000FFFC), 16);
            const bool aa = (code & 0x2) != 0;
            return aa ? static_cast<u64>(disp)
                      : static_cast<u64>(static_cast<s64>(address) + disp);
          }

          // True if the instruction has the LK write enabled
          inline bool IsLK(u32 code) { return (code & 0x1) != 0; }

          // True if the entry instruction is "mfspr rN, LR", which is the canonical first instruction of a non-leaf
          // PPC function prologue
          inline bool IsMfsprLR(u32 code) {
            if (PrimaryOpcode(code) != kPrimaryX1F) return false;
            if (XOForm(code) != kXOMfspr) return false;
            // SPR field is encoded with the two 5-bit halves swapped.
            const u32 sprField = (code >> 11) & 0x3FF;
            const u32 actualSPR = ((sprField & 0x1F) << 5) | ((sprField >> 5) & 0x1F);
            return actualSPR == 8;  // 8 = LR
          }

          // True if the instruction is = mtspr LR (XO=467) targeting SPR=8 (LR). 
          inline bool IsMtlr(u32 code) {
            if (PrimaryOpcode(code) != kPrimaryX1F) return false;
            if (XOForm(code) != 0x1D3) return false;  // XO=467
            const u32 sprField = (code >> 11) & 0x3FF;
            const u32 actualSPR = ((sprField & 0x1F) << 5) |
                                  ((sprField >> 5) & 0x1F);
            return actualSPR == 8;
          }

          // Returns true if the given instr is any form of branch instruction.
          // Used by the epilogue-helper detector to bail on the first branch encountered other than the trailing `blr`
          inline bool IsBranchClass(u32 code) {
            const u32 primary = PrimaryOpcode(code);
            if (primary == kPrimaryB || primary == kPrimaryBC || primary == kPrimarySC) { return true; }
            if (primary == kPrimaryX13) {
              const u32 xo = XOForm(code);
              return xo == kXOBclr || xo == kXOBcctr || xo == kXORfid;
            }
            return false;
          }

          // Obviously invalid data, can't be an intruction
          inline bool IsObviouslyInvalid(u32 code) { return code == 0x00000000 || code == 0xFFFFFFFF ||code == 0xCDCDCDCD; }

          // Kind of possible edges names
          const char *EdgeKindName(PPCScanner::EdgeKind kind) {
            switch (kind) {
              case PPCScanner::EdgeKind::Fallthrough:        return "fallthrough";
              case PPCScanner::EdgeKind::DirectBranch:       return "b";
              case PPCScanner::EdgeKind::ConditionalBranch:  return "bc";
              case PPCScanner::EdgeKind::Call:               return "bl";
              case PPCScanner::EdgeKind::IndirectBranch:     return "bcctr";
              case PPCScanner::EdgeKind::IndirectCall:       return "bcctrl";
              case PPCScanner::EdgeKind::Return:             return "blr";
              case PPCScanner::EdgeKind::Syscall:            return "sc";
              case PPCScanner::EdgeKind::Rfid:               return "rfid";
            }
            return "?";
          }

        }  // namespace

        // Constructor
        PPCScanner::PPCScanner(ReadU32Fn reader) : reader_(std::move(reader)) {}

        // Creates a new MMU Reader (see header)
        PPCScanner::ReadU32Fn PPCScanner::MakeMMUReader(sPPEState *ppeState, ePPUThreadID threadId) {
          // Safety check
          if (!ppeState || threadId >= ePPUThread_None) { return [](u64, u32 &) { return false; }; }
          
          // If we encounter an exception, we bail out without altering the state, save a thread snapshot of the
          // possibly altered regs before the read and restore it afterwards.
          return [ppeState, threadId](u64 address, u32 &outWord) -> bool {
            auto &thread = ppeState->ppuThread[static_cast<u8>(threadId)];
            const u16 savedExcept = thread.exceptReg;
            const bool savedFetch = thread.instrFetch;
            thread.instrFetch = true;
            const u32 word = PPCInterpreter::MMURead32(ppeState, address);
            thread.instrFetch = savedFetch;
            const bool faulted = (thread.exceptReg & ~savedExcept) != 0;
            thread.exceptReg = savedExcept;
            if (faulted) { return false; }
            outWord = word;
            return true;
          };
        }

        PPCScanner::IsRestGprLrFn PPCScanner::MakeAutoRestGprLrDetector(
            ReadU32Fn reader) {
          if (!reader) {
            return [](u64) { return false; };
          }
          // Cache results so a hot dispatcher doesn't re-probe the same
          // helpers on every scan. The cache lives for the predicate's
          // lifetime; callers are expected to discard the predicate when
          // the underlying code memory is invalidated.
          struct Cache {
            std::mutex mutex;
            std::unordered_map<u64, bool> entries;
          };
          auto cache = std::make_shared<Cache>();
          return [reader, cache](u64 targetAddress) -> bool {
            {
              std::lock_guard<std::mutex> lock(cache->mutex);
              auto it = cache->entries.find(targetAddress);
              if (it != cache->entries.end()) {
                return it->second;
              }
            }
            // Walk up to a small window of straight-line instructions
            // looking for a trailing `blr` with no branches in between.
            // The canonical helpers restore GPRs r14..r31 (up to 18 ld/lwz
            // instructions) plus a LR restore and blr, so allow up to 36
            // instructions to cover __restgprlr_14 and similar large variants.
            constexpr u32 kMaxWindow = 36;
            bool isHelper = false;
            bool sawMtlr = false;
            bool readFailed = false;
            for (u32 i = 0; i < kMaxWindow; ++i) {
              const u64 addr = targetAddress + (static_cast<u64>(i) * 4);
              u32 code = 0;
              if (!reader(addr, code)) {
                // A read failure means we could not verify the stereotype.
                // Don't cache the negative result — the failure may be
                // transient (early boot, page not yet mapped) and caching
                // false would permanently mis-classify the helper.
                readFailed = true;
                break;
              }
              if (IsObviouslyInvalid(code)) {
                break;
              }
              if (IsMtlr(code)) {
                sawMtlr = true;
                continue;
              }
              if (code == kBlrEncoding) {
                // Helpers either restore LR explicitly (sawMtlr) or are
                // tail-only stubs (rare, but allow them: a leading `blr`
                // means a no-op restorer). Either way, the absence of any
                // intervening branch confirms the stereotype.
                isHelper = sawMtlr || (i > 0);
                break;
              }
              if (IsBranchClass(code)) {
                // Any other branch breaks the stereotype.
                break;
              }
            }
            // Only cache a definitive answer; skip caching on read failure so
            // a later call can re-probe once the address is accessible.
            if (!readFailed) {
              std::lock_guard<std::mutex> lock(cache->mutex);
              cache->entries.emplace(targetAddress, isHelper);
            }
            return isHelper;
          };
        }

        PPCScanner::ScanResult PPCScanner::Scan(u64 startAddress,
                                                const ScanOptions &options) const {
          ScanResult result{};
          result.startAddress = startAddress;
          result.endAddress = startAddress;

          if (!reader_) {
            LOG_WARNING(Xenon, "[PPCScanner]: Scan called without a reader; aborting");
            return result;
          }

          u64 address = startAddress;
          // Sorted set of forward branch targets that have not yet been reached
          // by the linear scan. Replaces the old single-value furthestTarget so
          // that dead code between an unconditional branch and the next live block
          // can be skipped — otherwise forward branches in unreachable instructions
          // keep pushing the horizon until the instruction cap fires.
          std::set<u64> pendingTargets;
          u64 instructionCount = 0;
          bool inBlock = false;
          BlockInfo currentBlock{};
          size_t blocksFound = 0;
          bool endFunction = false;

          // Returns true if any recorded forward target lies ahead of address.
          const auto hasPendingAhead = [&]() -> bool {
            return pendingTargets.upper_bound(address) != pendingTargets.end();
          };
          // Inserts a forward branch target; no-ops for backward/same targets.
          const auto addPending = [&](u64 target) {
            if (target > address) pendingTargets.insert(target);
          };

          const u64 endRangeAddress = startAddress + options.maxRangeBytes;

          while (true) {
            if (instructionCount >= options.maxInstructions) {
              LOG_WARNING(Xenon, "[PPCScanner]: hit instruction cap ({}) at {:#x}",
                          options.maxInstructions, address);
              result.ranOver = true;
              break;
            }
            if (address >= endRangeAddress) {
              LOG_WARNING(Xenon, "[PPCScanner]: hit range cap ({:#x} bytes) at {:#x}",
                          options.maxRangeBytes, address);
              result.ranOver = true;
              break;
            }

            // Guard: if the scanner is about to enter a new block (not yet inBlock)
            // at an address other than the function entry, check whether it's an
            // epilogue helper that leaked into pendingTargets. This catches helpers
            // that reached the worklist via a forward `bc` or a `b` whose
            // isRestGprLr probe was inconclusive (e.g. transient read failure).
            if (address != startAddress && !inBlock &&
                options.isRestGprLr && options.isRestGprLr(address)) {
              if (options.verboseTrace) {
                LOG_DEBUG(Xenon,
                          "[PPCScanner]: pending target {:#x} is an epilogue helper; stopping cleanly",
                          address);
              }
              result.reachedCleanEnd = true;
              break;
            }

            u32 code = 0;
            if (!reader_(address, code)) {
              LOG_WARNING(Xenon, "[PPCScanner]: read failure at {:#x}", address);
              result.hitInvalidWord = true;
              break;
            }

            if (IsObviouslyInvalid(code)) {
              if (options.verboseTrace) {
                LOG_DEBUG(Xenon, "[PPCScanner]: padding/invalid word {:#010x} at {:#x}",
                          code, address);
              }
              result.hasInvalidInstrs = true;
              if (options.stopOnInvalidWord) {
                result.hitInvalidWord = true;
                // Rewind: padding shouldn't be considered part of the function.
                if (address > startAddress) {
                  address -= 4;
                }
                break;
              }
            }

            if (!inBlock) {
              inBlock = true;
              currentBlock = BlockInfo{};
              currentBlock.startAddress = address;
              ++blocksFound;
            }

            if (address == startAddress && IsMfsprLR(code)) {
              result.startsWithMfsprLR = true;
            }

            const u32 primary = PrimaryOpcode(code);
            bool endsBlock = false;
            bool isUnconditionalBlock = false;  // set for branches with no fallthrough

            currentBlock.endAddress = address;
            currentBlock.endingOpcode = code;

            if (code == kBlrEncoding) {
              // Unconditional return.
              result.edges.push_back({address, 0, EdgeKind::Return});
              currentBlock.endsWithReturn = true;
              currentBlock.endsWithBranch = true;
              isUnconditionalBlock = true;
              if (hasPendingAhead()) {
                if (options.verboseTrace) {
                  const u64 nextPending = *pendingTargets.upper_bound(address);
                  LOG_DEBUG(Xenon, "[PPCScanner]: ignoring blr at {:#x} (pending targets remain, next: {:#x})",
                            address, nextPending);
                }
              } else {
                if (options.verboseTrace) {
                  LOG_DEBUG(Xenon, "[PPCScanner]: function end (blr) at {:#x}", address);
                }
                endFunction = true;
                result.reachedCleanEnd = true;
              }
              endsBlock = true;
            } else if (code == kBctrEncoding) {
              // Unconditional indirect branch — usually a jump table or tail
              // call through a function pointer.
              result.hasIndirectBranch = true;
              result.edges.push_back({address, 0, EdgeKind::IndirectBranch});
              currentBlock.endsWithIndirect = true;
              currentBlock.endsWithBranch = true;
              isUnconditionalBlock = true;
              if (hasPendingAhead()) {
                // Inside the function: definitely a jump table candidate.
                result.hasJumpTableSuspect = true;
              } else {
                // No remaining forward targets — treat as function exit.
                endFunction = true;
                result.reachedCleanEnd = true;
                if (options.verboseTrace) {
                  LOG_DEBUG(Xenon, "[PPCScanner]: function end (bctr tail) at {:#x}", address);
                }
              }
              endsBlock = true;
            } else if (primary == kPrimaryB) {
              // b / bl / ba / bla.
              const u64 target = ResolveBranchITarget(code, address);
              const bool link = IsLK(code);
              if (link) {
                result.edges.push_back({address, target, EdgeKind::Call});
                result.callTargets.push_back(target);
                if (options.verboseTrace) {
                  LOG_DEBUG(Xenon, "[PPCScanner]: bl  {:#x} -> {:#x}", address, target);
                }
              } else {
                result.edges.push_back({address, target, EdgeKind::DirectBranch});
                isUnconditionalBlock = true;
                if (options.verboseTrace) {
                  LOG_DEBUG(Xenon, "[PPCScanner]: b   {:#x} -> {:#x}", address, target);
                }
                // Backward branch into the function with no remaining forward
                // targets ⇒ end of function. If there are still pending targets
                // the unconditional-block jump below handles resuming at the next
                // live block, so we only set endFunction when the slate is clear.
                if (target >= startAddress && target < address && !hasPendingAhead()) {
                  endFunction = true;
                  result.reachedCleanEnd = true;
                }
                // Forward branch outside the function range ⇒ external tail call.
                // Always record, but only terminate if no pending targets remain.
                if (!endFunction && target < startAddress) {
                  result.tailCallTargets.push_back(target);
                  if (!hasPendingAhead()) {
                    endFunction = true;
                    result.reachedCleanEnd = true;
                  }
                }
                // Branch to a `__restgprlr_*`-style epilogue helper always ends
                // the function — it's a definitive tail call regardless of any
                // pending forward targets (those would be dead code).
                if (!endFunction && options.isRestGprLr && options.isRestGprLr(target)) {
                  result.tailCallTargets.push_back(target);
                  endFunction = true;
                  result.reachedCleanEnd = true;
                  if (options.verboseTrace) {
                    LOG_DEBUG(Xenon,
                              "[PPCScanner]: __restgprlr_* tail at {:#x} -> {:#x}",
                              address, target);
                  }
                }
                // Heuristic: a single-block leaf-style "li r3, ...; b foo"
                // is almost certainly a thunk.
                if (!endFunction && !result.startsWithMfsprLR && blocksFound == 1) {
                  result.tailCallTargets.push_back(target);
                  endFunction = true;
                  result.reachedCleanEnd = true;
                  if (options.verboseTrace) {
                    LOG_DEBUG(Xenon, "[PPCScanner]: leaf-thunk heuristic at {:#x}", address);
                  }
                }
                // Forward b whose target lies beyond the scan window is
                // definitionally external — an epilogue helper (__restgprlr_N,
                // etc.) or a tail call to a distant function. We cannot scan
                // it regardless of pendingTargets, so always record as a tail
                // call. No startsWithMfsprLR guard: even non-leaf functions
                // legitimately end with "b __restgprlr_N" 673 KB away.
                if (!endFunction && target > address && target >= endRangeAddress) {
                  result.tailCallTargets.push_back(target);
                  if (!hasPendingAhead()) {
                    endFunction = true;
                    result.reachedCleanEnd = true;
                  }
                  if (options.verboseTrace) {
                    LOG_DEBUG(Xenon, "[PPCScanner]: out-of-range tail-call at {:#x} -> {:#x}",
                              address, target);
                  }
                }
                // Forward unconditional b with no pending forward edges and no
                // mfspr-LR prologue ⇒ tail call. Catches "loop + tail jump" patterns
                // like: bc back_to_top / b far_function.
                if (!endFunction && target > address && !hasPendingAhead() &&
                    !result.startsWithMfsprLR) {
                  result.tailCallTargets.push_back(target);
                  endFunction = true;
                  result.reachedCleanEnd = true;
                  if (options.verboseTrace) {
                    LOG_DEBUG(Xenon, "[PPCScanner]: forward-escape tail-call at {:#x} -> {:#x}",
                              address, target);
                  }
                }
                if (!endFunction) {
                  addPending(target);
                }
                currentBlock.endsWithBranch = true;
              }
              endsBlock = true;
            } else if (primary == kPrimaryBC) {
              // bc / bcl / bca / bcla.
              const u64 target = ResolveBranchBTarget(code, address);
              const bool link = IsLK(code);
              if (link) {
                result.edges.push_back({address, target, EdgeKind::Call});
                result.callTargets.push_back(target);
                if (options.verboseTrace) {
                  LOG_DEBUG(Xenon, "[PPCScanner]: bcl {:#x} -> {:#x}", address, target);
                }
              } else {
                result.edges.push_back({address, target, EdgeKind::ConditionalBranch});
                // A conditional branch to an epilogue helper (__restgprlr_*, etc.)
                // is a conditional tail-exit. Record the target as a tail call but
                // never add it to pendingTargets — scanning the helper body as
                // function code would corrupt the CFG and trigger the range cap.
                const bool isEpilogueExit =
                    options.isRestGprLr && options.isRestGprLr(target);
                if (isEpilogueExit) {
                  result.tailCallTargets.push_back(target);
                  if (options.verboseTrace) {
                    LOG_DEBUG(Xenon, "[PPCScanner]: bc  {:#x} -> {:#x} [epilogue-exit]",
                              address, target);
                  }
                } else {
                  // A forward bc beyond maxBcForwardExtension bytes is treated as an
                  // external escape (error handler, assert, etc.) — recorded as an edge
                  // but NOT added to pendingTargets so the scanner never visits that block.
                  const bool withinLimit =
                      target <= address ||
                      options.maxBcForwardExtension == 0 ||
                      (target - address) <= options.maxBcForwardExtension;
                  if (withinLimit) {
                    addPending(target);
                  }
                  if (options.verboseTrace) {
                    LOG_DEBUG(Xenon, "[PPCScanner]: bc  {:#x} -> {:#x}{}",
                              address, target, withinLimit ? "" : " [horizon-capped]");
                  }
                }
                currentBlock.endsWithBranch = true;
              }
              endsBlock = true;
            } else if (primary == kPrimaryX13) {
              const u32 xo = XOForm(code);
              if (xo == kXOBclr) {
                // Generic bclr/bclrl that wasn't the canonical 0x4E800020.
                // LK=1 is an indirect call through LR (rare but legal).
                // LK=0 is a computed return path (typical conditional return).
                const bool link = IsLK(code);
                result.edges.push_back({
                    address, 0,
                    link ? EdgeKind::IndirectCall : EdgeKind::Return,
                });
                if (link) {
                  result.hasIndirectCall = true;
                } else {
                  result.hasIndirectBranch = true;
                  isUnconditionalBlock = true;
                }
                currentBlock.endsWithIndirect = !link;
                currentBlock.endsWithReturn = !link;
                currentBlock.endsWithBranch = true;
                endsBlock = true;
              } else if (xo == kXOBcctr) {
                // bcctrl (LK=1) is an indirect call — vtable / fn-pointer
                // dispatch. Structurally equivalent to `bl` for CFG purposes
                // (external edge), so we DO NOT set hasIndirectBranch.
                // bcctr  (LK=0) is a true indirect branch — jump table or
                // computed tail call — and breaks per-function CFG closure.
                const bool link = IsLK(code);
                result.edges.push_back({
                    address, 0,
                    link ? EdgeKind::IndirectCall : EdgeKind::IndirectBranch,
                });
                if (link) {
                  result.hasIndirectCall = true;
                } else {
                  result.hasIndirectBranch = true;
                  result.hasJumpTableSuspect = true;
                  isUnconditionalBlock = true;
                }
                currentBlock.endsWithIndirect = true;
                currentBlock.endsWithBranch = true;
                endsBlock = true;
              } else if (xo == kXORfid) {
                result.edges.push_back({address, 0, EdgeKind::Rfid});
                result.hasRfid = true;
                currentBlock.endsWithRfid = true;
                currentBlock.endsWithBranch = true;
                endFunction = true;
                result.reachedCleanEnd = true;
                endsBlock = true;
              }
            } else if (primary == kPrimarySC) {
              result.edges.push_back({address, 0, EdgeKind::Syscall});
              result.hasSyscall = true;
              currentBlock.endsWithSyscall = true;
              currentBlock.endsWithBranch = true;
              endsBlock = true;
            } else if (primary == kPrimaryX1F && XOForm(code) == kXOMtmsrd) {
              // Note an MSR-changing instruction; doesn't end the block but
              // matters for per-function feasibility (SF transitions).
              result.hasMsrChange = true;
            }

            ++instructionCount;

            if (endsBlock) {
              result.blocks.push_back(currentBlock);
              inBlock = false;
            }

            if (endFunction) {
              break;
            }

            // After any unconditional branch (blr, b, bctr, bclr, bcctr — no
            // fallthrough), skip dead code between this branch and the next
            // live block. Without this, forward branches inside unreachable
            // instructions keep pushing the scan horizon until the cap fires.
            if (endsBlock && isUnconditionalBlock) {
              const auto nextIt = pendingTargets.upper_bound(address);
              if (nextIt == pendingTargets.end()) {
                // All pending targets have been visited — function is complete.
                result.reachedCleanEnd = true;
                break;
              }
              address = *nextIt;
              continue;  // Jump directly to the next reachable block.
            }

            address += 4;
          }

          // Close any block that was still open (e.g. ran-over termination).
          if (inBlock) {
            result.blocks.push_back(currentBlock);
          }

          // Derive final extent from the blocks that were actually visited
          // (the worklist may visit blocks out of linear order, so 'address'
          // at loop exit may not be the highest address scanned).
          result.endAddress = address;
          for (const auto &blk : result.blocks) {
            result.endAddress = std::max(result.endAddress, blk.endAddress);
          }
          result.instructionCount = instructionCount;
          return result;
        }

        void PPCScanner::DumpResult(const ScanResult &result) {
          LOG_INFO(Xenon, "[PPCScanner]: === Scan {:#x}..{:#x} ({} instrs, {} blocks) ===",
                   result.startAddress, result.endAddress,
                   result.instructionCount, result.blocks.size());
          LOG_INFO(Xenon, "[PPCScanner]:   reachedCleanEnd={}  ranOver={}  hitInvalid={}",
                   result.reachedCleanEnd, result.ranOver, result.hitInvalidWord);
          
          LOG_INFO(Xenon, "[PPCScanner]:   prologueMfsprLR={}  msrChange={}  syscall={}  rfid={}",
                   result.startsWithMfsprLR, result.hasMsrChange,
                   result.hasSyscall, result.hasRfid);
          LOG_INFO(Xenon, "[PPCScanner]:   indirectBranch={}  indirectCall={}  jumpTableSuspect={}  invalidInstrs={}",
                   result.hasIndirectBranch, result.hasIndirectCall,
                   result.hasJumpTableSuspect, result.hasInvalidInstrs);

          LOG_INFO(Xenon, "[PPCScanner]:   blocks ({}):", result.blocks.size());
          for (const BlockInfo &b : result.blocks) {
            LOG_INFO(Xenon, "[PPCScanner]:     {:#x}..{:#x}  end={:#010x}  branch={} ret={} indirect={} sc={} rfid={}",
                     b.startAddress, b.endAddress, b.endingOpcode,
                     b.endsWithBranch, b.endsWithReturn, b.endsWithIndirect,
                     b.endsWithSyscall, b.endsWithRfid);
          }

          LOG_INFO(Xenon, "[PPCScanner]:   edges ({}):", result.edges.size());
          for (const EdgeInfo &e : result.edges) {
            if (e.toAddress) {
              LOG_INFO(Xenon, "[PPCScanner]:     {:#x} --{}--> {:#x}",
                       e.fromAddress, EdgeKindName(e.kind), e.toAddress);
            } else {
              LOG_INFO(Xenon, "[PPCScanner]:     {:#x} --{}--> (indirect)",
                       e.fromAddress, EdgeKindName(e.kind));
            }
          }

          if (!result.callTargets.empty()) {
            LOG_INFO(Xenon, "[PPCScanner]:   call targets ({}):", result.callTargets.size());
            for (u64 target : result.callTargets) {
              LOG_INFO(Xenon, "[PPCScanner]:     bl -> {:#x}", target);
            }
          }
          if (!result.tailCallTargets.empty()) {
            LOG_INFO(Xenon, "[PPCScanner]:   tail-call targets ({}):", result.tailCallTargets.size());
            for (u64 target : result.tailCallTargets) {
              LOG_INFO(Xenon, "[PPCScanner]:     b  -> {:#x}", target);
            }
          }
          LOG_INFO(Xenon, "[PPCScanner]: === End scan ===");
        }

      }  // namespace Analysis
    }  // namespace JIT
  }  // namespace XCPU
}  // namespace Xe
