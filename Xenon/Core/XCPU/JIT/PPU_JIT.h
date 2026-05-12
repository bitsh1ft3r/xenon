/***************************************************************/
/* Copyright 2025 Xenon Emulator Project. All rights reserved. */
/***************************************************************/

#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <mutex>

#include "Base/Arch.h"
#include "Base/Hash.h"

#if defined(ARCH_X86) || defined(ARCH_X86_64)
#include "asmjit/x86.h"
#else
#include "asmjit/core.h"
#endif

#include "Core/XCPU/PPU/PowerPC.h"
#include "Core/RootBus/RootBus.h"

//#define JIT_DEBUG

// JIT backend types
enum class eJITBackend : u8 {
  Legacy,  // Direct PPC -> x86 emission
  HIR,     // PPC -> HIR -> CodeGenBackend
};

// Function pointer types for compiled code.
class PPU;
using JITFunc = fptr<void(PPU*, sPPEState*, bool)>;
// HIR backend signature: (sPPUThread*, sPPEState*).
using HIRFunc = fptr<void(void*, void*)>;

#if defined(ARCH_X86) || defined(ARCH_X86_64)
template <typename T, typename fT>
class ArrayFieldProxy {
public:
  ArrayFieldProxy(const asmjit::x86::Gp &base, u64 offset = 0) :
    base(base), offset(offset)
  {}
  ArrayFieldProxy(const ArrayFieldProxy &other)
    : base(other.base), offset(other.offset)
  {}

  asmjit::x86::Mem operator[](u64 index) const {
    using eT = typename std::remove_extent<fT>::type;
    return asmjit::x86::ptr(base, offset + index * sizeof(eT));
  }

  asmjit::x86::Mem Ptr(u64 index) {
    using eT = typename std::remove_extent<fT>::type;
    return asmjit::x86::ptr(base, offset + index * sizeof(eT));
  }

  asmjit::x86::Gp Base() const {
    return base;
  }

  u64 Offset() const {
    return offset;
  }
private:
  asmjit::x86::Gp base;
  u64 offset = 0;
};

template <typename T, typename fT>
class ScalarFieldProxy {
public:
  ScalarFieldProxy(const asmjit::x86::Gp &base, u64 offset)
    : base(base), offset(offset)
  {}
  ScalarFieldProxy(const ScalarFieldProxy &other)
    : base(other.base), offset(other.offset)
  {}

  operator asmjit::x86::Mem() const {
    return asmjit::x86::ptr(base, offset);
  }

  template <typename pT = u8>
  asmjit::x86::Mem Ptr(u64 size = sizeof(pT)) const {
    return asmjit::x86::ptr(base, offset, size);
  }

  asmjit::x86::Gp Base() const {
    return base;
  }

  u64 Offset() const {
    return offset;
  }
private:
  asmjit::x86::Gp base;
  u64 offset;
};

template <typename T>
class ASMJitPtr {
public:
  ASMJitPtr(const asmjit::x86::Gp &baseReg, u64 offset = 0) :
base(baseReg), offset(offset)
  {}

  template<typename fT, typename U = T, std::enable_if_t<std::is_class_v<U>, int> = 0>
  ScalarFieldProxy<T, fT> scalar(fT U::*member) const {
    u64 off = reinterpret_cast<u64>(&(reinterpret_cast<T*>(0)->*member));
    return ScalarFieldProxy<T, fT>(base, offset + off);
  }

  template<typename fT, typename U = T, std::enable_if_t<std::is_class_v<U>, int> = 0>
  ArrayFieldProxy<T, fT> array(fT U::*member) const {
    u64 off = reinterpret_cast<u64>(&(reinterpret_cast<T*>(0)->*member));
    return ArrayFieldProxy<T, fT>(base, offset + off);
  }

  template<typename sT, typename U = T, std::enable_if_t<std::is_class_v<U>, int> = 0>
  ASMJitPtr<sT> substruct(sT U::*member) const {
    u64 off = reinterpret_cast<u64>(&(reinterpret_cast<T*>(0)->*member));
    return ASMJitPtr<sT>(base, offset + off);
  }

  template <typename pT = u8>
  asmjit::x86::Mem Ptr(u64 size = sizeof(pT)) const {
    return asmjit::x86::ptr(base, offset, size);
  }

  operator asmjit::x86::Gp() const {
return base;
  }

  u64 Offset() {
    return offset;
  }

  asmjit::x86::Gp Base() const {
    return base;
  }
private:
  asmjit::x86::Gp base;
  u64 offset = 0;
};
#endif

// Pre-computed instruction name hashes for fast comparison during block building
namespace JITOpcodeHashes {
  // Branch instructions that end blocks
  static constexpr u32 BCLR = "bclr"_j;
  static constexpr u32 BCCTR = "bcctr"_j;
  static constexpr u32 BC = "bc"_j;
  static constexpr u32 B = "b"_j;
  static constexpr u32 RFID = "rfid"_j;
  static constexpr u32 INVALID = "invalid"_j;
}

// Determine if an instruction (by name hash) can raise synchronous exceptions.
// TODO: make this using a map fo O1 lookups
inline bool InstrCanCauseSyncException(u32 opNameHash) {
  switch (opNameHash) {
    // These dont cause exceptions:
  case "mulli"_j: case "subficx"_j: case "cmpli"_j: case "cmpi"_j: case "addicx"_j:
  case "addi"_j: case "addis"_j: case "rlwimix"_j: case "rlwinmx"_j:
  case "rlwnmx"_j: case "ori"_j: case "oris"_j: case "xori"_j: case "xoris"_j: case "andix"_j:
  case "andisx"_j: case "mcrf"_j: case "crnor"_j: case "crandc"_j:
  case "isync"_j: case "crxor"_j: case "crnand"_j: case "crand"_j: case "creqv"_j: case "crorc"_j:
  case "cror"_j: case "rldiclx"_j: case "rldicrx"_j: case "rldicx"_j:
  case "rldimix"_j: case "rldclx"_j: case "rldcrx"_j: case "cmp"_j: case "subfcx"_j: case "subfcox"_j:
  case "mulhdux"_j: case "addcx"_j: case "addcox"_j: case "mulhwux"_j: case "mfocrf"_j: case "slwx"_j:
  case "cntlzwx"_j: case "sldx"_j: case "andx"_j: case "cmpl"_j: case "subfx"_j: case "subfox"_j:
  case "dcbst"_j: case "cntlzdx"_j: case "andcx"_j: case "mulhdx"_j: case "mulhwx"_j: case "mfmsr"_j:
  case "dcbf"_j: case "negx"_j: case "negox"_j: case "norx"_j: case "subfex"_j: case "addex"_j:
  case "addeox"_j: case "mtocrf"_j: case "subfze"_j: case "subfzeo"_j:
  case "addzex"_j: case "addzeox"_j: case "subfmex"_j: case "subfmeox"_j: case "mulldx"_j: case "mulldox"_j:
  case "addmex"_j: case "addmeox"_j: case "mullwx"_j: case "mullwox"_j: case "dcbtst"_j: case "addx"_j:
  case "addox"_j: case "dcbt"_j: case "eqvx"_j: case "eciwx"_j:
  case "xorx"_j: case "mfspr"_j: case "dst"_j: case "dstst"_j: case "slbmte"_j: case "orcx"_j:
  case "slbie"_j: case "ecowx"_j: case "orx"_j: case "divdux"_j: case "divduox"_j: case "divwux"_j:
  case "divwuox"_j: case "dcbi"_j: case "nandx"_j: case "slbia"_j: case "divdx"_j: case "divdox"_j: 
  case "divwx"_j: case "divwox"_j: case "srwx"_j: case "srdx"_j: case "tlbsync"_j:
  case "mfsrin"_j: case "mfsr"_j: case "sync"_j: case "srawx"_j: case "sradx"_j: case "dss"_j:
  case "srawix"_j: case "sradix"_j: case "eieio"_j: case "extshx"_j:
  case "extsbx"_j: case "extswx"_j: case "icbi"_j: 

  // VXU instructions
  // In reality they can all cause VX unavaleable exceptions, but they are really not necessary in emulaion
  // so we can safely skip em

  case "vaddubm"_j: case "vmaxub"_j: case "vrlb"_j: case "vmuloub"_j: case "vaddfp"_j: case "vmrghb"_j: 
  case "vpkuhum"_j:case "vadduhm"_j: case "vmaxuh"_j: case "vrlh"_j: case "vmulouh"_j: case "vsubfp"_j: 
  case "vmrghh"_j:  case "vpkuwum"_j: case "vadduwm"_j: case "vmaxuw"_j: case "vrlw"_j: case "vmrghw"_j: 
  case "vpkuhus"_j: case "vpkuwus"_j: case "vmaxsb"_j: case "vslb"_j: case "vmulosb"_j: case "vrefp"_j:
  case "vmrglb"_j: case "vpkshus"_j: case "vmaxsh"_j: case "vslh"_j: case "vmulosh"_j: case "vrsqrtefp"_j:
  case "vmrglh"_j: case "vpkswus"_j: case "vaddcuw"_j: case "vmaxsw"_j: case "vslw"_j: case "vexptefp"_j:
  case "vmrglw"_j: case "vpkshss"_j: case "vsl"_j: case "vlogefp"_j: case "vpkswss"_j: case "vaddubs"_j:
  case "vminub"_j: case "vsrb"_j: case "vmuleub"_j: case "vrfin"_j: case "vspltb"_j: case "vupkhsb"_j:
  case "vadduhs"_j: case "vminuh"_j: case "vsrh"_j: case "vmuleuh"_j:case "vrfiz"_j: case "vsplth"_j:
  case "vupkhsh"_j: case "vadduws"_j: case "vminuw"_j: case "vsrw"_j:case "vrfip"_j: case "vspltw"_j: 
  case "vupklsb"_j: case "vsr"_j:case "vrfim"_j: case "vupklsh"_j: case "vaddsbs"_j: case "vminsb"_j: 
  case "vsrab"_j: case "vmulesb"_j: case "vcfux"_j: case "vspltisb"_j: case "vpkpx"_j: case "vaddshs"_j:
  case "vminsh"_j: case "vsrah"_j: case "vmulesh"_j:case "vcfsx"_j: case "vspltish"_j: case "vupkhpx"_j:
  case "vaddsws"_j: case "vminsw"_j: case "vsraw"_j: case "vctuxs"_j: case "vspltisw"_j: case "vctsxs"_j:
  case "vupklpx"_j: case "vsububm"_j: case "vavgub"_j: case "vand"_j: case "vmaxfp"_j:  case "vslo"_j:
  case "vsubuhm"_j: case "vavguh"_j: case "vandc"_j: case "vminfp"_j: case "vsro"_j: case "vsubuwm"_j: 
  case "vavguw"_j: case "vor"_j: case "vxor"_j: case "vavgsb"_j: case "vnor"_j: case "vavgsh"_j: case "vsubcuw"_j:
  case "vavgsw"_j: case "vsububs"_j: case "mfvscr"_j: case "vsum4ubs"_j: case "vsubuhs"_j: case "mtvscr"_j: 
  case "vsum4shs"_j: case "vsubuws"_j: case "vsum2sws"_j:case "vsubsbs"_j: case "vsum4sbs"_j: case "vsubshs"_j:
  case "vsubsws"_j: case "vsumsws"_j: case "vcmpequb"_j: case "vcmpequh"_j: case "vcmpequwx"_j: case "vcmpeqfp"_j:
  case "vcmpgefp"_j: case "vcmpgtub"_j: case "vcmpgtuh"_j: case "vcmpgtuw"_j: case "vcmpgtfp"_j: case "vcmpgtsb"_j:
  case "vcmpgtsh"_j: case "vcmpgtsw"_j: case "vcmpbfp"_j: case "vmhaddshs"_j: case "vmhraddshs"_j: case "vmladduhm"_j: 
  case "vmsumubm"_j: case "vmsummbm"_j: case "vmsumuhm"_j: case "vmsumuhs"_j: case "vmsumshm"_j: case "vmsumshs"_j: 
  case "vsel"_j: case "vperm"_j: case "vsldoi"_j: case "vmaddfp"_j: case "vnmsubfp"_j: case "vsldoi128"_j:
  case "vperm128"_j: case "vaddfp128"_j: case "vsubfp128"_j: case "vmulfp128"_j: case "vmaddfp128"_j:
  case "vmaddcfp128"_j: case "vnmsubfp128"_j: case "vmsum3fp128"_j: case "vmsum4fp128"_j: case "vpkshss128"_j:
  case "vand128"_j:case "vpkshus128"_j: case "vandc128"_j: case "vpkswss128"_j: case "vnor128"_j: case "vpkswus128"_j:
  case "vor128"_j: case "vpkuhum128"_j: case "vxor128"_j: case "vpkuhus128"_j: case "vsel128"_j: case "vpkuwum128"_j:
  case "vslo128"_j: case "vpkuwus128"_j: case "vsro128"_j: case "vpermwi128"_j: case "vpkd3d128"_j: case "vrlimi128"_j:
  case "vcfpsxws128"_j: case "vcfpuxws128"_j: case "vcsxwfp128"_j: case "vcuxwfp128"_j: case "vrfim128"_j: 
  case "vrfin128"_j: case "vrfip128"_j: case "vrfiz128"_j: case "vrefp128"_j: case "vrsqrtefp128"_j: 
  case "vexptefp128"_j: case "vlogefp128"_j: case "vspltw128"_j: case "vspltisw128"_j: case "vupkd3d128"_j:
  case "vcmpeqfp128"_j: case "vcmpgefp128"_j: case "vcmpgtfp128"_j: case "vcmpbfp128"_j: case "vcmpequw128"_j:
  case "vrlw128"_j: case "vslw128"_j: case "vsraw128"_j: case "vsrw128"_j: case "vmaxfp128"_j: case "vminfp128"_j:
  case "vmrghw128"_j: case "vmrglw128"_j: case "vupkhsb128"_j: case "vupklsb128"_j:

    return false;

  // Everything else can potentially fault:
  default:
    return true;
  }
}

// Shared GPR patch table entry. Used by both Legacy and HIR JIT paths.
struct JITPatchEntry {
  u32 address;            // PPC address (32-bit kernel/game space)
  s32 reg;                // GPR index to patch
  u64 value;              // Value to set (0 = unused for OR-style patches)
  bool isOr;              // If true, OR the value into the register instead of replacing
  std::string patchName;  // Name or purpose of this patch
};

// Patch table shared between Legacy and HIR paths.
// Each entry patches a GPR at the given PPC address.
inline const JITPatchEntry kJITPatchTable[] = {
  // Set XAM Debug Output Level to Trace
  //{ 0x81743B20, 10, 4, false, "Set XAM Debug Output level to Trace"},
  { 0x0200C870, 5, 0, false, ""},
  // RGH 2 17489 in a JRunner Corona XDKBuild
  { 0x0200C7F0, 3, 0, false, "RGH 2 17489 - JRunner Corona XDK"},
  // VdpWriteXDVOUllong. Set r10 to 1. Skips XDVO write loop
  //{ 0x800EF7C0, 10, 1, false, "VdpWriteXDVOUllong - Skip XDVO Write loop"},
  // VdpSetDisplayTimingParameter. Set r11 to 0x10. Skips ANA Check
  //{ 0x800F6264, 11, 0x15E, false, "VdpSetDisplayTimingParameter - Skip (H)ANA check"},
  // Needed for FSB_FUNCTION_2
  { 0x01003598, 11, 0x0E, false, "1BL FSB_FUNCTION_2 Patch 1"},
  { 0x01003644, 11, 0x02, false, "1BL FSB_FUNCTION_2 Patch 2" },
  // Bootanim load skip
  //{ 0x80081EA4, 3, 0x0, false, "Skip loading bootanim.xex - Skip rendering boot animation"},
  // VdRetrainEDRAM return 0
  //{ 0x800FC288, 3, 0x0, false, "VdRetrainEDRAM - Return success"},
  // VdIsHSIOTrainingSucceeded return 1
  //{ 0x800F9130, 3, 0x1, false, "VdIsHSIOTrainingSucceeded - Return success"},
  // SATA SSC Speed patch
  //{ 0x800C5B58, 11, 0x3, false,"SATA SSC Speed patch"},
  // Pretend ARGON hardware is present, to avoid the call
  //{ 0x800819E0, 11, 0x08, true, "ARGON related call skip"},
  //{ 0x80081A60, 11, 0x08, true, "ARGON related call skip" },
};

// Metadata returned by the HIR translator for cache integration.
struct HIRBlockMetadata {
  u64 instrCount = 0;     // Number of PPC instructions translated
  u64 hash = 0;           // Sum of all raw instruction words
  bool lastWasBranch = false; // Whether the last instruction was a branch
  bool msrSF = true;      // MSR.SF mode this block was specialized for
                          // (true = 64-bit, false = 32-bit). Used by the cache key.
  // Constant chain target (post-MSR-truncation guest address). Mirrors
  // HIRBlock::chainTargetGuestAddr so PPU_JIT can wire the chain without
  // re-walking the HIR after backend emission. Only set for unconditional `b`.
  // ~0ULL means this block is not chainable.
  u64 chainTargetGuestAddr = ~0ULL;
};

using namespace asmjit;

// Forward declaration
class JITBlock;

// Composite block-cache key. PPC instructions are 4-byte aligned so bit 0 of a
// real address is always 0. We repurpose it to encode MSR.SF: bit 0 == 0 for
// 64-bit mode (SF=1, the historical default), bit 0 == 1 for 32-bit mode (SF=0).
// The two compiled versions of the same address coexist in the cache.
constexpr inline u64 ComposeBlockCacheKey(u64 addr, bool msrSF) {
  return (addr & ~1ULL) | (msrSF ? 0ULL : 1ULL);
}
constexpr inline u64 BlockCacheKeyAddress(u64 key) { return key & ~1ULL; }
constexpr inline bool BlockCacheKeyMSRSF(u64 key) { return (key & 1ULL) == 0; }

class JITBlockBuilder {
public:
  JITBlockBuilder(u64 addr, asmjit::JitRuntime *rt) :
  ppuAddr(addr), runtime(rt)
  {
    code.init(runtime->environment(), runtime->cpuFeatures());
  }
  ~JITBlockBuilder() {
#if defined(ARCH_X86) || defined(ARCH_X86_64)
    delete ppu;
 delete ppeState;
    delete threadCtx;
#endif
    ppuAddr = 0;
    size = 0;
  }
  u64 ppuAddr = 0; // Start Instruction Address
  u64 size = 0;   // PPC code size in bytes
  std::unordered_map<u64, u32> opcodesDataCache = {};

  asmjit::CodeHolder* Code() {
    return &code;
  }
#if defined(ARCH_X86) || defined(ARCH_X86_64)
  // x86_64
  // Current PPU
  ASMJitPtr<PPU> *ppu = nullptr;
  // Context pointer
  ASMJitPtr<sPPEState> *ppeState = nullptr;
  // Current thread context
  ASMJitPtr<sPPUThread> *threadCtx = nullptr;
  // EnableHalt flag
  x86::Gp haltBool{};
  // asmjit Compiler
  x86::Compiler *compiler = nullptr;
#endif
private:
  asmjit::CodeHolder code{};
  asmjit::JitRuntime *runtime = nullptr;
};

class JITBlock {
public:
  JITBlock(asmjit::JitRuntime *rt, std::mutex *rtMutex, u64 ppuAddr, JITBlockBuilder *builder) :
    runtime(rt), runtimeMutex(rtMutex), ppuAddress(ppuAddr), builder(builder), size(builder->size)
  {}
  // HIR path constructor: code is already compiled, no builder needed.
  JITBlock(asmjit::JitRuntime *rt, std::mutex *rtMutex, u64 ppuAddr, u64 blockSizeBytes, JITFunc code, u64 nativeCodeSize) :
    runtime(rt), runtimeMutex(rtMutex), ppuAddress(ppuAddr), builder(nullptr), size(blockSizeBytes),
    codePtr(code), codeSize(nativeCodeSize)
  {}

  // MSR.SF mode this block was specialized for. Used when invalidating to
  // recompose the cache key. true = 64-bit mode, false = 32-bit mode.
  bool msrSF = true;

  // Block linking. When the HIR translator proves this block ends with an
  // unconditional `b` to a translate-time-constant guest address, the
  // backend heap-allocates a `void *` cell and bakes its address into the
  // emitted tail. The dispatcher patches *chainSlot to the destination
  // block's host code pointer once that block is compiled, so subsequent
  // executions skip the dispatcher hashmap lookup.
  // chainTargetGuestAddr holds the post-MSR-truncation guest address (NOT
  // the composite cache key); compose it with msrSF at lookup time.
  void **chainSlot = nullptr;
  u64 chainTargetGuestAddr = ~0ULL;

  ~JITBlock() {
    // Release (delete) the code pointer allocated by asmjit
    if (codePtr) {
      std::unique_lock<std::mutex> lock;
      if (runtimeMutex) lock = std::unique_lock<std::mutex>(*runtimeMutex);
      runtime->release(codePtr);
    }
    // Free the chain slot cell. The dispatcher must have already removed any
    // registry entries that reference it.
    delete chainSlot;
    chainSlot = nullptr;
  }

  bool Build() {
    void *fnPtr = nullptr;
    asmjit::CodeHolder *code = builder->Code();
    {
      std::unique_lock<std::mutex> lock;
      if (runtimeMutex) lock = std::unique_lock<std::mutex>(*runtimeMutex);
      runtime->add(&fnPtr, code);
    }
    codePtr = reinterpret_cast<decltype(codePtr)>(fnPtr);
    codeSize = code->codeSize();
    return true;
  }

  // JIT block builder
  JITBlockBuilder *builder;
  // Pointer to compiled assembly code
  JITFunc codePtr = nullptr;
  // Size of the compiled code
  u64 codeSize = 0;
  // Address of the PPC block
  u64 ppuAddress = 0;
  // PPC code size in bytes
  u64 size = 0;
  // Reference to JIT runtime
  asmjit::JitRuntime *runtime = nullptr;
  std::mutex *runtimeMutex = nullptr;
  // Hash of all opcodes
  u64 hash = 0;
};

// Fast PPC block start -> JIT block lookup cache.
// This is a direct-mapped per-thread cache used on the dispatch hot path to
// avoid unordered_map lookups for frequently executed blocks.
class FastBlockCache {
public:
  static constexpr size_t NUM_ENTRIES = 8192;
  static constexpr size_t INDEX_MASK = NUM_ENTRIES - 1;
  static constexpr u64 INVALID_TAG = ~0ULL;

  FastBlockCache() { invalidateAll(); }

  JITBlock *lookup(u64 ppcAddress) const {
    const size_t idx = computeIndex(ppcAddress);
    const Entry &entry = entries[idx];
    if (entry.tag == ppcAddress) [[likely]] {
      return entry.block;
    }
    return nullptr;
  }

  void insert(u64 ppcAddress, JITBlock *block) {
    const size_t idx = computeIndex(ppcAddress);
    entries[idx].tag = ppcAddress;
    entries[idx].block = block;
  }

  void invalidate(u64 ppcAddress) {
    const size_t idx = computeIndex(ppcAddress);
    Entry &entry = entries[idx];
    if (entry.tag == ppcAddress) {
      entry.tag = INVALID_TAG;
      entry.block = nullptr;
    }
  }

  void invalidateAll() {
    for (size_t i = 0; i < NUM_ENTRIES; ++i) {
      entries[i].tag = INVALID_TAG;
      entries[i].block = nullptr;
    }
  }

private:
  struct Entry {
    u64 tag;
    JITBlock *block;
  };

  static constexpr size_t computeIndex(u64 ppcAddress) {
    const u64 shifted = ppcAddress >> 2;
    return static_cast<size_t>((shifted ^ (shifted >> 13)) & INDEX_MASK);
  }

  Entry entries[NUM_ENTRIES];
};

// Forward declarations for HIR backend.
namespace Xe::XCPU::JIT { class CodeGenBackend; class PPCTranslator; }

class PPU_JIT {
public:
  PPU_JIT(PPU *ppu);
  ~PPU_JIT();

  // --- Legacy (direct PPC -> x86) path ---
  void ExecuteJITInstrs(u64 numInstrs, std::atomic<eThreadState> &thrState, ePPUThreadID threadId, bool enableHalt = true, bool singleBlock = false);
  u64 ExecuteJITBlock(u64 blockStartAddress, bool enableHalt, ePPUThreadID threadId); // returns step count
  std::shared_ptr<JITBlock> BuildJITBlock(u64 blockStartAddress, u64 maxBlockSize, ePPUThreadID threadId);
  void SetupContext(JITBlockBuilder *b, ePPUThreadID threadId);
  void InstrPrologueConst(JITBlockBuilder *b, u64 cia, u32 instrData);
  void InstrPrologueMinimal(JITBlockBuilder *b, u32 instrData);

  // HIR backend path 
  std::shared_ptr<JITBlock> BuildJITBlockHIR(u64 blockStartAddress, u64 maxBlockSize, ePPUThreadID threadId);

  //HIR standalone path (testing only)
  bool BuildHIRBlock(u64 blockStartAddress, u64 maxInstrs, ePPUThreadID threadId,
                     void **outCode, u64 *outCodeSize);
  void ReleaseHIRCode(void *codePtr);

  // Backend selection
  eJITBackend activeBackend_ = eJITBackend::Legacy;
  void SetBackend(eJITBackend b) { activeBackend_ = b; }

  // Access the shared JitRuntime (used by CodeGenBackend).
  asmjit::JitRuntime *getJitRuntime() { return &jitRuntime; }

  // Page based indexing and invalidation methods.
  void InvalidateBlocksForRange(u64 startAddr, u64 endAddr, ePPUThreadID threadId);
  void InvalidateAllBlocks(ePPUThreadID threadId);

private:
  PPU *ppu = nullptr; // "Linked" PPU
  sPPEState *ppeState = nullptr; // For easier thread access
  asmjit::JitRuntime jitRuntime;
  std::mutex jitRuntimeMutex; // Protects jitRuntime add/release across threads

  // HIR pipeline (lazily initialized on first use)
  std::unique_ptr<Xe::XCPU::JIT::CodeGenBackend> hirBackend_[2]; // Per-thread backends
  std::unique_ptr<Xe::XCPU::JIT::PPCTranslator> hirTranslator_[2]; // Per-thread translators
  std::once_flag hirInitFlag_;
  std::atomic<bool> hirReady_{ false };
  void EnsureHIRPipeline();

  // Per-thread block caches (indexed by ePPUThreadID: 0 or 1).
  struct ThreadJITCache {
    // Block Cache, contains all created and valid JIT'ed blocks.
    std::unordered_map<u64, std::shared_ptr<JITBlock>> jitBlocksCache = {};
    // Hot-path lookup cache for frequently executed block entry points.
    FastBlockCache fastBlockCache{};
    // Page base -> set of block start addresses that cover that page.
    std::unordered_map<u64, std::unordered_set<u64>> pageBlockIndex = {};
    // Block start -> container of page bases it was registered under.
    std::unordered_map<u64, std::vector<u64>> blockPageList = {};
    // Block linking registry. Maps target cache key -> set of source chain
    // slots awaiting (or currently pointing at) that target. When the target
    // is compiled, every slot in the vector is patched to the target's
    // host code pointer. When the target is invalidated, every slot is
    // reset to nullptr (so the chain tail falls back to the dispatcher).
    // The slot cells themselves are owned by the source JITBlock; this
    // structure stores raw `void**` pointers and must be kept in sync with
    // block lifetimes (entries are removed when the source block is freed).
    std::unordered_map<u64, std::vector<void **>> chainTargetSlots = {};
    // Reverse map: source cache key -> list of (target cache key, slot)
    // edges. Each `b` source has one edge. Used on source invalidation to
    // find the entries in chainTargetSlots and remove them without scanning
    // the whole map.
    std::unordered_map<u64, std::vector<std::pair<u64, void **>>> chainSourceIndex = {};
    // Mutex  used for cross-thread invalidation.
    std::mutex invalidationMutex;
  };

  ThreadJITCache threadCaches[2];

  // Internal helpers for page based indexing (called without lock from owning thread).
  void RegisterBlockPages(u64 blockStart, u64 blockSize, ThreadJITCache &cache);
  void UnregisterBlockLocked(u64 blockStart, ThreadJITCache &cache);

  // Block-linking helpers (called from owning thread without lock; invalidation
  // callers hold cache.invalidationMutex).
  void RegisterChainEdge(u64 sourceKey, u64 targetKey, void **slot,
                         JITBlock *targetBlock, ThreadJITCache &cache);
  void ResolveChainsFor(u64 targetKey, JITFunc targetCode, ThreadJITCache &cache);
  void ClearChainsFor(u64 targetKey, ThreadJITCache &cache);
  void UnregisterChainSource(u64 sourceKey, ThreadJITCache &cache);
};
