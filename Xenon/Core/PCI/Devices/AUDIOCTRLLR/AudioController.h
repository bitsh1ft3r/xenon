/***************************************************************/
/* Copyright 2025 Xenon Emulator Project. All rights reserved. */
/***************************************************************/

#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

#include "Core/RAM/RAM.h"
#include "Core/PCI/PCIDevice.h"
#include "Core/PCI/Bridge/PCIBridge.h"

#ifdef GFX_ENABLED
#include <SDL3/SDL.h>
#include <SDL3/SDL_audio.h>
#endif

#define AUDIO_CTRLR_DEV_SIZE 0x40

namespace Xe {
namespace PCIDev {

//
// Xenon Audio DMA Controller (PCI DevID 0x580C)
//
// The audio controller is a dual-channel (I2S + SPDIF) free-running circular
// Bus-Mastering DMA engine in the Xenon Southbridge. Each channel has its own
// set of 4 registers and independently cycles through 32 PRD entries.
//
// Register map (BAR0, 64 bytes @ 0xEA001600):
//
//   I2S Channel (offsets 0x00 - 0x0C):
//     +0x00  PRD_BASE   Physical address of the I2S PRD table
//     +0x04  STATUS     [4:0] HW descriptor index, [12:8] SW last-valid, [31:16] bytes remaining
//     +0x08  CONTROL    Bit 0: idle, Bit 3: frame complete, Bit 4: underrun,
//                       Bit 24: kick, Bit 25: reset, Bits 28-26: enable
//     +0x0C  CONFIG     Bit 0: mode B, Bit 2: HW flag, Bits 7-6: format,
//                       Bit 10: mode A, Bit 12: stream active
//
//   SPDIF Channel (offsets 0x10 - 0x1C):
//     +0x10  PRD_BASE   Physical address of the SPDIF PRD table
//     +0x14  STATUS     Same layout as I2S status
//     +0x18  CONTROL    Same layout as I2S control
//     +0x1C  STATUS_DATA  SPDIF channel status (SCMS copy protection)
//
// PRD entry (8 bytes, big-endian in guest memory):
//   DWORD 0: Physical address of the audio buffer
//   DWORD 1: bit 31 = IOC (interrupt on completion), bits [15:0] = byte count
//

static constexpr u32 AUDIO_DMA_NUM_DESCRIPTORS = 0x20;

// PRD table entry as seen in guest memory (big-endian)
struct AudioPRDEntry {
  u32 bufferAddress; // Physical address (stored BE in guest RAM)
  u32 controlSize;   // bit31=IOC | bits[15:0]=size (stored BE in guest RAM)
};

// Register offsets from BAR0
enum eAudioReg : u32 {
  // I2S channel
  AUDIO_I2S_PRD_BASE  = 0x00,
  AUDIO_I2S_STATUS    = 0x04,
  AUDIO_I2S_CONTROL   = 0x08,
  AUDIO_I2S_CONFIG    = 0x0C,
  // SPDIF channel
  AUDIO_SPDIF_PRD_BASE   = 0x10,
  AUDIO_SPDIF_STATUS     = 0x14,
  AUDIO_SPDIF_CONTROL    = 0x18,
  AUDIO_SPDIF_STATUS_DATA = 0x1C,
};

// Control register bits (shared by both channels)
enum eAudioControlBits : u32 {
  AUDIO_CTRL_IDLE          = 0x00000001,  // Bit 0: DMA engine idle
  AUDIO_CTRL_FRAME_DONE    = 0x00000008,  // Bit 3: Frame complete (set by HW after descriptor processed)
  AUDIO_CTRL_UNDERRUN      = 0x00000010,  // Bit 4: Underrun detected
  AUDIO_CTRL_KICK          = 0x01000000,  // Bit 24: Kick/re-arm DMA
  AUDIO_CTRL_RESET         = 0x02000000,  // Bit 25: Reset DMA engine
  AUDIO_CTRL_ENABLE        = 0x1C000000,  // Bits 28-26: Enable flags
};

// Config/AUX register bits (I2S only, offset 0x0C)
enum eAudioConfigBits : u32 {
  AUDIO_CFG_MODE_B      = 0x00000001,  // Bit 0: Audio mode B (stereo select)
  AUDIO_CFG_FLAG_INV    = 0x00000004,  // Bit 2: Hardware flag (inverted)
  AUDIO_CFG_FORMAT_MASK = 0x000000C0,  // Bits 7-6: Sample format
  AUDIO_CFG_MODE_A      = 0x00000400,  // Bit 10: Audio mode A (rate select)
  AUDIO_CFG_STREAM_EN   = 0x00001000,  // Bit 12: I2S stream active
};

// Per-channel DMA state
struct AudioDMAChannel {
  // Raw register values
  u32 prdBase = 0;     // PRD table physical address
  u32 control = 0;     // Control register
  u32 config = 0;      // Config register (or SPDIF status data)

  // Status register components
  u32 hwDescriptor = 0;       // [4:0] Current HW read position
  u32 swLastValidDescr = 0;   // [12:8] SW last valid descriptor
  u32 bytesRemaining = 0;     // [31:16] Bytes remaining in current descriptor

  // Derived DMA engine state
  bool dmaEnabled = false;
  bool dmaKicked = false;

  // Compose the STATUS register value
  u32 getStatus() const {
    return ((bytesRemaining & 0xFFFF) << 16) |
           ((swLastValidDescr & 0x1F) << 8) |
           (hwDescriptor & 0x1F);
  }
};

class AUDIOCTRLR : public PCIDevice {
public:
  AUDIOCTRLR(const std::string &deviceName, u64 size, PCIBridge *parentPCIBridge, RAM *ram);
  ~AUDIOCTRLR();

  void Read(u64 readAddress, u8 *data, u64 size) override;
  void Write(u64 writeAddress, const u8 *data, u64 size) override;
  void MemSet(u64 writeAddress, s32 data, u64 size) override;
  void ConfigRead(u64 readAddress, u8* data, u64 size) override;
  void ConfigWrite(u64 writeAddress, const u8* data, u64 size) override;

  void SetAudioOutputEnabled(bool enabled);

private:
  // Dual-channel state (I2S = 0, SPDIF = 1)
  AudioDMAChannel channels[2] = {};
  std::mutex channelMutex;

  std::atomic_bool audioOutEnabled{ false };

  PCIBridge *parentBus = nullptr;
  RAM *ramPtr = nullptr;

  // SDL3 Audio output backend
#ifdef GFX_ENABLED
  SDL_AudioStream *sdlAudioStream = nullptr;
  bool sdlAudioInitialized = false;
  void initSDLAudio();
  void shutdownSDLAudio();
  void feedSDLAudio(const u8 *pcmData, u32 size);
#endif

  // Shared DMA thread (processes both channels)
  std::thread dmaThread;
  std::atomic_bool dmaThreadRunning{ false };
  std::condition_variable dmaCV;
  void dmaWorkerLoop();

  // Process a single DMA step for one channel
  // Returns true if a descriptor was processed (audio data moved)
  bool processChannelStep(int chIdx);

  // Read a single PRD entry from guest memory (handles byte-swap)
  AudioPRDEntry readPRDEntry(u32 prdBase, u32 index);

  // Register write handlers
  void handleControlWrite(int chIdx, u32 value);
  void handleStatusWrite(int chIdx, u32 value);

  // Fire audio interrupt to CPU
  void fireInterrupt();
};

} // namespace PCIDev
} // namespace Xe
