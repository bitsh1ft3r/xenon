/***************************************************************/
/* Copyright 2025 Xenon Emulator Project. All rights reserved. */
/***************************************************************/

#pragma once

#include "Base/Types.h"

namespace Xe {
namespace PCIDev {

// XMA Constants
constexpr u32 XMA_CONTEXT_COUNT = 320;
constexpr u32 XMA_BYTES_PER_PACKET = 2048;
constexpr u32 XMA_BITS_PER_PACKET = XMA_BYTES_PER_PACKET * 8;
constexpr u32 XMA_BITS_PER_HEADER = 32;
constexpr u32 XMA_BYTES_PER_SAMPLE = 2;
constexpr u32 XMA_SAMPLES_PER_FRAME = 512;
constexpr u32 XMA_SAMPLES_PER_SUBFRAME = 128;
constexpr u32 XMA_BYTES_PER_FRAME_CHANNEL = XMA_SAMPLES_PER_FRAME * XMA_BYTES_PER_SAMPLE;
constexpr u32 XMA_BYTES_PER_SUBFRAME_CHANNEL = XMA_SAMPLES_PER_SUBFRAME * XMA_BYTES_PER_SAMPLE;
constexpr u32 XMA_MAX_FRAME_SIZE_BYTES = 1 + (0x7FFF / 8); // 15-bit length + padding

// XMA Context Data Structure (64 bytes, stored in guest memory)
struct XMA_CONTEXT_DATA {
  // DWORD 0
  u32 inputBuffer0PacketCount : 12;   // Number of 2KB packets in buffer 0 (max 4095)
  u32 loopCount : 8;                  // Number of loops (255 = infinite)
  u32 inputBuffer0Valid : 1;          // Buffer 0 contains valid data
  u32 inputBuffer1Valid : 1;          // Buffer 1 contains valid data
  u32 outputBufferBlockCount : 5;     // Output buffer size in 256-byte blocks
  u32 outputBufferWriteOffset : 5;    // Current write position (256-byte blocks)

  // DWORD 1
  u32 inputBuffer1PacketCount : 12;   // Number of 2KB packets in buffer 1
  u32 loopSubframeStart : 2;          // Loop start subframe
  u32 loopSubframeEnd : 3;            // Loop end subframe
  u32 loopSubframeSkip : 3;           // Subframes to skip at loop point
  u32 subframeDecodeCount : 4;        // Subframes decoded
  u32 subframeSkipCount : 3;          // Subframes to skip
  u32 sampleRate : 2;                 // Sample rate ID (0-3)
  u32 isStereo : 1;                   // 1 = stereo, 0 = mono
  u32 unkDword1C : 1;                 // Unknown
  u32 outputBufferValid : 1;          // Output buffer is valid

  // DWORD 2
  u32 inputBufferReadOffset : 26;     // Current read position in bits
  u32 unkDword2 : 6;                  // Error status

  // DWORD 3
  u32 loopStart : 26;                 // Loop start offset in bits
  u32 unkDword3 : 6;                  // Parser error status

  // DWORD 4
  u32 loopEnd : 26;                   // Loop end offset in bits
  u32 packetMetadata : 5;             // Packet metadata
  u32 currentBuffer : 1;              // Which input buffer is active (0 or 1)

  // DWORD 5
  u32 inputBuffer0Ptr;                // Physical address of input buffer 0

  // DWORD 6
  u32 inputBuffer1Ptr;                // Physical address of input buffer 1

  // DWORD 7
  u32 outputBufferPtr;                // Physical address of output buffer

  // DWORD 8
  u32 workBufferPtr;                  // Physical address of overlap-add buffer

  // DWORD 9
  u32 outputBufferReadOffset : 5;     // Current read position
  u32 reserved1 : 25;                 // Reserved
  u32 stopWhenDone : 1;               // Stop processing when buffer exhausted
  u32 interruptWhenDone : 1;          // Raise interrupt when done

  // DWORD 10-15
  u32 unkDwords10_15[6];              // Reserved

  // Load from guest memory
  void Load(const u8* ptr) {
    const u32* src = reinterpret_cast<const u32*>(ptr);
    u32* dst = reinterpret_cast<u32*>(this);
    for (size_t i = 0; i < sizeof(XMA_CONTEXT_DATA) / sizeof(u32); i++) {
      dst[i] = byteswap_be<u32>(src[i]);
    }
  }

  // Store to guest memory
  void Store(u8* ptr) const {
    const u32* src = reinterpret_cast<const u32*>(this);
    u32* dst = reinterpret_cast<u32*>(ptr);
    for (size_t i = 0; i < sizeof(XMA_CONTEXT_DATA) / sizeof(u32); i++) {
      dst[i] = byteswap_be<u32>(src[i]);
    }
  }

  // Get current input buffer size in bits
  u32 GetCurrentInputBufferSizeBits() const {
    return (currentBuffer ? inputBuffer1PacketCount : inputBuffer0PacketCount) * XMA_BITS_PER_PACKET;
  }

  // Get current input buffer address
  u32 GetCurrentInputBufferAddress() const {
    return currentBuffer ? inputBuffer1Ptr : inputBuffer0Ptr;
  }

  // Check if current input buffer is valid
  bool IsCurrentInputBufferValid() const {
    return currentBuffer ? inputBuffer1Valid : inputBuffer0Valid;
  }

  // Get output buffer capacity in bytes
  u32 GetOutputBufferCapacity() const {
    return outputBufferBlockCount * XMA_BYTES_PER_SUBFRAME_CHANNEL;
  }
};
static_assert(sizeof(XMA_CONTEXT_DATA) == 64, "XMA_CONTEXT_DATA must be 64 bytes");

// XMA Register indices (divided by 4 for register file indexing)
namespace XmaRegister {
  constexpr u32 ContextArrayAddress = 0x00 / 4;
  constexpr u32 CurrentContextIndex = 0x18 / 4;
  constexpr u32 NextContextIndex = 0x1C / 4;
  
  // Context Kick registers (each controls 32 contexts via bitmask)
  constexpr u32 Context0Kick = 0x140 / 4;
  constexpr u32 Context1Kick = 0x144 / 4;
  constexpr u32 Context2Kick = 0x148 / 4;
  constexpr u32 Context3Kick = 0x14C / 4;
  constexpr u32 Context4Kick = 0x150 / 4;
  constexpr u32 Context5Kick = 0x154 / 4;
  constexpr u32 Context6Kick = 0x158 / 4;
  constexpr u32 Context7Kick = 0x15C / 4;
  constexpr u32 Context8Kick = 0x160 / 4;
  constexpr u32 Context9Kick = 0x164 / 4;

  // Context Lock registers
  constexpr u32 Context0Lock = 0x240 / 4;
  constexpr u32 Context1Lock = 0x244 / 4;
  constexpr u32 Context2Lock = 0x248 / 4;
  constexpr u32 Context3Lock = 0x24C / 4;
  constexpr u32 Context4Lock = 0x250 / 4;
  constexpr u32 Context5Lock = 0x254 / 4;
  constexpr u32 Context6Lock = 0x258 / 4;
  constexpr u32 Context7Lock = 0x25C / 4;
  constexpr u32 Context8Lock = 0x260 / 4;
  constexpr u32 Context9Lock = 0x264 / 4;

  // Context Clear registers
  constexpr u32 Context0Clear = 0x280 / 4;
  constexpr u32 Context1Clear = 0x284 / 4;
  constexpr u32 Context2Clear = 0x288 / 4;
  constexpr u32 Context3Clear = 0x28C / 4;
  constexpr u32 Context4Clear = 0x290 / 4;
  constexpr u32 Context5Clear = 0x294 / 4;
  constexpr u32 Context6Clear = 0x298 / 4;
  constexpr u32 Context7Clear = 0x29C / 4;
  constexpr u32 Context8Clear = 0x2A0 / 4;
  constexpr u32 Context9Clear = 0x2A4 / 4;
}

// XMA Helper functions for packet parsing
namespace XmaHelpers {

// Get number of frames that BEGIN in this packet (6 bits)
inline u32 GetPacketFrameCount(const u8* packet) {
  return static_cast<u32>(packet[0] >> 2);
}

// Get the first frame offset in bits from packet start (15 bits, includes 32-bit header)
inline u32 GetPacketFrameOffset(const u8* packet) {
  u32 val = static_cast<u32>(((packet[0] & 0x3) << 13) | 
                              (packet[1] << 5) | 
                              (packet[2] >> 3));
  return val + XMA_BITS_PER_HEADER;
}

// Get packet metadata (3 bits)
inline u32 GetPacketMetadata(const u8* packet) {
  return static_cast<u32>(packet[2] & 0x7);
}

// Get skip count for split frames (8 bits)
inline u32 GetPacketSkipCount(const u8* packet) {
  return static_cast<u32>(packet[3]);
}

// Get sample rate from ID
inline u32 GetSampleRate(u32 id) {
  switch (id) {
  case 0: return 24000;
  case 1: return 32000;
  case 2: return 44100;
  case 3: return 48000;
  default: return 48000;
  }
}

// Read bits from a byte stream (big-endian bit order)
inline u32 ReadBits(const u8* data, size_t bitOffset, size_t bitCount) {
  u32 result = 0;
  for (size_t i = 0; i < bitCount; i++) {
    size_t byteIdx = (bitOffset + i) / 8;
    size_t bitIdx = 7 - ((bitOffset + i) % 8); // Big-endian bit order
    if (data[byteIdx] & (1 << bitIdx)) {
      result |= (1 << (bitCount - 1 - i));
    }
  }
  return result;
}

// Get frame length from frame header (15 bits at bit offset 1)
inline u32 GetFrameLength(const u8* frameData, size_t bitOffset) {
  return ReadBits(frameData, bitOffset + 1, 15);
}

// Check if frame is a skip frame (bit 0 = 1)
inline bool IsSkipFrame(const u8* frameData, size_t bitOffset) {
  return (ReadBits(frameData, bitOffset, 1) == 1);
}

} // namespace XmaHelpers

} // namespace PCIDev
} // namespace Xe
