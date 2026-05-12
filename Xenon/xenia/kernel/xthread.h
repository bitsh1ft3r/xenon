// Minimal xe::kernel thread stubs for xenia GPU integration into Xenon
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>

#include "xenia/base/threading.h"
#include "xenia/cpu/processor.h"

namespace xe {
namespace kernel {

// Minimal ref-counted object base.
class XObject {
 public:
  virtual ~XObject() = default;
  void Retain() { ref_count_++; }
  void Release() {
    if (--ref_count_ == 0) {
      delete this;
    }
  }

 private:
  std::atomic<int> ref_count_{1};
};

// Intrusive ref-counted pointer, matching xenia's kernel::object_ref.
template <typename T>
class object_ref {
 public:
  object_ref() : ptr_(nullptr) {}
  explicit object_ref(T* p) : ptr_(p) {}
  object_ref(const object_ref& o) : ptr_(o.ptr_) {
    if (ptr_) ptr_->Retain();
  }
  object_ref(object_ref&& o) noexcept : ptr_(o.ptr_) { o.ptr_ = nullptr; }
  ~object_ref() { reset(); }

  object_ref& operator=(const object_ref& o) {
    if (this != &o) {
      reset();
      ptr_ = o.ptr_;
      if (ptr_) ptr_->Retain();
    }
    return *this;
  }
  object_ref& operator=(object_ref&& o) noexcept {
    if (this != &o) {
      reset();
      ptr_ = o.ptr_;
      o.ptr_ = nullptr;
    }
    return *this;
  }

  void reset() {
    if (ptr_) {
      ptr_->Release();
      ptr_ = nullptr;
    }
  }

  T* get() const { return ptr_; }
  T* operator->() const { return ptr_; }
  explicit operator bool() const { return ptr_ != nullptr; }

 private:
  T* ptr_;
};

class KernelState;

class XThread : public XObject {
 public:
  static XThread* GetCurrentThread() { return nullptr; }
  static bool IsInThread() { return false; }
  static bool IsInThread(XThread*) { return false; }
  void SetActiveCpu(uint32_t cpu) {}
  cpu::ThreadState* thread_state() { return nullptr; }
};

// XHostThread - wraps a std::thread to match xenia's kernel API.
// We will need to move to std::thread in callers toavoid unnecessary code.
class XHostThread : public XThread {
 public:
  XHostThread(KernelState* kernel_state, uint32_t stack_size,
              uint32_t creation_flags, std::function<int()> host_fn)
      : host_fn_(std::move(host_fn)) {}

  void set_name(const std::string& name) { name_ = name; }
  void set_can_debugger_suspend(bool value) {}

  bool Create() {
    native_thread_ = std::thread([this]() { host_fn_(); });
    return true;
  }

  void Wait(uint32_t a, uint32_t b, uint32_t c, void* d) {
    if (native_thread_.joinable()) {
      native_thread_.join();
    }
  }

  threading::Thread* thread() {
    return threading::Thread::GetCurrentThread();
  }

 private:
  std::function<int()> host_fn_;
  std::thread native_thread_;
  std::string name_;
};

}  // namespace kernel
}  // namespace xe
