/***************************************************************/
/* Copyright 2025 Xenon Emulator Project. All rights reserved. */
/***************************************************************/

#ifdef XENIA_D3D12_GPU

#include "XeniaD3D12Bridge.h"

#include "Core/RAM/RAM.h"

#include "xenia/memory.h"
#include "xenia/cpu/processor.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/gpu/command_processor.h"
#include "xenia/gpu/d3d12/d3d12_graphics_system.h"
#include "xenia/gpu/register_file.h"
#include "xenia/ui/windowed_app_context_win.h"
#include "xenia/ui/window.h"
#include "xenia/base/logging.h"

#include "Base/Logging/Log.h"

#include <future>

// Xenos register indices for the ring buffer
static constexpr u32 CP_RB_BASE = 0x01C0;
static constexpr u32 CP_RB_CNTL = 0x01C1;
static constexpr u32 CP_RB_RPTR_ADDR = 0x01C3;
static constexpr u32 CP_RB_WPTR = 0x01C5;

namespace Xe::Xenos {

XeniaD3D12Bridge::XeniaD3D12Bridge(RAM* ram, PCIBridge* pci_bridge)
    : ram_(ram), pci_bridge_(pci_bridge) {}

XeniaD3D12Bridge::~XeniaD3D12Bridge() { Shutdown(); }

bool XeniaD3D12Bridge::Initialize() {
  if (active_) return true;

  // 1. Create xe::Memory shim pointing at Xenon's RAM
  memory_ = std::make_unique<xe::Memory>();
  memory_->SetPhysicalBase(ram_->GetPointerToAddress(0),
                           ram_->GetSize());

  // Wire RAM write notifications into xe::Memory so that GPU SharedMemory
  // is notified whenever the CPU modifies physical memory.
  {
    xe::Memory* mem = memory_.get();
    ram_->SetWriteNotifyCallback(
        [mem](u32 physical_address, u32 length) {
          mem->HandlePhysicalMemoryWrite(physical_address, length);
        });
  }

  // 2. Create stubs for Processor and KernelState (will be replaced later)
  processor_ = std::make_unique<xe::cpu::Processor>(memory_.get(), pci_bridge_);

  kernel_state_ = std::make_unique<xe::kernel::KernelState>(memory_.get());

  // 3. Create D3D12GraphicsSystem
  graphics_system_ =
      std::make_unique<xe::gpu::d3d12::D3D12GraphicsSystem>();

  // Initialize logging
  //xe::InitializeLogging("Xenon");

  // 4. Launch the dedicated Win32 UI thread
  ui_thread_ready_ = false;
  ui_thread_quit_ = false;
  ui_thread_ = std::thread(&XeniaD3D12Bridge::UIThreadMain, this);

  // Wait for the UI thread to finish initialization.
  while (!ui_thread_ready_.load(std::memory_order_acquire)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  if (!active_.load(std::memory_order_relaxed)) {
    // UI thread failed to initialize.
    if (ui_thread_.joinable()) {
      ui_thread_.join();
    }
    Shutdown();
    return false;
  }

  LOG_INFO(Xenos, "[XeniaD3D12Bridge] Initialized successfully");
  return true;
}

void XeniaD3D12Bridge::UIThreadMain() {
  // Create the Win32 app context on this thread (this thread becomes the
  // "UI thread" for xenia's purposes).
  HINSTANCE hinstance = GetModuleHandle(nullptr);
  app_context_ = std::make_unique<xe::ui::Win32WindowedAppContext>(
      hinstance, SW_SHOWNORMAL);

  if (!app_context_->Initialize()) {
    LOG_ERROR(Xenos,
              "[XeniaD3D12Bridge] Failed to initialize Win32WindowedAppContext");
    app_context_.reset();
    ui_thread_ready_ = true;  // Signal failure.
    return;
  }

  // Call D3D12GraphicsSystem::Setup with the real app context. This creates
  // the D3D12 device, presenter, and command processor.
  auto status = graphics_system_->Setup(
      processor_.get(), kernel_state_.get(),
      app_context_.get(), /*is_surface_required=*/false);

  if (status != X_STATUS_SUCCESS) {
    LOG_ERROR(Xenos,
              "[XeniaD3D12Bridge] D3D12GraphicsSystem::Setup failed "
              "with status 0x{:08X}",
              status);
    app_context_.reset();
    ui_thread_ready_ = true;  // Signal failure.
    return;
  }

  // Cache frequently-used pointers.
  command_processor_ = graphics_system_->command_processor();
  register_file_ = graphics_system_->register_file();

  // Create a Win32 window and open it.
  window_ = xe::ui::Window::Create(*app_context_, "Xenon - Xenia D3D12",
                                   1280, 720);
  if (!window_ || !window_->Open()) {
    LOG_ERROR(Xenos, "[XeniaD3D12Bridge] Failed to create/open Win32 window");
    graphics_system_->Shutdown();
    graphics_system_.reset();
    app_context_.reset();
    ui_thread_ready_ = true;  // Signal failure.
    return;
  }

  // Attach the presenter to the window so the D3D12 swap chain can present.
  auto* presenter = graphics_system_->presenter();
  if (presenter) {
    window_->SetPresenter(presenter);
    LOG_INFO(Xenos,
             "[XeniaD3D12Bridge] Presenter attached to Win32 window");
  } else {
    LOG_WARNING(Xenos,
                "[XeniaD3D12Bridge] No presenter available to attach");
  }

  // Signal success.
  active_ = true;
  ui_thread_ready_ = true;

  LOG_INFO(Xenos, "[XeniaD3D12Bridge] Win32 UI thread entering message loop");

  // Run the Win32 message loop. This blocks until the window is closed or
  // quit is requested.
  app_context_->RunMainMessageLoop();

  LOG_INFO(Xenos, "[XeniaD3D12Bridge] Win32 UI thread exiting message loop");

  // Cleanup window and app context on the UI thread (where they were created).
  if (window_) {
    window_->SetPresenter(nullptr);
    window_.reset();
  }
  // app_context_ is cleaned up in Shutdown after the thread joins.
}

void XeniaD3D12Bridge::Shutdown() {
  if (!active_.exchange(false) && !ui_thread_.joinable()) return;

  LOG_INFO(Xenos, "[XeniaD3D12Bridge] Shutting down");

  // Disconnect the RAM write notification before tearing down xe::Memory.
  if (ram_) {
    ram_->SetWriteNotifyCallback(nullptr);
  }

  // Request the UI thread's message loop to quit.
  if (app_context_) {
    app_context_->RequestDeferredQuit();
  }

  // Wait for the UI thread to exit.
  if (ui_thread_.joinable()) {
    ui_thread_.join();
  }

  // Shutdown graphics system (must happen after the UI thread has stopped
  // since the presenter was owned by it).
  if (graphics_system_) {
    graphics_system_->Shutdown();
    graphics_system_.reset();
  }

  command_processor_ = nullptr;
  register_file_ = nullptr;

  // Destroy the app context last (the window was already destroyed on the
  // UI thread).
  app_context_.reset();

  kernel_state_.reset();
  processor_.reset();
  memory_.reset();
}

u32 XeniaD3D12Bridge::ForwardRegisterRead(uint32_t reg_index) {
  if (!active_.load(std::memory_order_relaxed)) return 0;

  // Mirror every register write into xenia's RegisterFile so the command
  // processor and shader translator see the same state the game wrote.
  if (reg_index < xe::gpu::RegisterFile::kRegisterCount) {
    return register_file_->values[reg_index];
  }
}

void XeniaD3D12Bridge::ForwardRegisterWrite(u32 reg_index,
                                             u32 value) {
  if (!active_.load(std::memory_order_relaxed)) return;

  // Mirror every register write into xenia's RegisterFile so the command
  // processor and shader translator see the same state the game wrote.
  if (reg_index < xe::gpu::RegisterFile::kRegisterCount) {
    register_file_->values[reg_index] = value;
  }

  // Ring-buffer control registers need special handling — poke the CP.
  switch (reg_index) {
    case CP_RB_BASE: {
      // RB base address.  Xenia expects (ptr, size_log2) but the size is
      // written to CP_RB_CNTL separately, so we store the base and wait.
      // The CP will pick it up on the next InitializeRingBuffer call.
      // For now just store it, the real init happens via CP_RB_CNTL.
      break;
    }
    case CP_RB_CNTL: {
      // Bits [5:0] = size_log2.  Re-initialize the ring buffer.
      u32 size_log2 = value & 0x3F;
      u32 rb_base = register_file_->values[CP_RB_BASE];

      LOG_DEBUG(Xenos, "Initializing RB Base {:#x}, size {:#x}", rb_base, size_log2);
      graphics_system_->InitializeRingBuffer(rb_base, size_log2);

      // Clear caches, this prevents garbage to be passed between running titles.
      graphics_system_->ClearCaches();
      break;
    }
    case CP_RB_WPTR: {
      // Write pointer update — this is the hot path that wakes the CP.
      command_processor_->UpdateWritePointer(value);
      break;
    }
    case CP_RB_RPTR_ADDR: {
      // CP_RB_RPTR_ADDR - Ring Buffer Read Pointer Address 
      // * Bits [0:1]: RB_RPTR_SWAP 
      // Swap control of the reported read pointer address. See 
      // CP_RB_CNTL.BUF_SWAP for the encoding.
      // * Bits [2:31]: RB_RPTR_ADDR
      // Ring Buffer Read Pointer Address. Address of the Host`s 
      // copy of the Read Pointer.C
      LOG_DEBUG(Xenos, "Writing CP_RB_RPTR_ADDR {:#x}", value);
      u32 size = register_file_->values[CP_RB_CNTL];
      command_processor_->EnableReadPointerWriteBack((value & 0xFFFFFFFC), size & 0x1f); // TODO: clamping the size is correct?
      break;
    }
    default:
      break;
  }
}

}  // namespace Xe::Xenos

#endif  // XENIA_D3D12_GPU
