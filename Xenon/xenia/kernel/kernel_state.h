// Minimal xe::kernel::KernelState stub for xenia GPU integration into Xenon
#pragma once

#include <cstdint>
#include <memory>

#include "xenia/memory.h"

// A lot of files reference kernel state, must clean this.

namespace xe {
namespace kernel {

class KernelState {
 public:
  KernelState() : memory_(nullptr) {}
  explicit KernelState(Memory* memory) : memory_(memory) {}

  Memory* memory() const { return memory_; }

 private:
  Memory* memory_;
};

}  // namespace kernel
}  // namespace xe
