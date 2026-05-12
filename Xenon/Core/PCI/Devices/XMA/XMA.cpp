/***************************************************************/
/* Copyright 2025 Xenon Emulator Project. All rights reserved. */
/***************************************************************/

#include "XMA.h"
#include "Base/Logging/Log.h"
#include "Base/Thread.h"
#include "Base/Global.h"

// Uncomment for XMA debug logging
//#define XMA_DEBUG

#ifndef XMA_DEBUG
#define DEBUGP(x, ...)
#else
#define DEBUGP(x, ...) LOG_DEBUG(XMA, x, ##__VA_ARGS__);
#endif

namespace Xe {
namespace PCIDev {

XMA::XMA(const std::string &deviceName, u64 size, PCIBridge* parentPCIBridge, RAM* ram) 
    : PCIDevice(deviceName, size), parentBus(parentPCIBridge), ramPtr(ram) {
  // Set PCI Properties
  pciConfigSpace.configSpaceHeader.reg0.hexData = 0x58011414;
  pciConfigSpace.configSpaceHeader.reg1.hexData = 0x02000002;
  // Set our PCI Dev Sizes
  pciDevSizes[0] = 0x400; // BAR0

  // Initialize work event
  workEvent = std::make_unique<std::condition_variable>();

  // Start worker thread
  workerRunning = true;
  workerThread = std::thread(&XMA::WorkerThreadMain, this);

  LOG_INFO(XMA, "XMA decoder initialized with {} contexts", XMA_CONTEXT_COUNT);
}

XMA::~XMA() {
  Shutdown();
}


void XMA::Shutdown() {
  workerRunning = false;

  if (workEvent) {
    workEvent->notify_all();
  }

  if (paused) {
    Resume();
  }

  if (workerThread.joinable()) {
    workerThread.join();
  }

  LOG_INFO(XMA, "XMA decoder shutdown");
}

void XMA::WorkerThreadMain() {
  Base::SetCurrentThreadName("[Xe] XMA Decoder");

  u32 idleLoopCount = 0;

  while (workerRunning && XeRunning) {
    bool didWork = false;

    // Process all enabled contexts
    for (u32 n = 0; n < XMA_CONTEXT_COUNT && workerRunning; n++) {
      XmaContext& context = contexts[n];
      didWork = context.Work() || didWork;
    }

    // Handle pause/resume
    if (paused) {
      std::unique_lock<std::mutex> lock(pauseMutex);
      pauseCV.notify_all();
      resumeCV.wait(lock, [this] { return !paused || !workerRunning; });
    }

    // Update idle counter
    if (!didWork) {
      idleLoopCount++;
    } else {
      idleLoopCount = 0;
    }

    // Sleep when idle for extended period
    if (idleLoopCount > 500) {
      std::unique_lock<std::mutex> lock(workMutex);
      workEvent->wait_for(lock, std::chrono::milliseconds(20));
    }

    std::this_thread::yield();
  }

  LOG_INFO(XMA, "XMA worker thread exiting");
}

s32 XMA::GetContextId(u32 guestPtr) {
  if (contextDataFirstPtr == 0 || contextDataLastPtr == 0) {
    return -1;
  }
  
  if (guestPtr < contextDataFirstPtr || guestPtr > contextDataLastPtr) {
    return -1;
  }
  
  // Check alignment
  if ((guestPtr & 0x3F) != 0) {
    return -1;
  }
  
  return (guestPtr - contextDataFirstPtr) >> 6;
}


void XMA::ReleaseContext(u32 guestPtr) {
  s32 contextId = GetContextId(guestPtr);
  if (contextId < 0) {
    return;
  }
  
  XmaContext& context = contexts[contextId];
  context.Release();
}

bool XMA::BlockOnContext(u32 guestPtr, bool poll) {
  s32 contextId = GetContextId(guestPtr);
  if (contextId < 0) {
    return false;
  }
  
  XmaContext& context = contexts[contextId];
  return context.Block(poll);
}

void XMA::Pause() {
  if (paused) {
    return;
  }
  
  paused = true;
  
  std::unique_lock<std::mutex> lock(pauseMutex);
  pauseCV.wait(lock);
}

void XMA::Resume() {
  if (!paused) {
    return;
  }
  
  paused = false;
  resumeCV.notify_all();
}

void XMA::IssueInterrupt() {
  if (parentBus) {
    parentBus->RouteInterrupt(PRIO_XMA);
  }
}

// PCI Read
void XMA::Read(u64 readAddress, u8 *data, u64 size) {
  // Get register index (register offset / 4)
  u32 regIdx = (readAddress - pciConfigSpace.configSpaceHeader.BAR0) / 4;

  DEBUGP("Device Read at address {:#x}, reg {:#x}, size {}", readAddress, regIdx, size);

  u32 dataOut = 0;

  switch (regIdx) {
  case XmaRegister::ContextArrayAddress:
    dataOut = registerFile[XmaRegister::ContextArrayAddress];
    break;
  case XmaRegister::CurrentContextIndex: {
    // 0606h (1818h) is rotating context processing # set to hardware ID of
    // context being processed.
    // If bit 200h is set, the locking code will possibly collide on hardware
    // IDs and error out, so we should never set it (I think?).
    // To prevent games from seeing a stuck XMA context, return a rotating
    // number.
    u32& currentContextIndex = registerFile[XmaRegister::CurrentContextIndex];
    u32& nextContextIndex = registerFile[XmaRegister::NextContextIndex];
    currentContextIndex = nextContextIndex;
    nextContextIndex = (nextContextIndex + 1) % XMA_CONTEXT_COUNT;
    dataOut = currentContextIndex;
    break;
  }
  default:
    DEBUGP("Unhandled XMA PCI register read, index {:#x}", regIdx);
    break;
  }

  // Copy out data
  memcpy(data, &dataOut, size);
}

// PCI Write
void XMA::Write(u64 writeAddress, const u8 *data, u64 size) {
  u32 dataIn = 0;
  memcpy(&dataIn, data, size);

  // Get register index
  u32 regIdx = (writeAddress - pciConfigSpace.configSpaceHeader.BAR0) / 4;

  DEBUGP("Device Write at address {:#x}, reg {:#x}, data {:#x}, size {}", 
         writeAddress, regIdx, dataIn, size);

  // Store in register file
  registerFile[regIdx] = dataIn;

  // Handle kick registers (Context0Kick through Context9Kick)
  if (regIdx >= XmaRegister::Context0Kick && regIdx <= XmaRegister::Context9Kick) {
    // Context kick command - enable decoding for specified contexts
    u32 baseContextId = (regIdx - XmaRegister::Context0Kick) * 32;
    u32 value = dataIn;
    
    for (s32 i = 0; value && i < 32; ++i, value >>= 1) {
      if (value & 1) {
        u32 contextId = baseContextId + i;
        if (contextId < XMA_CONTEXT_COUNT) {
          contexts[contextId].Enable();
        }
      }
    }
    
    // Signal worker thread to start processing
    workEvent->notify_one();
  }
  // Handle lock registers (Context0Lock through Context9Lock)
  else if (regIdx >= XmaRegister::Context0Lock && regIdx <= XmaRegister::Context9Lock) {
    // Context lock command - disable decoding
    u32 baseContextId = (regIdx - XmaRegister::Context0Lock) * 32;
    u32 value = dataIn;
    
    for (s32 i = 0; value && i < 32; ++i, value >>= 1) {
      if (value & 1) {
        u32 contextId = baseContextId + i;
        if (contextId < XMA_CONTEXT_COUNT) {
          contexts[contextId].Disable();
        }
      }
    }
  }
  // Handle clear registers (Context0Clear through Context9Clear)
  else if (regIdx >= XmaRegister::Context0Clear && regIdx <= XmaRegister::Context9Clear) {
    // Context clear command - reset context state
    u32 baseContextId = (regIdx - XmaRegister::Context0Clear) * 32;
    u32 value = dataIn;
    
    for (s32 i = 0; value && i < 32; ++i, value >>= 1) {
      if (value & 1) {
        u32 contextId = baseContextId + i;
        if (contextId < XMA_CONTEXT_COUNT) {
          contexts[contextId].Clear();
        }
      }
    }
  }
  else {
    switch (regIdx) {
    case XmaRegister::ContextArrayAddress:
      // Context array address is set by guest
      contextDataFirstPtr = dataIn;
      contextDataLastPtr = contextDataFirstPtr + 
                           (sizeof(XMA_CONTEXT_DATA) * XMA_CONTEXT_COUNT - 1);
      
      // Re-setup contexts with new base address
      for (u32 i = 0; i < XMA_CONTEXT_COUNT; i++) {
        u32 guestPtr = contextDataFirstPtr + i * sizeof(XMA_CONTEXT_DATA);
        contexts[i].Setup(i, ramPtr, guestPtr);
      }
      
      DEBUGP("Context array address set to {:#x}", dataIn);
      break;
    default:
      DEBUGP("Unhandled XMA PCI register write, index {:#x}, data {:#x}", regIdx, dataIn);
      break;
    }
  }
}

void XMA::MemSet(u64 writeAddress, s32 data, u64 size) {
  // Convert memset to write
  u8 buffer[8];
  memset(buffer, data, std::min(size, static_cast<u64>(sizeof(buffer))));
  Write(writeAddress, buffer, size);
}

void XMA::ConfigRead(u64 readAddress, u8 *data, u64 size) {
  memcpy(data, &pciConfigSpace.data[static_cast<u8>(readAddress)], size);
}

void XMA::ConfigWrite(u64 writeAddress, const u8 *data, u64 size) {
  // Check if we're being scanned
  u64 tmp = 0;
  memcpy(&tmp, data, size);
  if (static_cast<u8>(writeAddress) >= 0x10 && static_cast<u8>(writeAddress) < 0x34) {
    const u32 regOffset = (static_cast<u8>(writeAddress) - 0x10) >> 2;
    if (pciDevSizes[regOffset] != 0) {
      if (tmp == 0xFFFFFFFF) { // PCI BAR Size discovery
        u64 x = 2;
        for (int idx = 2; idx < 31; idx++) {
          tmp &= ~x;
          x <<= 1;
          if (x >= pciDevSizes[regOffset]) {
            break;
          }
        }
        tmp &= ~0x3;
      }
    }
    if (static_cast<u8>(writeAddress) == 0x30) { // Expansion ROM Base Address
      tmp = 0; // Register not implemented
    }
  }

  memcpy(&pciConfigSpace.data[static_cast<u8>(writeAddress)], &tmp, size);
}

} // namespace PCIDev
} // namespace Xe
