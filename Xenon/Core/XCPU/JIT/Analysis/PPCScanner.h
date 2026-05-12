/***************************************************************/
/* Copyright 2026 Xenon Emulator Project. All rights reserved. */
/***************************************************************/

#pragma once

#include <functional>
#include <vector>

#include "Base/Types.h"

// Forward declarations
struct sPPEState;
enum ePPUThreadID : u8;

namespace Xe {
  namespace XCPU {
    namespace JIT {
      namespace Analysis {

        // PowerPC function/basic-block analyzer. 
        // Walks guest instructions starting at a given entry address, follows branch instructions, and produces a CFG-style report.
        class PPCScanner {
        public:
          // Memory read callback. Returns true if the word at |address| was
          // successfully fetched into |outWord|. Implementations may translate
          // through the guest MMU, read directly from a buffer, etc.
          using ReadU32Fn = std::function<bool(u64 address, u32 &outWord)>;

          // Predicate identifying epilogue-helper entry points such as the
          // AIX/PPC ABI's `__restgprlr_NN` family. Branches to such helpers
          // are tail calls that effectively end the calling function, so the
          // scanner treats them as clean exits rather than extending the
          // function range. May be empty (always returns false).
          using IsRestGprLrFn = std::function<bool(u64 targetAddress)>;

          enum class EdgeKind : u8 {
            Fallthrough,        // implicit edge to addr+4 after a non-branch
            DirectBranch,       // unconditional b
            ConditionalBranch,  // bc (taken edge)
            Call,               // bl / bcl  (LK=1 direct)
            IndirectBranch,     // bcctr (LK=0)
            IndirectCall,       // bcctrl / bclrl (LK=1)
            Return,             // bclr (LK=0, treated as return)
            Syscall,            // sc
            Rfid,               // rfid
          };

          struct EdgeInfo {
            u64 fromAddress;  // address of the source instruction
            u64 toAddress;    // resolved target, or 0 if indirect/unknown
            EdgeKind kind;
          };

          struct BlockInfo {
            u64 startAddress;     // first instruction in the block
            u64 endAddress;       // last instruction in the block (inclusive)
            u32 endingOpcode;     // raw word at endAddress
            bool endsWithBranch;  // any branch-class terminator
            bool endsWithReturn;  // bclr (no remaining forward target)
            bool endsWithIndirect;// bcctr / bclrl etc.
            bool endsWithSyscall;
            bool endsWithRfid;
          };

          struct ScanResult {
            // Function extent.
            u64 startAddress;
            u64 endAddress;          // last instruction inside the function (inclusive)
            u64 instructionCount;

            // Termination reason.
            bool reachedCleanEnd;    // hit blr / bctr-as-return / heuristic end
            bool ranOver;            // hit instruction or range cap
            bool hitInvalidWord;     // 0x00000000 / 0xFFFFFFFF / read failure

            // Heuristic flags.
            bool startsWithMfsprLR;  // "mfspr rN, LR" at entry — typical prologue
            bool hasJumpTableSuspect;// non-terminal bcctr (LK=0) inside the function
            bool hasIndirectBranch;  // bcctr/bclr LK=0 that isn't the canonical return —
                                     // structurally significant (jump table / computed
                                     // tail call); breaks per-function CFG completeness
            bool hasIndirectCall;    // bcctrl/bclrl LK=1 — vtable / function-pointer
                                     // call; harmless for function-mode JIT (treated
                                     // as an external edge, like a regular `bl`)
            bool hasMsrChange;       // mtmsrd encountered
            bool hasSyscall;         // sc encountered
            bool hasRfid;            // rfid encountered
            bool hasInvalidInstrs;   // any decoded-as-invalid words inside the body

            // CFG.
            std::vector<BlockInfo> blocks;
            std::vector<EdgeInfo> edges;

            // External references.
            std::vector<u64> callTargets;     // bl / bcl direct call targets
            std::vector<u64> tailCallTargets; // unconditional b's that look like tail calls
          };

          struct ScanOptions {
            // Max instructions possible on a scan, prevents garbage data from sneaking in on malformed functions
            u64 maxInstructions = 4096;
            // Max data size we can take in for a given analysis
            u64 maxRangeBytes = 64 * 1024;
            // If true, stop the scan when an obviously invalid instr is read (garbage data, heap fill on debug). When false, treat
            // as no-ops and keep walking
            bool stopOnInvalidWord = true;
            // If true, emit per-instruction LOG_DEBUG lines as the scanner walks. Otherwise only a summary is logged when DumpResult runs.
            bool verboseTrace = false;
            // Optional predicate flagging a guest address as an epilogue helper (e.g. `__restgprlr_*`). When set and matched on a
            // branch target, the scanner treats the branch as a clean function exit and records it as a tail call.
            // Most of the functions in guest code end with such an epilogue.
            IsRestGprLrFn isRestGprLr;
            // Maximum forward distance (bytes) that a conditional branch (bc) may add a new
            // block to the worklist. A forward bc beyond this distance is recorded as an edge
            // but the target is treated as an external escape (e.g. "bgt- KiBugCheckIrqlNotGreaterOrEqual")
            // and not scanned as part of the function. Default 4096 bytes (~1024 instrs) covers
            // virtually all legitimate in-function out-of-line blocks while suppressing IRQL/assert
            // error-handler escapes that are typically tens of KB away.
            // Set to 0 to disable the cap (needed for unusually large functions with far cold paths).
            u64 maxBcForwardExtension = 4096;
          };

          explicit PPCScanner(ReadU32Fn reader);

          // Run the scan
          ScanResult Scan(u64 startAddress, const ScanOptions &options = ScanOptions{}) const;

          // Build a reader that pulls instruction words through the guest MMU
          // for the given PPE state. The reader temporarily borrows the
          // |threadId| slot's exception/instr-fetch fields and restores them
          // when done; pick the slot of the thread that owns the translation
          // so other PPU threads aren't disturbed. Safe to pass nullptr
          // (returns a reader that always reports failure).
          static ReadU32Fn MakeMMUReader(sPPEState *ppeState,
                                         ePPUThreadID threadId);

          // Build a memoized predicate that auto-detects `__restgprlr_*` /
          // `__restfpr_*` style epilogue helpers by inspecting the first
          // few instructions at the target address through |reader|. The
          // helpers are stereotyped: a short straight-line sequence ending
          // in `mtlr` (or pre-existing LR restore) followed by `blr`, with
          // no intervening branches. Results are cached per address.
          static IsRestGprLrFn MakeAutoRestGprLrDetector(ReadU32Fn reader);

          // Pretty-print the scan result via the project's logging system.
          // Output level is LOG_INFO so it shows up by default.
          static void DumpResult(const ScanResult &result);

        private:
          ReadU32Fn reader_;
        };

      }  // namespace Analysis
    }  // namespace JIT
  }  // namespace XCPU
}  // namespace Xe
