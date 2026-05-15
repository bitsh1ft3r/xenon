/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "Core/XCPU/JIT/Compiler/Passes/ContextPromotionPass.h"
#include "Core/XCPU/JIT/Compiler/Compiler.h"
#include "Core/XCPU/PPU/PowerPC.h"

namespace Xe {
  namespace XCPU {
    namespace Compiler {
      namespace Passes {

        // TODO(benvanik): remove when enums redefined.
        using namespace HIR;

        using HIR::HIRBlock;
        using HIR::HIRBuilder;
        using HIR::Instr;
        using HIR::Value;

        ContextPromotionPass::ContextPromotionPass() : CompilerPass() {}

        ContextPromotionPass::~ContextPromotionPass() {}

        bool ContextPromotionPass::Initialize(Compiler *compiler) {
          if (!CompilerPass::Initialize(compiler)) {
            return false;
          }

          // This is a terrible implementation.
          context_values_.resize(sizeof(sPPUThread));
          context_validity_.resize(static_cast<uint32_t>(sizeof(sPPUThread)));

          return true;
        }

        bool ContextPromotionPass::Run(HIRBuilder *builder) {
          // Like mem2reg, but because context memory is unaliasable it's easier to
          // check and convert LoadContext/StoreContext into value operations.
          // Example of load->value promotion:
          //   v0 = load_context +100
          //   store_context +200, v0
          //   v1 = load_context +100  <-- replace with v1 = v0
          //   store_context +200, v1
          //
          // It'd be possible in this stage to also remove redundant context stores:
          // Example of dead store elimination:
          //   store_context +100, v0  <-- removed due to following store
          //   store_context +100, v1
          // This is more generally done by DSE, however if it could be done here
          // instead as it may be faster (at least on the block-level).

          // Promote loads to values.
          // Process each block independently, for now.
          auto block = builder->getBlockHead();
          while (block) {
            PromoteBlock(block);
            block = block->next;
          }

          // Remove all dead stores.
          // This will break debugging as we can't recover this information when
          // trying to extract stack traces/register values, so we don't do that.
          if (true) { //(!cvars::debug && !cvars::store_all_context_values) {
            block = builder->getBlockHead();
            while (block) {
              RemoveDeadStoresBlock(block);
              block = block->next;
            }
          }

          return true;
        }

        void ContextPromotionPass::PromoteBlock(HIRBlock *block) {
          auto &validity = context_validity_;
          validity.reset();

          Instr *i = block->instr_head;
          while (i) {
            auto next = i->next;
            if (i->opcode->flags & OPCODE_FLAG_VOLATILE) {
              // Volatile instruction - requires all context values be flushed.
              validity.reset();
            }
            else if (i->opcode == &OPCODE_LOAD_CONTEXT_info) {
              size_t offset = i->src1.offset;
              if (validity.test(static_cast<uint32_t>(offset))) {
                // Legit previous value, reuse.
                Value *previous_value = context_values_[offset];
                i->opcode = &HIR::OPCODE_ASSIGN_info;
                i->set_src1(previous_value);
              }
              else {
                // Store the loaded value into the table.
                context_values_[offset] = i->dest;
                validity.set(static_cast<uint32_t>(offset));
              }
            }
            else if (i->opcode == &OPCODE_STORE_CONTEXT_info) {
              size_t offset = i->src1.offset;
              Value *value = i->src2.value;
              // Store value into the table for later.
              context_values_[offset] = value;
              validity.set(static_cast<uint32_t>(offset));
            }
            i = next;
          }
        }

        void ContextPromotionPass::RemoveDeadStoresBlock(HIRBlock *block) {
          auto &validity = context_validity_;
          validity.reset();

          // Walk backwards and mark offsets that are written to.
          // If the offset was written to earlier, ignore the store.
          Instr *i = block->instr_tail;
          while (i) {
            Instr *prev = i->prev;
            if (i->opcode->flags & (OPCODE_FLAG_VOLATILE | OPCODE_FLAG_BRANCH)) {
              // Volatile instruction - requires all context values be flushed.
              validity.reset();
            }
            else if (i->opcode == &OPCODE_STORE_CONTEXT_info) {
              size_t offset = i->src1.offset;
              if (!validity.test(static_cast<uint32_t>(offset))) {
                // Offset not yet written, mark and continue.
                validity.set(static_cast<uint32_t>(offset));
              }
              else {
                // Already written to. Remove this store.
                i->Remove();
              }
            }
            i = prev;
          }
        }

      }  // namespace passes
    }  // namespace compiler
  }  // namespace cpu
}  // namespace xe
