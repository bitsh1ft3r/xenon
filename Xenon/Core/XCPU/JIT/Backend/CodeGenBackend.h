/***************************************************************/
/* Copyright 2026 Xenon Emulator Project. All rights reserved. */
/***************************************************************/

#pragma once

#include "Base/Types.h"
#include "Core/XCPU/PPU/PowerPC.h"
#include "Core/XCPU/JIT/HIR/Block.h"

namespace Xe {
  namespace XCPU {
    namespace JIT {

      // Native code generation backend
      class CodeGenBackend {
      public:
        virtual ~CodeGenBackend() = default;

        // One-time initialization (build dispatch tables, etc.).
        virtual bool Initialize() = 0;

        // Emit native code for an entire HIR block.
        // NOTE: The caller owns |outCode| and is responsible for releasing it via ReleaseCode().
        // For unconditional `b` blocks the backend heap-allocates a `void *`
        // cell and bakes its address into the emitted chain tail. The caller
        // takes ownership and must `delete` it when the block is freed.
        // outChainSlot is set to nullptr when the block is not chainable.
        virtual bool EmitBlock(HIR::HIRBlock *block, void **outCode, u64 *outCodeSize,
                               ePPUThreadID threadId = ePPUThread_Zero,
                               void ***outChainSlot = nullptr) = 0;

        // Emit native code for an entire multi-block function.
        // blockHead is the first HIRBlock in the linked list produced by HIRBuilder
        // (builder.getBlockHead()). All blocks in the chain are emitted into a single
        // CodeHolder so intra-function label jumps resolve without re-entering the dispatcher.
        // outChainSlot is always set to nullptr — chain slots are per-block only.
        virtual bool EmitFunction(HIR::HIRBlock *blockHead,
                                  void **outCode, u64 *outCodeSize,
                                  ePPUThreadID threadId = ePPUThread_Zero,
                                  void ***outChainSlot = nullptr) = 0;

        // Release a previously compiled code pointer.
        virtual void ReleaseCode(void *codePtr) = 0;

        // Reset transient state between block compilations.
        virtual void Reset() = 0;
      };

    } // namespace JIT
  } // namespace XCPU
} // namespace Xe
