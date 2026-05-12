/***************************************************************/
/* Copyright 2026 Xenon Emulator Project. All rights reserved. */
/***************************************************************/

// Branch control instructions are now lowered entirely at HIR build time
// (see HIRBuilder::Branch / BranchConditional / BranchConditionalToCTR /
// BranchConditionalToLR). The lowering emits load_context / store_context /
// select / and / shr / sub primitives whose backend emitters live alongside
// the rest of the ALU/memory ops. There are no branch-specific opcodes for
// this file to emit, so it is intentionally empty -- kept around so the
// build system's per-file references and any in-tree includes remain valid.
