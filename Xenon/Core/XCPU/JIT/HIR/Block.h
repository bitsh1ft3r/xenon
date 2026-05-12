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

        // If this block ends with an unconditional `b` whose target is a
        // translate-time-constant guest address, the backend installs a chain
        // slot so the emitted tail can dispatch directly into the destination
        // block's host code (skipping the dispatcher hashmap lookup). Set by
        // HIRBuilder::Branch when the target is proved constant.
        // Holds the post-MSR-truncation guest address, NOT the cache key.
        uint64_t chainTargetGuestAddr;

        void AssertNoCycles();
      };

    }  // namespace HIR
  }  // namespace XCPU
}  // namespace Xe