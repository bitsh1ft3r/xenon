/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "Core/XCPU/JIT/Compiler/Compiler.h"
#include "Core/XCPU/JIT/Compiler/Passes/ConditionalGroupSubpass.h"

namespace Xe {
  namespace XCPU {
    namespace Compiler {
      namespace Passes {

        ConditionalGroupSubpass::ConditionalGroupSubpass() : CompilerPass() {}

        ConditionalGroupSubpass::~ConditionalGroupSubpass() = default;

      }  // namespace passes
    }  // namespace compiler
  }  // namespace cpu
}  // namespace xe
