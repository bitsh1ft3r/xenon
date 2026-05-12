/***************************************************************/
/* Copyright 2025 Xenon Emulator Project. All rights reserved. */
/***************************************************************/

#pragma once

#ifdef XENIA_D3D12_GPU

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>

// Forward declarations — avoid pulling Xenon headers into this header.
class RAM;
class PCIBridge;

namespace xe {
class Memory;
namespace cpu {
class Processor;
}
namespace kernel {
class KernelState;
}
namespace gpu {
class GraphicsSystem;
class CommandProcessor;
class RegisterFile;
namespace d3d12 {
class D3D12GraphicsSystem;
}
}  // namespace gpu
namespace ui {
class Win32WindowedAppContext;
class Window;
}  // namespace ui
}  // namespace xe

namespace Xe::Xenos {

/// Bridge between Xenon's XGPU and Xenia's D3D12 GPU backend.
///
/// Lifecycle:
///   1. Construct with a RAM* and PCIBridge*.
///   2. Call Initialize() — creates the xe::Memory shim, stubs, and
///      D3D12GraphicsSystem.  Opens a Win32 window with a D3D12 swap chain.
///   3. Call ForwardRegisterWrite() from XenosState::WriteRawRegister for every
///      GPU register write.  The bridge mirrors it into xenia's RegisterFile
///      and, for ring-buffer registers, pokes xenia's command processor.
///   4. Call Shutdown() or destroy to tear down.
class XeniaD3D12Bridge {
 public:
  XeniaD3D12Bridge(RAM* ram, PCIBridge* pci_bridge);
  ~XeniaD3D12Bridge();

  /// Create xenia subsystems and the D3D12 graphics system.
  /// Returns true on success.
  bool Initialize();

  /// Tear down xenia subsystems.
  void Shutdown();

  /// Mirror a register write into xenia's RegisterFile and forward
  /// ring-buffer control registers to the command processor.
  /// @param reg_index  Register index (addr / 4).
  /// @param value      Value being written (host byte order).
  void ForwardRegisterWrite(uint32_t reg_index, uint32_t value);

  u32 ForwardRegisterRead(uint32_t reg_index);

  /// Returns true if Initialize() succeeded.
  bool IsActive() const { return active_.load(std::memory_order_relaxed); }

 private:
  /// Entry point for the dedicated Win32 UI thread.
  void UIThreadMain();

  RAM* ram_ = nullptr;
  PCIBridge* pci_bridge_ = nullptr;

  // Xenia subsystem stubs (owned).
  std::unique_ptr<xe::Memory> memory_;
  std::unique_ptr<xe::cpu::Processor> processor_;
  std::unique_ptr<xe::kernel::KernelState> kernel_state_;

  // The D3D12 graphics system (owned by this bridge).
  std::unique_ptr<xe::gpu::d3d12::D3D12GraphicsSystem> graphics_system_;

  // Win32 UI thread and its owned objects.
  std::thread ui_thread_;
  // These are created/destroyed on the UI thread only.
  std::unique_ptr<xe::ui::Win32WindowedAppContext> app_context_;
  std::unique_ptr<xe::ui::Window> window_;

  // Quick access cached from graphics_system_.
  xe::gpu::CommandProcessor* command_processor_ = nullptr;
  xe::gpu::RegisterFile* register_file_ = nullptr;

  std::atomic<bool> active_{false};
  // Signaled by the UI thread once initialization is complete.
  std::atomic<bool> ui_thread_ready_{false};
  // Set to request the UI thread to quit.
  std::atomic<bool> ui_thread_quit_{false};
};

}  // namespace Xe::Xenos

#endif  // XENIA_D3D12_GPU
