// Minimal xe::Memory shim for xenia GPU integration into Xenon
// Wraps Xenon's RAM* to provide the interface xenia's GPU code expects.
#pragma once

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <utility>
#include <vector>

// Forward declare Xenon's RAM class to avoid pulling in Xenon headers.
class RAM;

namespace xe {

class Memory {
 public:
  // System page size used for invalidation tracking granularity.
  // 4 KB matches the Xbox 360's smallest page size and xenia's default.
  static constexpr uint32_t kPageSizeLog2 = 12;
  static constexpr uint32_t kPageSize = 1u << kPageSizeLog2;
  // 512 MB physical address space.
  static constexpr uint32_t kPhysicalSize = 512 * 1024 * 1024;
  static constexpr uint32_t kPageCount = kPhysicalSize >> kPageSizeLog2;
  // Number of uint64_t blocks needed to hold one bit per page.
  static constexpr uint32_t kWatchBlockCount = (kPageCount + 63) / 64;

  Memory() {
    // Zero the watched-pages bitset — nothing is watched initially.
    memset(watched_pages_, 0, sizeof(watched_pages_));
  }

  // Wire this shim to Xenon's RAM. Called once during bridge init
  // `base` is RAM::GetPointerToAddress(0), `size` is RAM::GetSize()
  void SetPhysicalBase(uint8_t* base, uint64_t size) {
    physical_base_ = base;
    mem_size_ = size;
  }

  uint8_t* physical_membase() const { return physical_base_; }

  // Xbox 360 has 512 MB physical RAM at address 0.
  // Xenia masks with 0x1FFFFFFF to stay within bounds.
  // TODO: expand the mask accordingly since we can also emulate 1Gb XDK's
  uint8_t* TranslatePhysical(uint32_t guest_address) const {
    return physical_base_ + (guest_address & 0x1FFFFFFF);
  }

  template <typename T>
  T TranslatePhysical(uint32_t guest_address) const {
    return reinterpret_cast<T>(physical_base_ + (guest_address & 0x1FFFFFFF));
  }


  // Physical memory invalidation callbacks
  // Used by SharedMemory and PrimitiveProcessor
  using PhysicalMemoryInvalidationCallback =
      std::pair<uint32_t, uint32_t> (*)(void* context_ptr,
                                        uint32_t physical_address_start,
                                        uint32_t length, bool exact_range);

  void* RegisterPhysicalMemoryInvalidationCallback(
      PhysicalMemoryInvalidationCallback callback, void* context) {
    std::lock_guard<std::mutex> lock(callbacks_mutex_);
    auto* entry = new CallbackEntry{callback, context};
    callbacks_.push_back(entry);
    return static_cast<void*>(entry);
  }

  void UnregisterPhysicalMemoryInvalidationCallback(void* handle) {
    if (!handle) return;
    std::lock_guard<std::mutex> lock(callbacks_mutex_);
    auto it = std::find(callbacks_.begin(), callbacks_.end(),
                        static_cast<CallbackEntry*>(handle));
    if (it != callbacks_.end()) {
      callbacks_.erase(it);
    }
    delete static_cast<CallbackEntry*>(handle);
  }

  // Mark a range of physical pages as "watched". When the CPU writes to any
  // watched page, the registered invalidation callbacks will be fired.
  // This is called by SharedMemory::MakeRangeValid() after uploading data to
  // the GPU, so subsequent CPU writes are detected.
  void EnablePhysicalMemoryAccessCallbacks(
      uint32_t physical_address, uint32_t length,
      bool enable_invalidation_notifications,
      bool enable_access_callbacks) {
    if (!enable_invalidation_notifications || length == 0) return;
    // Clamp to the 512 MB physical window.
    physical_address &= 0x1FFFFFFF;
    if (physical_address >= kPhysicalSize) return;
    if (physical_address + length > kPhysicalSize) {
      length = kPhysicalSize - physical_address;
    }

    uint32_t page_first = physical_address >> kPageSizeLog2;
    uint32_t page_last = (physical_address + length - 1) >> kPageSizeLog2;

    std::lock_guard<std::mutex> lock(watch_mutex_);
    for (uint32_t p = page_first; p <= page_last; ++p) {
      watched_pages_[p / 64] |= (uint64_t(1) << (p % 64));
    }
  }

  // Called by Xenon's RAM (or the bridge) whenever the CPU writes to physical
  // memory.  If any page in [physical_address, physical_address+length) is
  // watched, we fire every registered invalidation callback and clear the
  // watched bits for the range (the callback's return value is not used for
  // re-watching — SharedMemory will call EnablePhysicalMemoryAccessCallbacks
  // again after re-uploading).
  // This mimics the behavior in xenia used by the GPU
  // Can also be expanded in the future for JIT invalidation callbacks from main RAM
  void HandlePhysicalMemoryWrite(uint32_t physical_address, uint32_t length) {
    if (length == 0) return;
    physical_address &= 0x1FFFFFFF;
    if (physical_address >= kPhysicalSize) return;
    if (physical_address + length > kPhysicalSize) {
      length = kPhysicalSize - physical_address;
    }

    uint32_t page_first = physical_address >> kPageSizeLog2;
    uint32_t page_last = (physical_address + length - 1) >> kPageSizeLog2;

    // Fast path: check if any page in the range is actually watched.
    bool any_watched = false;
    {
      std::lock_guard<std::mutex> lock(watch_mutex_);
      for (uint32_t p = page_first; p <= page_last; ++p) {
        if (watched_pages_[p / 64] & (uint64_t(1) << (p % 64))) {
          any_watched = true;
          break;
        }
      }
      if (!any_watched) return;

      // Clear watched bits for this range — the callbacks will re-enable
      // watching after the next upload.
      for (uint32_t p = page_first; p <= page_last; ++p) {
        watched_pages_[p / 64] &= ~(uint64_t(1) << (p % 64));
      }
    }

    // Fire all registered invalidation callbacks.
    std::lock_guard<std::mutex> lock(callbacks_mutex_);
    for (auto* entry : callbacks_) {
      entry->callback(entry->context, physical_address, length,
                      /*exact_range=*/true);
    }
  }

 private:
  struct CallbackEntry {
    PhysicalMemoryInvalidationCallback callback;
    void* context;
  };

  uint8_t* physical_base_ = nullptr;
  uint64_t mem_size_ = 0;

  // Registered invalidation callbacks.
  std::mutex callbacks_mutex_;
  std::vector<CallbackEntry*> callbacks_;

  // Bitset: one bit per 4 KB page across the 512 MB physical address space.
  // When a bit is set, the corresponding page is "watched" and CPU writes
  // will trigger invalidation callbacks.
  std::mutex watch_mutex_;
  uint64_t watched_pages_[kWatchBlockCount];
};

}  // namespace xe
