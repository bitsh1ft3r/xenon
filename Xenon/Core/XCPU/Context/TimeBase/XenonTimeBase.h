/***************************************************************/
/* Copyright 2025 Xenon Emulator Project. All rights reserved. */
/***************************************************************/

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <array>

#include "Base/Types.h"

// Forward declarations
struct sPPEState;

namespace Xe::XCPU {

// ---------------------------------------------------------------------------
// XenonTimeBase
//
// QEMU-style lazy timebase implementation for the Xbox 360 Xenon CPU.
//
// Design contract
// ---------------
//  * The Time Base (TB) register is a free-running 64-bit counter driven at
//    XB_FREQ (50 MHz on real hardware).  Rather than incrementing it on every
//    emulator tick ("push" model) we record a single (hostEpoch, tbEpoch) pair
//    and derive the current TB value on demand using the host high-resolution
//    clock ("lazy" / "pull" model).  This eliminates timer-thread drift and
//    removes all per-instruction TB bookkeeping overhead.
//
//  * Each hardware thread has its own Decrementer (DEC) register.  When
//    software writes DEC we compute the host-clock deadline at which the
//    decrementer will underflow (reach 0x80000000 wrap) and store it.  A
//    single dedicated thread sleeps until the nearest deadline, then raises
//    the interrupt by writing ppuDecrementerEx / ppuHypervisorDecrementerEx
//    directly into sPPUThread::exceptReg.  The IIC is not involved.
//
//  * HID6[tb_enable] is respected: while disabled both TB advancement and
//    decrementer countdowns are frozen.
//
// Thread-safety
// -------------
//  * Epoch fields are protected by epochMutex.
//  * Per-thread DEC deadlines are protected by decMutex.
//  * The decrementer wakeup thread is signalled via decCV whenever a new
//    deadline is registered or the nearest deadline changes.
// ---------------------------------------------------------------------------
class XenonTimeBase {
public:
    // Xbox 360 timebase frequency: 50 MHz
    static constexpr u64 XB_FREQ = 50'000'000ULL;

    // Sentinel meaning "no active deadline"
    static constexpr auto NO_DEADLINE =
        std::chrono::steady_clock::time_point::max();

    // Maximum number of hardware threads (3 PPEs × 2 threads)
    static constexpr u8 MAX_THREADS = 6;

    // -----------------------------------------------------------------------
    // Construction / destruction
    // -----------------------------------------------------------------------
    XenonTimeBase();
    ~XenonTimeBase();

    // Must be called once all PPU objects are fully constructed, before any
    // guest code runs.  ppeStates[i] is the sPPEState for PPU i (threads
    // 2i and 2i+1 live inside it).
    void Init(std::array<sPPEState*, 3> ppeStates);

    // -----------------------------------------------------------------------
    // Timebase enable / disable  (mirrors HID6[tb_enable])
    // -----------------------------------------------------------------------
    void SetEnabled(bool enabled);
    bool IsEnabled() const { return tbEnabled.load(std::memory_order_relaxed); }

    // -----------------------------------------------------------------------
    // TB read (lazy – called from the interpreter SPR read path)
    // Returns the current guest TB value derived from the host clock.
    // -----------------------------------------------------------------------
    u64  ReadTB() const;

    // -----------------------------------------------------------------------
    // TB write  (mtspr TBL/TBU – hypervisor only)
    // Re-anchors the epoch to the current host time with the supplied value.
    // -----------------------------------------------------------------------
    void WriteTBL(u32 value);
    void WriteTBU(u32 value);

    // -----------------------------------------------------------------------
    // DEC read (lazy – called from the interpreter SPR read path)
    // threadId is the PIR value (0-5).
    // -----------------------------------------------------------------------
    s32  ReadDEC(u8 threadId) const;

    // -----------------------------------------------------------------------
    // DEC write  (mtspr DEC)
    // Records the new DEC value and schedules the decrementer interrupt.
    // threadId is the PIR value (0-5).
    // -----------------------------------------------------------------------
    void WriteDEC(u8 threadId, s32 value);

    // -----------------------------------------------------------------------
    // HDEC read / write  (hypervisor decrementer, per-PPE not per-thread)
    // ppuId is 0-2.
    // -----------------------------------------------------------------------
    s32  ReadHDEC(u8 ppuId) const;
    void WriteHDEC(u8 ppuId, s32 value);

private:
    // ------------------------------------------------------------------
    // Internal helpers
    // ------------------------------------------------------------------

    // Convert a host duration (nanoseconds) to guest TB ticks.
    static u64 NsToTicks(u64 ns) {
        // ticks = ns * XB_FREQ / 1e9
        // Use 128-bit intermediate to avoid overflow for large ns values.
        // We approximate with: ticks = (ns * 50) / 1000  (= ns/20)
        return ns / 20ULL;  // 1 tick = 20 ns at 50 MHz
    }

    // Convert guest TB ticks to nanoseconds.
    static u64 TicksToNs(u64 ticks) {
        return ticks * 20ULL;  // 1 tick = 20 ns at 50 MHz
    }

    // Re-anchor epoch to now.  Caller must hold epochMutex.
    void ReanchorLocked();

    // Compute absolute host deadline for a DEC value written now.
    // Returns NO_DEADLINE if tb is disabled or value is already negative.
    std::chrono::steady_clock::time_point
    ComputeDecDeadline(s32 decValue) const;

    // Arm or re-arm a DEC deadline for the given thread.
    // Caller must NOT hold decMutex.
    void ArmDecDeadline(u8 threadId, s32 decValue);

    // Arm or re-arm an HDEC deadline for the given PPU.
    void ArmHDecDeadline(u8 ppuId, s32 hdecValue);

    // Entry point for the decrementer watcher thread.
    void DecThread();

    // Fire the decrementer interrupt on the given hardware thread.
    void FireDecInterrupt(u8 threadId);

    // Fire the hypervisor decrementer interrupt on the given PPU.
    void FireHDecInterrupt(u8 ppuId);

    // ------------------------------------------------------------------
    // Epoch state  (protected by epochMutex)
    // ------------------------------------------------------------------
    mutable std::mutex epochMutex;

    // Host time at which tbEpoch was recorded.
    std::chrono::steady_clock::time_point hostEpoch;

    // Guest TB value at hostEpoch.
    u64 tbEpoch = 0;

    // ------------------------------------------------------------------
    // Enable flag  (atomic – read from hot interpreter path)
    // ------------------------------------------------------------------
    std::atomic<bool> tbEnabled{ true };

    // ------------------------------------------------------------------
    // Per-thread DEC state  (protected by decMutex)
    // ------------------------------------------------------------------
    mutable std::mutex decMutex;
    std::condition_variable decCV;

    struct DecState {
        // Host-clock absolute time when this decrementer reaches zero.
        std::chrono::steady_clock::time_point deadline{ NO_DEADLINE };
        // The DEC value at the moment it was written (for lazy reads).
        s32  valueAtWrite = 0x7FFFFFFF;
        // Host time at which the above value was written.
        std::chrono::steady_clock::time_point writeTime{};
        // True while a pending interrupt has been raised but not yet
        // acknowledged (prevents re-firing on every wakeup).
        bool pending = false;
    };

    std::array<DecState, MAX_THREADS> decState{};

    // Per-PPU HDEC state (indices 0-2)
    struct HDecState {
        std::chrono::steady_clock::time_point deadline{ NO_DEADLINE };
        s32  valueAtWrite = 0x7FFFFFFF;
        std::chrono::steady_clock::time_point writeTime{};
        bool pending = false;
    };
    std::array<HDecState, 3> hdecState{};

    // ------------------------------------------------------------------
    // Decrementer watcher thread
    // ------------------------------------------------------------------
    std::thread           decThread;
    std::atomic<bool>     decThreadActive{ false };

    // ------------------------------------------------------------------
    // External references (set by Init, never changed afterwards)
    // ------------------------------------------------------------------
    std::array<sPPEState*, 3> ppeStates{ nullptr, nullptr, nullptr };
};

} // namespace Xe::XCPU
