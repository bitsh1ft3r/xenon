// Minimal xe::cpu::Processor stub for xenia GPU integration into Xenon
#pragma once

#include <cstdint>
#include "xenia/memory.h"

// Forward declaration — avoid pulling Xenon headers into xenia stubs.
class PCIBridge;

namespace xe {
namespace cpu {

class ThreadState {};

class Processor {
 public:
  Processor() : memory_(nullptr), pci_bridge_(nullptr) {}
  explicit Processor(Memory* memory, PCIBridge* pci_bridge = nullptr)
      : memory_(memory), pci_bridge_(pci_bridge) {}

  Memory* memory() const { return memory_; }

  void SetPCIBridge(PCIBridge* pci_bridge) { pci_bridge_ = pci_bridge; }

  // GPU interrupt dispatch — in xenia this calls back into PPC guest code.
  // In Xenon we route the interrupt through the PCI bridge to the IIC,
  // just like the real Xenon CP does for PM4_INTERRUPT packets.
  void ExecuteInterrupt(uint32_t targetCPUMask);

 private:
  Memory* memory_;
  PCIBridge* pci_bridge_;
};

}  // namespace cpu
}  // namespace xe
