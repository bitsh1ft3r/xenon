/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#pragma once

#include "Core/XCPU/JIT/HIR/Arena.h"

namespace llvm {
  class BitVector;
}  // namespace llvm

namespace Xe {
  namespace XCPU {
    namespace HIR {
      class HIRBuilder;
      class Instr;
      class Label;

      // Sentinel meaning "no constant-target chain available for this block".
      // Used as the default for HIRBlock::chainTargetGuestAddr.
      static constexpr uint64_t INVALID_CHAIN_TARGET = ~0ULL;

      class HIRBlock {
      public:
        Arena *arena;

        llvm::BitVector *incoming_values;

        Label *label_head;
        Label *label_tail;

        Instr *instr_head;
        Instr *instr_tail;

        uint16_t ordinal;

        // If this block ends with a branch to a translate-time-constant guest
        // address, the backend installs a chain slot so the emitted tail can
        // dispatch directly into the destination block's host code (skipping
        // the dispatcher hashmap lookup).
        // chainTargetGuestAddr:     taken-path target (set by Branch / BranchConditional)
        // chainTargetGuestAddrFall: fall-through target for `bc` (set by BranchConditional)
        // Both hold the post-MSR-truncation guest address, NOT the cache key.
        // INVALID_CHAIN_TARGET means "not chainable on this path".
        uint64_t chainTargetGuestAddr;
        uint64_t chainTargetGuestAddrFall;

        void AssertNoCycles();
      };

    }  // namespace HIR
  }  // namespace XCPU
}  // namespace Xe