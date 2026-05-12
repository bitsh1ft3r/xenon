/***************************************************************/
/* Copyright 2025 Xenon Emulator Project. All rights reserved. */
/***************************************************************/

#ifdef XENIA_D3D12_GPU

#include "xenia/cpu/processor.h"

#include "Core/PCI/Bridge/PCIBridge.h"
#include "Base/Logging/Log.h"

// PRIO_XPS = 0x54, matching the Xenon CP's PM4_INTERRUPT routing.
#ifndef PRIO_XPS
#define PRIO_XPS 0x54
#endif

namespace xe {
namespace cpu {

void Processor::ExecuteInterrupt(uint32_t targetCPUMask) {
  if (!pci_bridge_) {
    return;
  }

  // Route as PRIO_XPS through the PCI bridge
  pci_bridge_->RouteInterrupt(PRIO_XPS, targetCPUMask);
}

}  // namespace cpu
}  // namespace xe

#endif  // XENIA_D3D12_GPU
