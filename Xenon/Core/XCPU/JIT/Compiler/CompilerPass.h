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
#include "Core/XCPU/JIT/HIR/HIRBuilder.h"

namespace Xe {
  namespace XCPU {
    namespace Compiler {

      class Compiler;

      class CompilerPass {
      public:
        CompilerPass();
        virtual ~CompilerPass();

        virtual bool Initialize(Compiler *compiler);

        virtual bool Run(HIR::HIRBuilder *builder) = 0;

      protected:
        Arena *scratch_arena() const;

      protected:
        Compiler *compiler_;
      };

    }  // namespace compiler
  }  // namespace cpu
}  // namespace xe
