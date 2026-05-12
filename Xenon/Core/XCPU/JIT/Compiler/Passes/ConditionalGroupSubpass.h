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
#include "Core/XCPU/JIT/Compiler/CompilerPass.h"

namespace Xe {
  namespace XCPU {
    namespace Compiler {
      class Compiler;
      namespace Passes {

        class ConditionalGroupSubpass : public CompilerPass {
        public:
          ConditionalGroupSubpass();
          virtual ~ConditionalGroupSubpass();

          bool Run(HIR::HIRBuilder *builder) override {
            bool dummy;
            return Run(builder, dummy);
          }

          virtual bool Run(HIR::HIRBuilder *builder, bool &result) = 0;
        };

      }  // namespace passes
    }  // namespace compiler
  }  // namespace cpu
}  // namespace xe