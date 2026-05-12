/***************************************************************/
/* Copyright 2025 Xenon Emulator Project. All rights reserved. */
/***************************************************************/

#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "Core/PCI/PCIDevice.h"
#include "Core/PCI/Bridge/PCIBridge.h"
#include "Core/RAM/RAM.h"
#include "XMAContext.h"
#include "XMATypes.h"

#define XMA_DEV_SIZE 0x400

namespace Xe {
namespace PCIDev {

// XMA PCI Registers and offsets
// Taken from:
// https://github.com/xenia-canary/xenia-canary/blob/canary_experimental/src/xenia/apu/xma_register_table.inc
enum eXMARegister : u32 {
  ContextArrayAddress = 0x00,
  CurrentContextIndex = 0x18,
  NextContextIndex = 0x1C,
  // Unknown Group at 0x40
  Unknown_Group_0x40 = 0x40,
  Unknown_Group_0x44 = 0x44,
  Unknown_Group_0x48 = 0x48,
  Unknown_Group_0x4C = 0x4C,
  Unknown_Group_0x50 = 0x50,
  Unknown_Group_0x54 = 0x54,
  Unknown_Group_0x58 = 0x58,
  Unknown_Group_0x5C = 0x5C,
  Unknown_Group_0x60 = 0x60,
  Unknown_Group_0x64 = 0x64,
  // Unknown Group at 0x80
  Unknown_Group_0x80 = 0x80,
  Unknown_Group_0x84 = 0x84,
  Unknown_Group_0x88 = 0x88,
  Unknown_Group_0x8C = 0x8C,
  Unknown_Group_0x90 = 0x90,
  Unknown_Group_0x94 = 0x94,
  Unknown_Group_0x98 = 0x98,
  Unknown_Group_0x9C = 0x9C,
  Unknown_Group_0xA0 = 0xA0,
  Unknown_Group_0xA4 = 0xA4,
  // Context Kick
  Context0Kick = 0x140,
  Context1Kick = 0x144,
  Context2Kick = 0x148,
  Context3Kick = 0x14C,
  Context4Kick = 0x150,
  Context5Kick = 0x154,
  Context6Kick = 0x158,
  Context7Kick = 0x15C,
  Context8Kick = 0x160,
  Context9Kick = 0x164,
  // Unknown Group at 0x180
  Unknown_Group_0x180 = 0x180,
  Unknown_Group_0x184 = 0x184,
  Unknown_Group_0x188 = 0x188,
  Unknown_Group_0x18C = 0x18C,
  Unknown_Group_0x190 = 0x190,
  Unknown_Group_0x194 = 0x194,
  Unknown_Group_0x198 = 0x198,
  Unknown_Group_0x19C = 0x19C,
  Unknown_Group_0x1A0 = 0x1A0,
  Unknown_Group_0x1A4 = 0x1A4,
  // Context Lock
  Context0Lock = 0x240,
  Context1Lock = 0x244,
  Context2Lock = 0x248,
  Context3Lock = 0x24C,
  Context4Lock = 0x250,
  Context5Lock = 0x254,
  Context6Lock = 0x258,
  Context7Lock = 0x25C,
  Context8Lock = 0x260,
  Context9Lock = 0x264,
  // Context Clear
  Context0Clear = 0x280,
  Context1Clear = 0x284,
  Context2Clear = 0x288,
  Context3Clear = 0x28C,
  Context4Clear = 0x290,
  Context5Clear = 0x294,
  Context6Clear = 0x298,
  Context7Clear = 0x29C,
  Context8Clear = 0x2A0,
  Context9Clear = 0x2A4,
};

// XMA Register File - stores all XMA registers
class XmaRegisterFile {
public:
  static constexpr u32 kRegisterCount = 0x400 / 4; // 256 registers

  u32& operator[](u32 index) { return registers[index]; }
  const u32& operator[](u32 index) const { return registers[index]; }

private:
  std::array<u32, kRegisterCount> registers = {};
};

class XMA : public PCIDevice {
public:
  XMA(const std::string &deviceName, u64 size, PCIBridge* parentPCIBridge, RAM* ram);
  ~XMA();

  void Read(u64 readAddress, u8 *data, u64 size) override;
  void Write(u64 writeAddress, const u8 *data, u64 size) override;
  void MemSet(u64 writeAddress, s32 data, u64 size) override;
  void ConfigRead(u64 readAddress, u8* data, u64 size) override;
  void ConfigWrite(u64 writeAddress, const u8* data, u64 size) override;

  void Shutdown();

  // Context management
  void ReleaseContext(u32 guestPtr);
  bool BlockOnContext(u32 guestPtr, bool poll);

  // Pause/Resume for debugging
  void Pause();
  void Resume();

  bool IsPaused() const { return paused; }

private:
  // Get context ID from guest pointer
  s32 GetContextId(u32 guestPtr);

  // Worker thread main loop
  void WorkerThreadMain();

  // Route interrupt to CPU
  void IssueInterrupt();

private:
  // Parent PCI bridge for interrupt routing
  PCIBridge* parentBus = nullptr;
  
  // RAM pointer for memory access
  RAM* ramPtr = nullptr;

  // Register file
  XmaRegisterFile registerFile;

  // XMA Contexts
  std::array<XmaContext, XMA_CONTEXT_COUNT> contexts;

  // Context data pointers
  u32 contextDataFirstPtr = 0;
  u32 contextDataLastPtr = 0;

  // Worker thread
  std::atomic<bool> workerRunning = false;
  std::thread workerThread;
  std::unique_ptr<std::condition_variable> workEvent;
  std::mutex workMutex;

  // Pause/Resume synchronization
  bool paused = false;
  std::condition_variable pauseCV;
  std::condition_variable resumeCV;
  std::mutex pauseMutex;
};

} // namespace PCIDev
} // namespace Xe
