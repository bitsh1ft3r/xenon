/***************************************************************/
/* Copyright 2025 Xenon Emulator Project. All rights reserved. */
/***************************************************************/

#pragma once

#include <memory>
#include <condition_variable>
#include <mutex>

#include "PowerPC.h"
#include "Core/XCPU/Context/XenonContext.h"
#include "Core/RootBus/RootBus.h"
#include "Core/XCPU/MMU/XenonMMU.h"

class PPU_JIT;

namespace Xe { namespace XCPU { namespace JIT { class PPCTranslator; } } }

// Describes the execution backends available for the PPU.
enum class eExecutorMode : u8 {
  Interpreter,
  JIT,
  Hybrid
};

// Per-guest-thread state that lives on the PPU (host-side scheduling state).
struct PPUHostThreadState {
  // Host thread handle
  std::thread hostThread{};
  // Thread execution state
  std::atomic<eThreadState> threadState{ eThreadState::None };
  // Previous state before halting (for resume)
  std::atomic<eThreadState> previousThreadState{ eThreadState::None };
  // Thread active flag
  std::atomic<bool> threadActive{ true };
  // Thread resetting flag
  std::atomic<bool> threadResetting{ false };
  // Amount of instructions to step (debug)
  u64 stepAmount = 0;
  // If this is set, then the guest requested us to halt
  bool guestHalt = false;
};

// Power Procesing Unit. Main execution unit inside the PPE's within the Xenon CPU.
class PPU {
public:
  PPU(Xe::XCPU::XenonContext * inXenonContext, u64 resetVector, u32 PIR);
  ~PPU();

  // Start execution
  void StartExecution(bool setHRMOR = true);

  // Reset the PPU state
  void Reset();

  // Debug tools
  void Halt(u64 haltOn = 0, bool requestedByGuest = false, s8 ppuId = 0, ePPUThreadID threadId = ePPUThread_None);
  void Continue();
  void ContinueFromException();
  void Step(int amount = 1);

  // Thread state machine (per guest thread)
  void ThreadStateMachine(ePPUThreadID threadId);

  // Thread function (per guest thread)
  void ThreadLoop(ePPUThreadID threadId);

  // Returns a pointer to a thread
  sPPUThread *GetPPUThread(u8 thrdID);

  // Runs a specified number of instructions on the given guest thread
  void PPURunInstructions(u64 numInstrs, ePPUThreadID threadId, bool enableHalt = true);

  // Checks if any thread is active
  bool ThreadActive() {
    return hostThreads[0].threadState.load() == eThreadState::Executing ||
           hostThreads[0].threadState.load() == eThreadState::Running ||
           hostThreads[1].threadState.load() == eThreadState::Executing ||
           hostThreads[1].threadState.load() == eThreadState::Running;
  }

  // Checks if any thread is halted
  bool IsHalted() {
    return hostThreads[0].threadState.load() == eThreadState::Halted ||
           hostThreads[1].threadState.load() == eThreadState::Halted;
  }

  // Checks if halted by guest
  bool IsHaltedByGuest() {
    return (hostThreads[0].guestHalt && hostThreads[0].threadState.load() == eThreadState::Halted) ||
           (hostThreads[1].guestHalt && hostThreads[1].threadState.load() == eThreadState::Halted);
  }

  // Returns the thread state for a specific guest thread
  eThreadState ThreadState(ePPUThreadID id = ePPUThread_Zero) { return hostThreads[static_cast<u8>(id)].threadState.load(); }

  // Get ppeState
  sPPEState *GetPPUState() { return ppeState.get(); }
  // Get ppuJIT
  PPU_JIT *GetPPUJIT() { return ppuJIT.get(); }
  // Get cpu context
  Xe::XCPU::XenonContext *GetCPUContext() { return xenonContext; }

  // Load a elf image from host memory. Copies into RAM
  // Returns entrypoint
  u64 loadElfImage(u8 *data, u64 size);

  FILE *traceFile;

  eExecutorMode currentExecMode = eExecutorMode::Interpreter;
private:
  // Per-guest-thread host thread state (index 0 = Thread0, index 1 = Thread1)
  PPUHostThreadState hostThreads[2]{};

  // If this is set to a non-zero value, it will halt on that address then clear it
  u64 ppuHaltOn = 0;

  // Execution threads inside this PPU.
  std::unique_ptr<sPPEState> ppeState;

  // Main CPU Context.
  Xe::XCPU::XenonContext *xenonContext = nullptr;

  // Xenon Memory Management Unit
  std::unique_ptr<Xe::XCPU::MMU::XenonMMU> xenonMMU;

  // Initial reset vector
  u32 resetVector = 0;

  //
  // Exceptions
  //

  // Process Synchronous exceptions
  void PPUProcessSyncExceptions(sPPEState* ppeState);

  // Process Asynchronous exceptions
  void PPUProcessAsyncExceptions(sPPEState* ppeState);

  void PPUSystemResetException(sPPEState* ppeState);
  void PPUInstStorageException(sPPEState* ppeState);
  void PPUDataStorageException(sPPEState* ppeState);
  void PPUDataSegmentException(sPPEState* ppeState);
  void PPUInstSegmentException(sPPEState* ppeState);
  void PPUSystemCallException(sPPEState* ppeState);
  void PPUDecrementerException(sPPEState* ppeState);
  void PPUHypDecrementerException(sPPEState* ppeState);
  void PPUProgramException(sPPEState* ppeState);
  void PPUExternalException(sPPEState* ppeState);
  void PPUFPUnavailableException(sPPEState* ppeState);
  void PPUVXUnavailableException(sPPEState* ppeState);

  //
  // JIT
  //

  std::unique_ptr<PPU_JIT> ppuJIT;
  friend class PPU_JIT;
  friend class Xe::XCPU::JIT::PPCTranslator;

  //
  // Helpers
  //
 
  // Read next intruction from memory (uses explicit thread ID)
  bool PPUReadNextInstruction(ePPUThreadID threadId);
  // Checks for pending exceptions
  bool PPUCheckInterrupts(ePPUThreadID threadId);
  // Checks for pending exceptions
  bool PPUCheckExceptions(ePPUThreadID threadId);
  // Checks if a specific guest thread is enabled via CTRL register.
  bool IsGuestThreadEnabled(ePPUThreadID threadId);
  // Simulates the behavior of the 1BL inside the Xenon Secure ROM.
  bool Simulate1Bl();

  //
  // Testing Utilities
  //
  
  // Runs instruction tests on the desired backend.
  bool RunInstructionTests(sPPEState* ppeState, PPU_JIT* ppuJITPtr, ePPUTestingMode testMode);
};
