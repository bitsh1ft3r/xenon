/***************************************************************/
/* Copyright 2025 Xenon Emulator Project. All rights reserved. */
/***************************************************************/

#include <algorithm>

#include "Base/Logging/Log.h"
#include "Base/Global.h"

#if defined(ARCH_X86) || defined(ARCH_X86_64)
#include "Core/XCPU/JIT/x86_64/JITEmitter_Helpers.h"
#include "Core/XCPU/JIT/Backend/X86Backend/x86CodeGenBackend.h"
#endif
#include "Core/XCPU/JIT/HIR/PPCTranslator.h"
#include "Core/XCPU/Interpreter/PPCInterpreter.h"
#include "Core/XCPU/PPU/PPCInternal.h"
#include "Core/XCPU/XenonCPU.h"
#include "Core/XCPU/PPU/PPU.h"
#include "Core/XeMain.h"
#include "PPU_JIT.h"

//
//  Trampolines for Invoke
//
void callHalt() {
  return XeMain::GetCPU()->Halt();
}

// Constructor
PPU_JIT::PPU_JIT(PPU *ppu) :
  ppu(ppu),
  ppeState(ppu->ppeState.get()) {}

// Destructor
PPU_JIT::~PPU_JIT() {
  for (u8 t = 0; t < 2; t++) {
    auto &cache = threadCaches[t];
    // At destruction no threads are running, but lock for safety.
    std::lock_guard<std::mutex> lock(cache.invalidationMutex);
    cache.chainTargetSlots.clear();
    cache.chainSourceIndex.clear();
    for (auto &[hash, block] : cache.jitBlocksCache)
      block.reset();
    cache.jitBlocksCache.clear();
    cache.fastBlockCache.invalidateAll();
    cache.pageBlockIndex.clear();
    cache.blockPageList.clear();
  }
}


//
// HIR backend path
//

void PPU_JIT::EnsureHIRPipeline() {
  if (hirReady_.load(std::memory_order_acquire)) return;
  std::call_once(hirInitFlag_, [this]() {
#if defined(ARCH_X86) || defined(ARCH_X86_64)
    // Create per-thread backends to avoid shared mutable state (codeHolder, compiler, etc.)
    for (u8 t = 0; t < 2; t++) {
      auto backend = std::make_unique<Xe::XCPU::JIT::x86CodeGenBackend>(&jitRuntime, &jitRuntimeMutex);
      if (!backend->Initialize()) {
        LOG_ERROR(Xenon, "[JIT]: Failed to initialize HIR x86 backend for thread {}", t);
        return;
      }
      hirBackend_[t] = std::move(backend);
      hirTranslator_[t] = std::make_unique<Xe::XCPU::JIT::PPCTranslator>(ppu, ppeState, hirBackend_[t].get());
      hirTranslator_[t]->SetHLEFunctionTable(&ppu->xenonContext->hleFunctionTable);
    }
    hirReady_.store(true, std::memory_order_release);
    LOG_INFO(Xenon, "[JIT]: HIR pipeline initialized (x86 backend, 2 per-thread translators + backends)");
#else
    LOG_ERROR(Xenon, "[JIT]: No HIR backend available for this architecture");
#endif
  });
}

bool PPU_JIT::BuildHIRBlock(u64 blockStartAddress, u64 maxInstrs, ePPUThreadID threadId, void **outCode, u64 *outCodeSize) {
  EnsureHIRPipeline();
  u8 tid = static_cast<u8>(threadId);
  if (!hirReady_ || !hirTranslator_[tid]) return false;

  // Full pipeline: PPC -> HIR -> Backend -> code pointer.
  u64 codeSize = 0;
  void *code = hirTranslator_[tid]->TranslateBlock(blockStartAddress, ppeState, maxInstrs, &codeSize,
    nullptr, threadId);
  if (!code) return false;

  *outCode = code;
  if (outCodeSize) *outCodeSize = codeSize;
  return true;
}

void PPU_JIT::ReleaseHIRCode(void *codePtr) {
  // Both backends share the same JitRuntime, so either can release.
  if (codePtr && hirBackend_[0]) {
    hirBackend_[0]->ReleaseCode(codePtr);
  }
}

// Build a JIT block using the HIR pipeline and integrate it into the cache system.
std::shared_ptr<JITBlock> PPU_JIT::BuildJITBlockHIR(u64 blockStartAddress, u64 maxBlockSize, ePPUThreadID threadId) {
  MICROPROFILE_SCOPEI("[Xe::JIT]", "BuildJITBlockHIR", MP_AUTO);
  EnsureHIRPipeline();
  u8 tid = static_cast<u8>(threadId);
  if (!hirReady_ || !hirTranslator_[tid]) return nullptr;

  auto &cache = threadCaches[tid];
  auto &thread = ppeState->ppuThread[tid];

  // Full pipeline: PPC -> HIR -> Backend -> code pointer, with metadata.
  u64 codeSize = 0;
  HIRBlockMetadata meta;
  void **chainSlot = nullptr;
  void *code = hirTranslator_[tid]->TranslateBlock(blockStartAddress, ppeState, maxBlockSize,
                                                     &codeSize, &meta, threadId,
                                                     &chainSlot);

  if (!code) {
    // Backend should have already freed any allocated slot on failure, but
    // guard here in case a future backend forgets.
    delete chainSlot;
    return nullptr;
  }

  // Cast to JITFunc (safe — HIR backend now produces JITFunc-compatible signature).
  JITFunc funcPtr = reinterpret_cast<JITFunc>(code);

  // Create JITBlock via the HIR constructor (no JITBlockBuilder needed).
  u64 blockSizeBytes = meta.instrCount * 4;
  std::shared_ptr<JITBlock> block = std::make_shared<JITBlock>(
    &jitRuntime, &jitRuntimeMutex, blockStartAddress, blockSizeBytes, funcPtr, codeSize);

  // Set metadata from the HIR translator.
  block->hash = meta.hash;
  block->msrSF = meta.msrSF;
  block->chainSlot = chainSlot;
  block->chainTargetGuestAddr = meta.chainTargetGuestAddr;

  // Insert block into the cache under a key that encodes MSR.SF so 32-bit
  // and 64-bit specializations of the same address coexist.
  const u64 cacheKey = ComposeBlockCacheKey(blockStartAddress, meta.msrSF);
  cache.jitBlocksCache.emplace(cacheKey, block);
  cache.fastBlockCache.insert(cacheKey, block.get());

  // Register pages used by the block. Pages key by the composite key so a
  // page invalidation tears down both SF=0 and SF=1 versions independently.
  RegisterBlockPages(cacheKey, blockSizeBytes, cache);

  // Block linking.
  // 1. Register each chain edge (taken + optional fall) so we can patch the
  //    slot when the destination block is compiled (or invalidated).
  // 2. If the destination block is already in the cache, resolve the chain
  //    immediately. Self-loops are rejected to avoid unbounded recursion;
  //    the chain budget bounds longer cycles.
  // 3. Resolve any pending edges that target THIS block. Other blocks may
  //    have chains waiting for blockStartAddress.
  if (chainSlot && meta.chainTargetGuestAddr != ~0ULL) {
    const u64 targetKey = ComposeBlockCacheKey(meta.chainTargetGuestAddr, meta.msrSF);
    RegisterChainEdge(cacheKey, targetKey, chainSlot, block.get(), cache);
  }
  ResolveChainsFor(cacheKey, funcPtr, cache);

  return block;
}

// Per-function HIR path: attempt to compile an entire guest function into one
// native function. Falls back to BuildJITBlockHIR when TranslateFunction returns
// nullptr (function has indirect branches, MSR changes, rfid, or scan failure).
std::shared_ptr<JITBlock> PPU_JIT::BuildJITFunctionHIR(u64 funcStartAddress,
                                                        u64 maxBlockSize,
                                                        ePPUThreadID threadId) {
  MICROPROFILE_SCOPEI("[Xe::JIT]", "BuildJITFunctionHIR", MP_AUTO);
  EnsureHIRPipeline();
  u8 tid = static_cast<u8>(threadId);
  if (!hirReady_ || !hirTranslator_[tid]) return nullptr;

  auto &cache = threadCaches[tid];

  // Try per-function mode.
  u64 codeSize = 0;
  HIRBlockMetadata meta{};
  void *code = hirTranslator_[tid]->TranslateFunction(funcStartAddress, ppeState,
                                                       &codeSize, &meta, threadId,
                                                       /*outChainSlot=*/nullptr);

  if (!code) {
    // Function mode declined — fall back to per-block.
    return BuildJITBlockHIR(funcStartAddress, maxBlockSize, threadId);
  }

  JITFunc funcPtr = reinterpret_cast<JITFunc>(code);

  u64 blockSizeBytes = meta.instrCount * 4;
  std::shared_ptr<JITBlock> block = std::make_shared<JITBlock>(
    &jitRuntime, &jitRuntimeMutex, funcStartAddress, blockSizeBytes, funcPtr, codeSize);

  block->hash = meta.hash;
  block->msrSF = meta.msrSF;
  // Per-function blocks have no external chain slot (intra-function branches are direct).
  block->chainSlot = nullptr;
  block->chainTargetGuestAddr = HIR::INVALID_CHAIN_TARGET;

  const u64 cacheKey = ComposeBlockCacheKey(funcStartAddress, meta.msrSF);
  cache.jitBlocksCache.emplace(cacheKey, block);
  cache.fastBlockCache.insert(cacheKey, block.get());

  RegisterBlockPages(cacheKey, blockSizeBytes, cache);

  // Resolve any pending chain edges that target the function entry point.
  ResolveChainsFor(cacheKey, funcPtr, cache);

  return block;
}

// Block-linking registry helpers. Per-thread cache, single-owner thread.
void PPU_JIT::RegisterChainEdge(u64 sourceKey, u64 targetKey, void **slot,
                                JITBlock * /*targetBlock*/, ThreadJITCache &cache) {
  if (!slot) return;
  *slot = nullptr;

  // Self-loops (`b .` halt patterns, or `bc` with branch=fall=self) would
  // still be bounded by the chain budget but waste a budget hop on something
  // the dispatcher would handle anyway. Skip them.
  if (sourceKey == targetKey) {
    return;
  }

  // Track the edge so future builds/invalidations of the target can patch
  // the slot, and so source teardown can remove its entry.
  cache.chainTargetSlots[targetKey].push_back(slot);
  cache.chainSourceIndex[sourceKey].push_back({ targetKey, slot });
}

void PPU_JIT::ResolveChainsFor(u64 targetKey, JITFunc targetCode, ThreadJITCache &cache) {
  auto it = cache.chainTargetSlots.find(targetKey);
  if (it == cache.chainTargetSlots.end()) return;

  void *codeAsVoid = reinterpret_cast<void *>(targetCode);
  for (void **slot : it->second) {
    if (!slot) continue;
    // Skip self-loops: find the source key for this slot via reverse index.
    // The slot itself doesn't tell us the source, but every slot in this
    // vector targets `targetKey`. If a slot's source key equals targetKey
    // we'd be installing a self-chain — leave it null.
    *slot = codeAsVoid;
  }
}

void PPU_JIT::ClearChainsFor(u64 targetKey, ThreadJITCache &cache) {
  auto it = cache.chainTargetSlots.find(targetKey);
  if (it == cache.chainTargetSlots.end()) return;
  for (void **slot : it->second) {
    if (slot) *slot = nullptr;
  }
}

void PPU_JIT::UnregisterChainSource(u64 sourceKey, ThreadJITCache &cache) {
  auto it = cache.chainSourceIndex.find(sourceKey);
  if (it == cache.chainSourceIndex.end()) return;

  for (const auto &edge : it->second) {
    const u64 targetKey = edge.first;
    void **slot = edge.second;
    auto vit = cache.chainTargetSlots.find(targetKey);
    if (vit != cache.chainTargetSlots.end()) {
      auto &vec = vit->second;
      vec.erase(std::remove(vec.begin(), vec.end(), slot), vec.end());
      if (vec.empty()) cache.chainTargetSlots.erase(vit);
    }
  }
  cache.chainSourceIndex.erase(it);
}

// Register the pages a block covers.
void PPU_JIT::RegisterBlockPages(u64 blockStart, u64 blockSize, ThreadJITCache &cache) {
  constexpr u64 pageSize = 4096ULL;
  if (blockSize == 0) return;

  u64 pageCount = (blockSize + pageSize - 1) / pageSize;
  u64 firstPage = blockStart & ~(pageSize - 1ULL);

  std::vector<u64> pages;
  pages.reserve(static_cast<size_t>(pageCount));
  for (u64 i = 0; i < pageCount; ++i) {
    u64 pageBase = firstPage + i * pageSize;
    cache.pageBlockIndex[pageBase].insert(blockStart);
    pages.push_back(pageBase);
  }
  cache.blockPageList[blockStart] = std::move(pages);

#ifdef JIT_DEBUG
  LOG_DEBUG(Xenon, "[JIT]: Registered block {:#x} size {:#x} -> pages: {:#x}..{:#x}", blockStart, blockSize,
    firstPage, firstPage + pageCount * pageSize - 1);
#endif
}

// Unregister a block from the page index (caller must ensure safe access).
void PPU_JIT::UnregisterBlockLocked(u64 blockStart, ThreadJITCache &cache) {
  auto it = cache.blockPageList.find(blockStart);
  if (it == cache.blockPageList.end()) { return; }
  for (u64 pageBase : it->second) {
    auto pit = cache.pageBlockIndex.find(pageBase);
    if (pit != cache.pageBlockIndex.end()) {
      pit->second.erase(blockStart);
      if (pit->second.empty()) {
        cache.pageBlockIndex.erase(pit);
      }
    }
  }
  cache.blockPageList.erase(it);

#ifdef JIT_DEBUG
  LOG_DEBUG(Xenon, "[JIT]: Unregistered block {:#x} from page index", blockStart);
#endif
}

// Invalidation.
void PPU_JIT::InvalidateBlocksForRange(u64 startAddr, u64 endAddr, ePPUThreadID threadId) {
  MICROPROFILE_SCOPEI("[Xe::JIT]", "InvalidateBlocksForRange", MP_AUTO);
  constexpr u64 pageSize = 4096ULL;
  if (startAddr >= endAddr) return;

  u64 startPage = startAddr & ~(pageSize - 1ULL);
  u64 endPage = (endAddr + pageSize - 1) & ~(pageSize - 1ULL);

  auto &cache = threadCaches[threadId];

  // Lock for the entire invalidation sequence.
  std::lock_guard<std::mutex> lock(cache.invalidationMutex);

  std::vector<u64> blocksToInvalidate;
  for (u64 page = startPage; page < endPage; page += pageSize) {
    auto pit = cache.pageBlockIndex.find(page);
    if (pit == cache.pageBlockIndex.end()) continue;
    for (u64 blk : pit->second) blocksToInvalidate.push_back(blk);
  }

  std::sort(blocksToInvalidate.begin(), blocksToInvalidate.end());
  blocksToInvalidate.erase(std::unique(blocksToInvalidate.begin(), blocksToInvalidate.end()), blocksToInvalidate.end());

  if (blocksToInvalidate.empty()) {
#ifdef JIT_DEBUG
    LOG_DEBUG(Xenon, "[JIT]: No JIT blocks to invalidate for range {:#x}-{:#x} (thread {})", startAddr, endAddr, t);
#endif
    return;
  }

  for (u64 blkAddr : blocksToInvalidate) {
    auto it = cache.jitBlocksCache.find(blkAddr);
    if (it != cache.jitBlocksCache.end()) {
#ifdef JIT_DEBUG
      LOG_DEBUG(Xenon, "[JIT]: Invalidating block at {:#x} due to page invalidation range {:#x}-{:#x} (thread {})", blkAddr, startAddr, endAddr, t);
#endif
      cache.fastBlockCache.invalidate(blkAddr);
      // Block linking: any slot pointing at this block's host code is now
      // a dangling pointer. Reset all such slots to null (forces a dispatch
      // fallback). Then remove this block's own slot from the registry so
      // its destructor can free it without leaving a dangling registry entry.
      ClearChainsFor(blkAddr, cache);
      cache.chainTargetSlots.erase(blkAddr);
      UnregisterChainSource(blkAddr, cache);
      it->second.reset();
      cache.jitBlocksCache.erase(it);
    }
    UnregisterBlockLocked(blkAddr, cache);
  }
}

void PPU_JIT::InvalidateAllBlocks(ePPUThreadID threadId) {

  auto &cache = threadCaches[threadId];
  std::lock_guard<std::mutex> lock(cache.invalidationMutex);
#ifdef JIT_DEBUG
  LOG_DEBUG(Xenon, "[JIT]: Invalidating ALL JIT blocks (thread {})", t);
#endif
  // Block linking: clear the registry first. The slot cells themselves are
  // owned by individual JITBlocks and will be freed when those blocks are
  // reset below. Clearing the registry now ensures no stale entries reference
  // soon-to-be-freed slot pointers.
  cache.chainTargetSlots.clear();
  cache.chainSourceIndex.clear();
  for (auto &p : cache.jitBlocksCache) {
    p.second.reset();
  }
  cache.jitBlocksCache.clear();
  cache.fastBlockCache.invalidateAll();
  cache.pageBlockIndex.clear();
  cache.blockPageList.clear();
}

// Gets current sPPUThread using the compile-time known thread ID.
// Instead of reading ppeState->currentThread at runtime (which races between
// host threads), we bake the thread offset directly into the JIT code
void PPU_JIT::SetupContext(JITBlockBuilder *b, ePPUThreadID threadId) {
#if defined(ARCH_X86) || defined(ARCH_X86_64)
  // Compute the fixed offset for this thread's sPPUThread within sPPEState
  u64 threadOffset = static_cast<u8>(threadId) * sizeof(sPPUThread);
  COMP->lea(b->threadCtx->Base(), asmjit::x86::ptr(b->ppeState->Base(), static_cast<s32>(threadOffset)));
#endif
}

// JIT Instruction Prologue (Constant Address)
// * Uses compile-time known CIA value instead of reading NIA from memory
// * Used for instructions that may cause sync exceptions or are branches
void PPU_JIT::InstrPrologueConst(JITBlockBuilder *b, u64 cia, u32 instrData) {
#if defined(ARCH_X86) || defined(ARCH_X86_64)
  x86::Gp temp = newGP64();
  // CIA
  COMP->mov(temp, cia);
  COMP->mov(b->threadCtx->scalar(&sPPUThread::CIA), temp);
  // NIA = CIA + 4
  COMP->mov(temp, cia + 4);
  COMP->mov(b->threadCtx->scalar(&sPPUThread::NIA), temp);
  // CI data
  COMP->mov(temp, instrData);
  COMP->mov(b->threadCtx->scalar(&sPPUThread::CI).Ptr<u32>(), temp);
#endif
}

// JIT Instruction Prologue (Minimal)
// * Only writes the CI data. CIA and NIA are not updated. Saves 4 x86 instructions
// * Used for instructions that cannot cause sync exceptions and are not branches
void PPU_JIT::InstrPrologueMinimal(JITBlockBuilder *b, u32 instrData) {
#if defined(ARCH_X86) || defined(ARCH_X86_64)
  x86::Gp temp = newGP64();
  // CI data only
  COMP->mov(temp, instrData);
  COMP->mov(b->threadCtx->scalar(&sPPUThread::CI).Ptr<u32>(), temp);
#endif
}

#undef GPR
using namespace asmjit;
// Builds a JIT block starting at the given address.
std::shared_ptr<JITBlock> PPU_JIT::BuildJITBlock(u64 blockStartAddress, u64 maxBlockSize, ePPUThreadID threadId) {
  MICROPROFILE_SCOPEI("[Xe::JIT]", "BuildJITBlockLegacy", MP_AUTO);
  auto &cache = threadCaches[static_cast<u8>(threadId)];

  std::unique_ptr<JITBlockBuilder> jitBuilder = std::make_unique<STRIP_UNIQUE(jitBuilder)>(blockStartAddress, &jitRuntime);

#if defined(ARCH_X86) || defined(ARCH_X86_64)
  asmjit::x86::Compiler compiler(jitBuilder->Code());
  jitBuilder->compiler = &compiler;

  // Setup function and, state / thread context
  jitBuilder->ppu = new ASMJitPtr<PPU>(compiler.newGpz("ppu"));
  jitBuilder->ppeState = new ASMJitPtr<sPPEState>(compiler.newGpz("ppeState"));
  jitBuilder->threadCtx = new ASMJitPtr<sPPUThread>(compiler.newGpz("thread"));
  jitBuilder->haltBool = compiler.newGpb("enableHalt"); // bool

  FuncNode *signature = nullptr;
  compiler.addFuncNode(&signature, FuncSignature::build<void, PPU *, sPPEState *, bool>());
  signature->setArg(0, jitBuilder->ppu->Base());
  signature->setArg(1, jitBuilder->ppeState->Base());
  signature->setArg(2, jitBuilder->haltBool);

  // Enable AVX support
  signature->frame().setAvxEnabled();

#endif

  // Temporary container holding all instructions data in the block.
  // Pre-allocate to reduce reallocations during block building
  std::vector<u32> instrsTemp{};
  instrsTemp.reserve(maxBlockSize > 64 ? 64 : static_cast<size_t>(maxBlockSize));

  // Setup our block context.
  SetupContext(jitBuilder.get(), threadId);

  //
  // Instruction emitter
  //

  u64 instrCount = 0;

  while (XeRunning && !XePaused) {
    auto &thread = curThread;

    // Update previous instruction address
    thread.PIA = thread.CIA;
    // Update current instruction address
    thread.CIA = thread.NIA;
    // Increase next instruction address
    thread.NIA += 4;

    // Is the instruction data valid?
    bool instrDataValid = true;
    // Fetch Instruction data.
    thread.instrFetch = true;
    uPPCInstr op{ PPCInterpreter::MMURead32(ppeState, thread.CIA) };
    thread.instrFetch = false;

    // Check for Instruction storage/segment exceptions. If found we must end the block.
    if (curThread.exceptReg & ppuInstrStorageEx || curThread.exceptReg & ppuInstrSegmentEx) {
#ifdef JIT_DEBUG
      LOG_DEBUG(Xenon, "[JIT]: Instruction exception when creating block at CIA {:#x}, block start address {:#x}, instruction count {:#x}",
        thread.CIA, blockStartAddress, instrCount);
#endif
      if (instrCount != 0) {
        // We're a few instructions into the block, just end the block on the last instruction and start a new block on
        // the faulting instruction. It will process the exception accordingly.
        // We clear the exception condition or else the exception handler will run on the first instruction of last the 
        // compiled block.
        thread.exceptReg &= ~(ppuInstrStorageEx | ppuInstrSegmentEx);
        break;
      } else {
        // Manually process the pending exceptions.
        ppu->PPUProcessSyncExceptions(ppeState);
        // Return from block creation. Next block will be one the handlers for instruction exceptions.
        return nullptr;
      }
    }

    u32 opcode = op.opcode;
    instrsTemp.push_back(opcode);

    // Decode and emit

    // Saves a few cycles to cache the value here
    u32 decodedInstr = PPCDecode(opcode);
    auto emitter = PPCInterpreter::ppcDecoder.decodeJIT(opcode);

    // Compute instruction name hash - use direct computation instead of thread_local map
    // The hash is only needed for block termination check, so compute it efficiently
    u32 opName = Base::JoaatStringHash(PPCInterpreter::ppcDecoder.getNameTable()[decodedInstr]);

    // Check if this instruction is a block-ending branch
    bool isBranchInstr = (opName == JITOpcodeHashes::B || opName == JITOpcodeHashes::BC ||
      opName == JITOpcodeHashes::BCLR || opName == JITOpcodeHashes::BCCTR ||
      opName == JITOpcodeHashes::RFID);

    // Check if this instruction can cause sync exceptions
    bool canCauseException = InstrCanCauseSyncException(opName);

    // Branch instructions and sync exception-capable instructions need CIA/NIA updated
    // Safe instructions only need CI data written
    if (isBranchInstr || canCauseException) {
      InstrPrologueConst(jitBuilder.get(), thread.CIA, opcode);
    } else {
      InstrPrologueMinimal(jitBuilder.get(), opcode);
    }

    // Check for ocurred Instruction access exceptions.
    if (opcode == 0xFFFFFFFF || opcode == 0xCDCDCDCD || opcode == 0x00000000) {
      instrDataValid = false;
    }

    // Call JIT Emitter on fetched instruction id instruction data is valid.
    if (instrDataValid) {
      // First perform any patches for registers at runtime.
      // Used for codeflow skips, and value patching.
      // TODO: Use the correct patching system as in HIR
#if defined(ARCH_X86) || defined(ARCH_X86_64)
      auto patchGPR = [&](s32 reg, u64 val) {
        x86::Gp temp = compiler.newGpq();
        compiler.mov(temp, val);
        compiler.mov(jitBuilder->threadCtx->array(&sPPUThread::GPR).Ptr(reg), temp);
        };

      // Patches are done using the 32 bit Kernel/Games address space.
      switch (static_cast<u32>(thread.CIA)) {
        // Set XAM Debug Output Level to Trace
      case 0x81743B20: patchGPR(10, 4); break;
      case 0x0200C870: patchGPR(5, 0); break;
        // CNicEmac::NicDoTimer trap, 17489
      //case 0x801086a8: patchGPR(10, 2); break;
        // RGH 2 17489 in a JRunner Corona XDKBuild
      case 0x0200C7F0: patchGPR(3, 0); break;
        // VdpWriteXDVOUllong. Set r10 to 1. Skips XDVO write loop
      case 0x800EF7C0: patchGPR(10, 1); break;
        // VdpSetDisplayTimingParameter. Set r11 to 0x10. Skips ANA Check
      case 0x800F6264: patchGPR(11, 0x15E); break;
        // Needed for FSB_FUNCTION_2
      case 0x1003598ULL: patchGPR(11, 0x0E); break;
      case 0x1003644ULL: patchGPR(11, 0x02); break;
        // Bootanim load skip
      case 0x80081EA4: patchGPR(3, 0x0); break;
        // VdRetrainEDRAM return 0
      case 0x800FC288: patchGPR(3, 0x0); break;
        // VdIsHSIOTrainingSucceeded return 1
      case 0x800F9130: patchGPR(3, 0x1); break;
        // SATA SSC Speed patch (until I can get proper code pages working in ODD)
      case 0x800C5B58: patchGPR(11, 0x3); break;
        // Pretend ARGON hardware is present, to avoid the call
      case 0x800819E0:
      case 0x80081A60: {
        x86::Gp temp = compiler.newGpq();
        compiler.mov(temp, jitBuilder->threadCtx->array(&sPPUThread::GPR).Ptr(11));
        compiler.or_(temp, 0x08);
        compiler.mov(jitBuilder->threadCtx->array(&sPPUThread::GPR).Ptr(11), temp);
      } break;
      }
#endif

      bool invalidInstr = emitter == &PPCInterpreter::PPCInterpreterJIT_invalid;

      // If the instruction is invalid and we're in hybrid mode, call the interpreter decoder and function lookup.
      if (ppu->currentExecMode == eExecutorMode::Hybrid && invalidInstr) {
        // Hybrid fallback needs CIA/NIA set for the interpreter
        // We must only set it once!
        if (!isBranchInstr && !canCauseException) {
          InstrPrologueConst(jitBuilder.get(), thread.CIA, opcode);
        }

        auto function = PPCInterpreter::ppcDecoder.decode(opcode);

#if defined(ARCH_X86) || defined(ARCH_X86_64)
        InvokeNode *out = nullptr;
        compiler.invoke(&out, imm((void *)function), FuncSignature::build<void, void *>());
        out->setArg(0, jitBuilder->ppeState->Base());
#endif
      }
      else {
        // Execute decoded instruction.
        emitter(ppeState, jitBuilder.get(), op);
      }
    }

    // Check if the executed instruction can produce Sync exceptions
    // Most instructions (System/ALU) don't, saving 3 x86 instructions per PPC instruction.
    // TODO: See if we can also include VXU/FPU arithmetic ops based on wheter MSR[SF,FP] is set at block build time.
    if (canCauseException) {
#if defined(ARCH_X86) || defined(ARCH_X86_64)
      // Test for present exceptions and return if any is found.
      Label skipRet = compiler.newLabel();
      x86::Gp exceptReg = compiler.newGpw();
      compiler.mov(exceptReg, jitBuilder->threadCtx->scalar(&sPPUThread::exceptReg));
      compiler.test(exceptReg, exceptReg);  // Check for a positive result.
      compiler.jz(skipRet);           // Skip return if no exceptions.
      compiler.ret();                 // Return if exceptions ocurred.
      compiler.bind(skipRet);         // Skip return Tag.
#endif
    }
    // Check if the last instruction was a branch or a jump (rfid). We must end the block if any is found or the block
    // is at the maximum available size.
    instrCount++;

    bool isBlockEnd = false;
    if (opName == JITOpcodeHashes::B || opName == JITOpcodeHashes::BC ||
        opName == JITOpcodeHashes::BCLR || opName == JITOpcodeHashes::BCCTR ||
        opName == JITOpcodeHashes::RFID || opName == JITOpcodeHashes::INVALID) {
      isBlockEnd = true;
    }

    if (isBlockEnd || instrCount >= maxBlockSize) {
      break;
    }
  }

  // Reset CIA and NIA.
  curThread.CIA = blockStartAddress - 4;
  curThread.NIA = blockStartAddress;

  // Set block size in bytes.
  jitBuilder->size = instrCount * 4;

#if defined(ARCH_X86) || defined(ARCH_X86_64)

  // If the block was not terminated by a branch instruction, NIA may not have been
  // materialized by the last instruction (if it used InstrPrologueMinimal). Emit a
  // final NIA write so that execution resumes at the correct address.
  if (instrCount > 0) {
    bool lastWasBranch = false;
    if (!instrsTemp.empty()) {
      u32 lastOpcode = instrsTemp.back();
      u32 lastDecoded = PPCDecode(lastOpcode);
      u32 lastOpName = Base::JoaatStringHash(PPCInterpreter::ppcDecoder.getNameTable()[lastDecoded]);
      lastWasBranch = (lastOpName == JITOpcodeHashes::B || lastOpName == JITOpcodeHashes::BC ||
        lastOpName == JITOpcodeHashes::BCLR || lastOpName == JITOpcodeHashes::BCCTR ||
        lastOpName == JITOpcodeHashes::RFID);
    }
    if (!lastWasBranch) {
      // Block ended due to maxBlockSize, set final NIA.
      u64 finalNIA = blockStartAddress + instrCount * 4;
      x86::Gp temp = compiler.newGpq();
      compiler.mov(temp, finalNIA);
      compiler.mov(jitBuilder->threadCtx->scalar(&sPPUThread::NIA), temp);
    }
  }

  // Block end.
  compiler.ret();
  compiler.endFunc();
  compiler.finalize();
#endif

  // Create the final JITBlock
  std::shared_ptr<JITBlock> block = std::make_shared<STRIP_UNIQUE(block)>(&jitRuntime, &jitRuntimeMutex, blockStartAddress, jitBuilder.get());
  if (!block->Build()) {
    block.reset();
    return nullptr; // Block build failed.
  }

  // Create block hash
  u64 hash = 0;
  for (const auto &instr : instrsTemp) { hash += instr; }
  block->hash = hash;

  // Capture the MSR.SF mode this block was built under so the cache key can
  // separate 32/64-bit specializations of the same address.
  block->msrSF = curThread.SPR.MSR.SF != 0;

  // Insert block into the block cache.
  const u64 cacheKey = ComposeBlockCacheKey(blockStartAddress, block->msrSF);
  cache.jitBlocksCache.emplace(cacheKey, block);
  cache.fastBlockCache.insert(cacheKey, block.get());

  // Register pages used by the block.
  RegisterBlockPages(cacheKey, block->size, cache);

  return block;
}
#define GPR(x) curThread.GPR[x]

// Executes a given JIT block at a designated address.
u64 PPU_JIT::ExecuteJITBlock(u64 blockStartAddress, bool enableHalt, ePPUThreadID threadId) {
  MICROPROFILE_SCOPEI("[Xe::JIT]", "ExecuteJITBlock", MP_AUTO);
  auto &cache = threadCaches[static_cast<u8>(threadId)];
  // Compose the lookup key with the current thread's MSR.SF.
  auto &thread = ppeState->ppuThread[static_cast<u8>(threadId)];
  const u64 cacheKey = ComposeBlockCacheKey(blockStartAddress, thread.SPR.MSR.SF != 0);
  // No lock — only the owning thread calls this.
  JITBlock *block = cache.fastBlockCache.lookup(cacheKey);
  if (!block) {
    auto it = cache.jitBlocksCache.find(cacheKey);
    if (it == cache.jitBlocksCache.end() || !it->second) {
      return 0;
    }
    block = it->second.get();
    cache.fastBlockCache.insert(cacheKey, block);
  }
  block->codePtr(ppu, ppeState, enableHalt);
  return block->size / 4;
}

// Execute a given number of instructions using JIT.
void PPU_JIT::ExecuteJITInstrs(u64 numInstrs, std::atomic<eThreadState> &threadState, ePPUThreadID threadId, bool enableHalt, bool singleBlock) {
  MICROPROFILE_SCOPEI("[Xe::JIT]", "ExecuteJITInstrs", MP_AUTO);
  // Get this thread cache
  auto &cache = threadCaches[static_cast<u8>(threadId)];

  // Ensure the thread-local and legacy currentThread are set for this thread
  PPCInterpreter::SetCurrentThreadId(threadId);

  // Check for Async (System Reset) exceptions, this must be done here to avoid re-running a block that would 
  // ultimately suspend the thread until a system reset is issued.
  if (curThread.exceptReg & ppuSystemResetEx) { ppu->PPUProcessAsyncExceptions(ppeState); }
  
  // Get current thread
  auto &thread = curThread;
  
  // Block-linking chain budget. The HIR chain tail decrements this before
  // each indirect call into the next block; reaching zero forces a return to
  // the dispatcher loop. Pick a depth that lets common straight-line code
  // (long sequences of unconditional `b` between basic blocks, or `bc`
  // ladders) chain freely while keeping worst-case host stack usage bounded.
  constexpr u32 kChainBudgetPerDispatch = 32;

  // Execute while running and the current thread is active
  while ((threadState.load() == eThreadState::Running) && (XeRunning && !XePaused)) {
    // Reset the chain budget for each top-level dispatch.
    thread.jitChainBudget = kChainBudgetPerDispatch;
    // When POST 0x2E HW_INIT fires, hwInitPosted is set and hwReturnAddress.
    // We make use of that to grab the LR and set it here, thus completly avoiding it.
    if (XeMain::GetCPU()->HasHWINITPosted()) {
      XeMain::GetCPU()->SetHWINITPosted(false);
      PPCInterpreter::ppuSetCR(ppeState, 0, false, false, true, false);
      thread.NIA = XeMain::GetCPU()->GetHWINITReturnAddress() & ~3ULL;
      LOG_INFO(Xenon, "[JIT] HwInit intercepted. Returning to {:#x}.", thread.NIA);
      continue;
    }

    // Quick way of skiping function calls:
    // This *must *be done here simply because of how we handle JIT.
    // We run until the start of a block, which is a branch opcode (or until it's a invalid instruction),
    // but these are branch opcodes, designed to avoid calling them.
    // So, these will break under BuildJITBlock, and to avoid the issue, it's done here
    bool skipBlock = false;
    switch (thread.NIA) {
      // XDK 17.489.0 AudioChipCorder Device Detect bypass. This is not needed for
      // older console revisions
    case 0x801AF580:
      skipBlock = true;
      break;
    default:
      break;
    }

    // Skip to next block if needed.
    if (skipBlock) { thread.NIA += 4; }

    // Get next block start address.
    u64 blockStartAddress = thread.NIA;
    // Compose the key with MSR.SF so 32-bit and 64-bit specializations of the
    // same address are distinct cache entries.
    const u64 cacheKey = ComposeBlockCacheKey(blockStartAddress, thread.SPR.MSR.SF != 0);

    // Lookup in the cache.
    JITBlock *blockPtr = nullptr;
    {
      MICROPROFILE_SCOPEI("[Xe::JIT]", "DispatchLookup", MP_AUTO);
      blockPtr = cache.fastBlockCache.lookup(cacheKey);
      if (!blockPtr) {
        auto it = cache.jitBlocksCache.find(cacheKey);
        if (it != cache.jitBlocksCache.end() && it->second) {
          blockPtr = it->second.get();
          cache.fastBlockCache.insert(cacheKey, blockPtr);
        }
      }
    }

    if (!blockPtr) {
      // Block was not found. Attempt to create a new one using the active backend.
      std::shared_ptr<JITBlock> block;
      if (activeBackend_ == eJITBackend::HIR) {
        // Try per-function mode first; BuildJITFunctionHIR falls back to
        // per-block automatically when the function is ineligible.
        block = BuildJITFunctionHIR(blockStartAddress, 0x1000, threadId);
      } else {
        block = BuildJITBlock(blockStartAddress, 0x1000, threadId);
      }

      if (!block) { continue; } // Block build attempt failed.

      // Execute our block.
      {
        MICROPROFILE_SCOPEI("[Xe::JIT]", "ExecuteCompiledBlock", MP_AUTO);
        block->codePtr(ppu, ppeState, enableHalt);
      }

      // For Testing and debugging purposes only.
      if (singleBlock)
        break;

      // If the thread was suspended due to CTRL being written, we must end execution on said thread.
      if (!ppu->IsGuestThreadEnabled(threadId)) { break; }

      // Process pending synchronous exceptions.
      if ((thread.exceptReg & SyncExceptionMask) && !thread.interruptTaken){ ppu->PPUProcessSyncExceptions(ppeState); }
    } else {
      bool realMode = false;
      realMode = !thread.SPR.MSR.DR || !thread.SPR.MSR.IR;
      if (realMode) { // When in real mode TLB is disabled. Fallback to old approach.
        u64 sum = 0;
        {
          MICROPROFILE_SCOPEI("[Xe::JIT]", "RealModeHashVerify", MP_AUTO);
          // We have a match, check for the block hash to see if it hasn't been modified.
          const u64 blockSize = blockPtr->size;
          const u64 blockAddr = blockPtr->ppuAddress;

          // Optimized hash verification - read 64-bits at a time when possible
          if (blockSize % 8 == 0) {
            const u64 count = blockSize / 8;
            for (u64 i = 0; i < count; i++) {
              thread.instrFetch = true;
              u64 val = PPCInterpreter::MMURead64(ppeState, blockAddr + i * 8);
              thread.instrFetch = false;
              sum += (val >> 32) + (val & 0xFFFFFFFF);
            }
          } else {
            const u64 count = blockSize / 4;
            for (u64 i = 0; i < count; i++) {
              thread.instrFetch = true;
              sum += PPCInterpreter::MMURead32(ppeState, blockAddr + i * 4);
              thread.instrFetch = false;
            }
          }
        }

        if (blockPtr->hash != sum) {
#ifdef JIT_DEBUG
          LOG_DEBUG(Xenon, "[JIT]: Block hash mismatch for block at address {:#x}", blockStartAddress);
#endif // JIT_DEBUG
          // Blocks do not match. Invalidate using the composite key the block
          // was registered under (the current key for this thread's MSR.SF).
          cache.fastBlockCache.invalidate(cacheKey);
          ClearChainsFor(cacheKey, cache);
          cache.chainTargetSlots.erase(cacheKey);
          UnregisterChainSource(cacheKey, cache);
          auto eraseIt = cache.jitBlocksCache.find(cacheKey);
          if (eraseIt != cache.jitBlocksCache.end()) {
            eraseIt->second.reset();
            cache.jitBlocksCache.erase(eraseIt);
          }
          UnregisterBlockLocked(cacheKey, cache);
          continue;
        }
      }

      // Run block as usual.
      JITBlock *currentBlock = blockPtr;
      {
        MICROPROFILE_SCOPEI("[Xe::JIT]", "ExecuteCompiledBlock", MP_AUTO);
        currentBlock->codePtr(ppu, ppeState, enableHalt);
      }

      // If the thread was suspended due to CTRL being written, we must end execution on said thread.
      if(!ppu->IsGuestThreadEnabled(threadId)) { break; }

      // Process pending synchronous exceptions.
      if ((thread.exceptReg & SyncExceptionMask) && !thread.interruptTaken) { ppu->PPUProcessSyncExceptions(ppeState); }
    }

    // Check for external and async exceptions.
    if (thread.SPR.MSR.EE){
      if (ppu->xenonContext->iic.hasPendingInterrupts(thread.SPR.PIR)) {
        thread.exceptReg |= ppuExternalEx;
      }
    }
    if (thread.exceptReg & AsyncExceptionMask) {
      ppu->PPUProcessAsyncExceptions(ppeState);
    }
  }
}
