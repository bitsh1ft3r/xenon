// No-op TraceWriter stub for xenia GPU integration into Xenon
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

#include "xenia/gpu/registers.h"

namespace xe {
namespace gpu {

struct EventCommand {
  enum class Type : uint32_t {
    kSwap = 0,
  };
};

class TraceWriter {
 public:
  TraceWriter(uint8_t* membase) {}
  TraceWriter() {}

  bool is_open() const { return false; }

  void Open(const std::filesystem::path& path, uint32_t title_id) {}
  void Close() {}
  void Flush() {}

  void WritePrimaryBufferStart(uint32_t base_ptr, uint32_t count) {}
  void WritePrimaryBufferEnd() {}
  void WriteIndirectBufferStart(uint32_t base_ptr, uint32_t count) {}
  void WriteIndirectBufferEnd() {}
  void WritePacketStart(uint32_t base_ptr, uint32_t count) {}
  void WritePacketEnd() {}
  void WriteMemoryRead(uint32_t base_ptr, uint32_t length) {}
  void WriteMemoryRead(uint32_t base_ptr, uint32_t length,
                       const uint8_t* host_ptr) {}
  void WriteMemoryWrite(uint32_t base_ptr, uint32_t length) {}
  void WriteRegisters(uint32_t first_register, const uint32_t* values,
                      uint32_t count, bool execute_callbacks = false) {}
  void WriteGammaRamp(const void* gamma_256, const void* gamma_pwl,
                      uint32_t rw_component = 0) {}
  void WriteEvent(EventCommand::Type type) {}
  void WriteEdramSnapshot(const void* data) {}
};

}  // namespace gpu
}  // namespace xe
