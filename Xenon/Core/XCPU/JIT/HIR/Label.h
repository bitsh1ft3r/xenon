/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#pragma once

namespace Xe {
  namespace XCPU {
    namespace HIR {

      class Block;

      class Label {
      public:
        Block *block;
        Label *next;
        Label *prev;

        uint32_t id;
        char *name;

        void *tag;
      };

    }  // namespace hir
  }  // namespace cpu
}  // namespace xe
