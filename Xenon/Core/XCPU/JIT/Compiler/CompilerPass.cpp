/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "Core/XCPU/JIT//Compiler/Compiler.h"
#include "Core/XCPU/JIT//Compiler/CompilerPass.h"
namespace Xe {
  namespace XCPU {
    namespace Compiler {

      CompilerPass::CompilerPass() : compiler_(nullptr) {}

      CompilerPass::~CompilerPass() = default;

      bool CompilerPass::Initialize(Compiler *compiler) {
        compiler_ = compiler;
        return true;
      }

      Arena *CompilerPass::scratch_arena() const {
        return compiler_->scratch_arena();
      }

    }  // namespace compiler
  }  // namespace cpu
}  // namespace xe
