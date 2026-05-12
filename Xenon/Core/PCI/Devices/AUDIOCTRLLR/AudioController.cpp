/***************************************************************/
/* Copyright 2025 Xenon Emulator Project. All rights reserved. */
/***************************************************************/

#include "Base/Thread.h"
#include "AudioController.h"
#include "Base/Logging/Log.h"
#include "Base/EndianReader.h"

// Uncomment for Audio Controller debug logging
//#define AUDIOCTRLR_DEBUG

#ifndef AUDIOCTRLR_DEBUG
#define DEBUGP(x, ...)
#else
#define DEBUGP(x, ...) LOG_DEBUG(AudioController, x, ##__VA_ARGS__);
#endif


// Channel indices
static constexpr int CH_I2S   = 0;
static constexpr int CH_SPDIF = 1;

Xe::PCIDev::AUDIOCTRLR::AUDIOCTRLR(const std::string &deviceName, u64 size, PCIBridge *parentPCIBridge, RAM *ram) :
  PCIDevice(deviceName, size), parentBus(parentPCIBridge), ramPtr(ram) {
  // Set PCI Properties (Vendor 0x1414, Device 0x580C)
  pciConfigSpace.configSpaceHeader.reg0.hexData = 0x580C1414;
  pciConfigSpace.configSpaceHeader.reg1.hexData = 0x02880006;
  pciConfigSpace.configSpaceHeader.reg2.hexData = 0x04010001;
  pciConfigSpace.configSpaceHeader.regB.hexData = 0x75011039;
  pciConfigSpace.configSpaceHeader.regF.hexData = 0x0B340100;
  // Set our PCI Dev Sizes
  pciDevSizes[0] = 0x40; // BAR0

#ifdef GFX_ENABLED
  initSDLAudio();
#endif

  dmaThreadRunning = false;
  dmaThread = std::thread(&AUDIOCTRLR::dmaWorkerLoop, this);
}

Xe::PCIDev::AUDIOCTRLR::~AUDIOCTRLR() {
  dmaThreadRunning = false;
  dmaCV.notify_all();
  if (dmaThread.joinable()) {
    dmaThread.join();
  }

#ifdef GFX_ENABLED
  shutdownSDLAudio();
#endif
}

// --- SDL3 Audio Backend ---

#ifdef GFX_ENABLED
void Xe::PCIDev::AUDIOCTRLR::initSDLAudio() {
  if (!SDL_Init(SDL_INIT_AUDIO)) {
    LOG_ERROR(Xenon, "Failed to initialize SDL Audio subsystem: {}", SDL_GetError());
  }

  SDL_AudioSpec spec = {};
  spec.format = SDL_AUDIO_S16LE;
  spec.channels = 2;
  spec.freq = 48000;

  sdlAudioStream = SDL_OpenAudioDeviceStream(
    SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
    &spec,
    nullptr,
    nullptr
  );

  if (!sdlAudioStream) {
    LOG_ERROR(AudioController, "Failed to open SDL audio device: {}", SDL_GetError());
    sdlAudioInitialized = false;
    return;
  }

  SDL_ResumeAudioStreamDevice(sdlAudioStream);
  sdlAudioInitialized = true;
  LOG_INFO(AudioController, "SDL3 audio backend initialized (48kHz, S16LE, Stereo)");
}

void Xe::PCIDev::AUDIOCTRLR::shutdownSDLAudio() {
  if (sdlAudioStream) {
    SDL_DestroyAudioStream(sdlAudioStream);
    sdlAudioStream = nullptr;
  }
  sdlAudioInitialized = false;
}

void Xe::PCIDev::AUDIOCTRLR::feedSDLAudio(const u8 *pcmData, u32 size) {
  if (!sdlAudioInitialized || !sdlAudioStream || !size) {
    return;
  }

  if (!SDL_PutAudioStreamData(sdlAudioStream, pcmData, static_cast<int>(size))) {
    LOG_ERROR(AudioController, "SDL_PutAudioStreamData failed: {}", SDL_GetError());
  }
}
#endif

// --- PRD Entry Reading ---

Xe::PCIDev::AudioPRDEntry Xe::PCIDev::AUDIOCTRLR::readPRDEntry(u32 prdBase, u32 index) {
  AudioPRDEntry entry = {};
  if (!ramPtr || !prdBase) {
    return entry;
  }

  u32 entryAddr = prdBase + (index * sizeof(AudioPRDEntry));
  u8 *entryPtr = ramPtr->GetPointerToAddress(entryAddr);
  if (!entryPtr) {
    return entry;
  }

  u32 raw0, raw1;
  memcpy(&raw0, entryPtr, sizeof(u32));
  memcpy(&raw1, entryPtr + sizeof(u32), sizeof(u32));

  entry.bufferAddress = raw0;
  entry.controlSize = raw1;

  return entry;
}

// --- Interrupt Delivery ---

void Xe::PCIDev::AUDIOCTRLR::fireInterrupt() {
  parentBus->RouteInterrupt(PRIO_AUDIO);
}

// --- DMA Channel Processing ---

bool Xe::PCIDev::AUDIOCTRLR::processChannelStep(int chIdx) {
  u32 currentDescr;
  u32 prdBase;
  bool kicked;
  bool enabled;

  {
    std::lock_guard<std::mutex> lock(channelMutex);
    AudioDMAChannel &ch = channels[chIdx];
    kicked = ch.dmaKicked;
    enabled = ch.dmaEnabled;
    currentDescr = ch.hwDescriptor;
    prdBase = ch.prdBase;
  }

  // If not running, set idle and return
  if (!kicked || !enabled || prdBase == 0) {
    std::lock_guard<std::mutex> lock(channelMutex);
    channels[chIdx].control |= AUDIO_CTRL_IDLE;
    return false;
  }

  // Read PRD entry for current descriptor
  AudioPRDEntry prd = readPRDEntry(prdBase, currentDescr);
  u32 bufAddr = prd.bufferAddress;
  u32 chunkSize = prd.controlSize & 0x0000FFFF; // Lower 16 bits = byte count
  bool iocSet = (prd.controlSize & 0x80000000) != 0;

  {
    std::lock_guard<std::mutex> lock(channelMutex);
    channels[chIdx].bytesRemaining = chunkSize;
  }

  // Feed audio data to SDL (I2S channel only — primary audio output)
  if (chunkSize > 0 && ramPtr && chIdx == CH_I2S) {
    u8 *pcmData = ramPtr->GetPointerToAddress(bufAddr);
    if (pcmData) {
#ifdef GFX_ENABLED
      feedSDLAudio(pcmData, chunkSize);
#endif
    }
  }

  // Advance to next descriptor
  u32 nextDescr = (currentDescr + 1) % AUDIO_DMA_NUM_DESCRIPTORS;

  {
    std::lock_guard<std::mutex> lock(channelMutex);
    AudioDMAChannel &ch = channels[chIdx];
    ch.hwDescriptor = nextDescr;
    ch.bytesRemaining = 0;

    // Set frame-complete bit (bit 3) — the kernel ISR checks this
    ch.control |= AUDIO_CTRL_FRAME_DONE;

    // Clear the kick bit so the kernel can see a fresh status.
    // The kernel re-reads control to check for frame completion.
    // Kick bit (bit 24) stays set as long as DMA is running —
    // we only set frame-done alongside it.
  }
  if (bufAddr != 0x01ed2200) {
    // Fire interrupt so the kernel ISR processes the completed frame
    fireInterrupt();
  }


  return true;
}

// --- DMA Worker Thread ---

void Xe::PCIDev::AUDIOCTRLR::dmaWorkerLoop() {
  Base::SetCurrentThreadName("[Xe] Audio DMA");

  while (dmaThreadRunning) {
    // Wait until at least one channel is kicked
    {
      std::unique_lock<std::mutex> lock(channelMutex);
      dmaCV.wait(lock, [this] {
        return !dmaThreadRunning ||
               (channels[CH_I2S].dmaKicked && channels[CH_I2S].dmaEnabled &&
                channels[CH_I2S].prdBase != 0) ||
               (channels[CH_SPDIF].dmaKicked && channels[CH_SPDIF].dmaEnabled &&
                channels[CH_SPDIF].prdBase != 0);
      });
    }

    if (!dmaThreadRunning) {
      break;
    }

    // Run the DMA engines
    while (dmaThreadRunning) {
      bool i2sActive;
      bool spdifActive;
      {
        std::lock_guard<std::mutex> lock(channelMutex);
        i2sActive = channels[CH_I2S].dmaKicked && channels[CH_I2S].dmaEnabled &&
                    channels[CH_I2S].prdBase != 0;
        spdifActive = channels[CH_SPDIF].dmaKicked && channels[CH_SPDIF].dmaEnabled &&
                      channels[CH_SPDIF].prdBase != 0;
      }

      if (!i2sActive && !spdifActive) {
        // Both channels stopped — go back to waiting
        break;
      }

      bool didWork = false;

      if (i2sActive) {
        didWork |= processChannelStep(CH_I2S);
      }

      if (spdifActive) {
        processChannelStep(CH_SPDIF);
      }

      // Pace to real-time playback.
      // Kernel uses 0x400 (1024) byte chunks = 256 frames @ 48kHz stereo S16
      // = ~5.33ms per descriptor.
      if (didWork) {
        std::this_thread::sleep_for(std::chrono::microseconds(5300));
      } else {
        // No work done — sleep briefly before rechecking
        std::this_thread::sleep_for(std::chrono::microseconds(1000));
      }
    }
  }
}

// --- PCI Register Access ---

void Xe::PCIDev::AUDIOCTRLR::Read(u64 readAddress, u8 *data, u64 size) {
  u32 regOffset = static_cast<u32>(readAddress) & 0x3F;
  u32 dataOut = 0;

  std::lock_guard<std::mutex> lock(channelMutex);

  switch (regOffset) {
  // --- I2S Channel ---
  case AUDIO_I2S_PRD_BASE:
    dataOut = channels[CH_I2S].prdBase;
    break;
  case AUDIO_I2S_STATUS:
    dataOut = channels[CH_I2S].getStatus();
    break;
  case AUDIO_I2S_CONTROL:
    dataOut = channels[CH_I2S].control;
    break;
  case AUDIO_I2S_CONFIG:
    dataOut = channels[CH_I2S].config;
    break;

  // --- SPDIF Channel ---
  case AUDIO_SPDIF_PRD_BASE:
    dataOut = channels[CH_SPDIF].prdBase;
    break;
  case AUDIO_SPDIF_STATUS:
    dataOut = channels[CH_SPDIF].getStatus();
    break;
  case AUDIO_SPDIF_CONTROL:
    dataOut = channels[CH_SPDIF].control;
    break;
  case AUDIO_SPDIF_STATUS_DATA:
    dataOut = channels[CH_SPDIF].config;
    break;

  default:
    DEBUGP("Unhandled read at offset {:#x}", regOffset);
    memset(data, 0xFF, size);
    return;
  }

  memcpy(data, &dataOut, size);
  DEBUGP("Read reg {:#x} = {:#x}", regOffset, dataOut);
}

void Xe::PCIDev::AUDIOCTRLR::Write(u64 writeAddress, const u8 *data, u64 size) {
  u32 regOffset = static_cast<u32>(writeAddress) & 0x3F;

  u64 dataIn = 0;
  memcpy(&dataIn, data, size);

  DEBUGP("Write reg {:#x} = {:#x}, size {}", regOffset, dataIn, size);

  switch (regOffset) {
  // --- I2S Channel ---
  case AUDIO_I2S_PRD_BASE: {
    std::lock_guard<std::mutex> lock(channelMutex);
    channels[CH_I2S].prdBase = static_cast<u32>(dataIn) & 0x1FFFFFFF;
    DEBUGP("I2S PRD base set to {:#x}", channels[CH_I2S].prdBase);
  } break;
  case AUDIO_I2S_STATUS:
    handleStatusWrite(CH_I2S, static_cast<u32>(dataIn));
    break;
  case AUDIO_I2S_CONTROL:
    handleControlWrite(CH_I2S, static_cast<u32>(dataIn));
    break;
  case AUDIO_I2S_CONFIG: {
    std::lock_guard<std::mutex> lock(channelMutex);
    channels[CH_I2S].config = static_cast<u32>(dataIn);
    DEBUGP("I2S config set to {:#x}", channels[CH_I2S].config);
  } break;

  // --- SPDIF Channel ---
  case AUDIO_SPDIF_PRD_BASE: {
    std::lock_guard<std::mutex> lock(channelMutex);
    channels[CH_SPDIF].prdBase = static_cast<u32>(dataIn) & 0x1FFFFFFF;
    DEBUGP("SPDIF PRD base set to {:#x}", channels[CH_SPDIF].prdBase);
  } break;
  case AUDIO_SPDIF_STATUS:
    handleStatusWrite(CH_SPDIF, static_cast<u32>(dataIn));
    break;
  case AUDIO_SPDIF_CONTROL:
    handleControlWrite(CH_SPDIF, static_cast<u32>(dataIn));
    break;
  case AUDIO_SPDIF_STATUS_DATA: {
    std::lock_guard<std::mutex> lock(channelMutex);
    channels[CH_SPDIF].config = static_cast<u32>(dataIn);
    DEBUGP("SPDIF status data set to {:#x}", channels[CH_SPDIF].config);
  } break;

  default:
    DEBUGP("Unhandled write at offset {:#x}, data {:#x}", regOffset, dataIn);
    break;
  }
}

void Xe::PCIDev::AUDIOCTRLR::handleControlWrite(int chIdx, u32 value) {
  const char *chName = (chIdx == CH_I2S) ? "I2S" : "SPDIF";
  std::lock_guard<std::mutex> lock(channelMutex);
  AudioDMAChannel &ch = channels[chIdx];

  // Writing to control register acknowledges (clears) the frame-complete
  // and underrun interrupt status bits. The kernel ISR reads the control
  // register, saves the value, then writes it back to acknowledge.
  // We clear bits 3 and 4 on any write to CONTROL, matching HW behavior.
  // The bits we just set in processChannelStep get read by the ISR before
  // this write-back clears them.

  // Reset (bit 25)
  if (value & AUDIO_CTRL_RESET) {
    DEBUGP("{} DMA reset (control={:#x})", chName, value);
    ch.dmaKicked = false;
    ch.dmaEnabled = false;
    ch.hwDescriptor = 0;
    ch.swLastValidDescr = 0;
    ch.bytesRemaining = 0;
    ch.control = (value & ~AUDIO_CTRL_RESET) | AUDIO_CTRL_IDLE;
    return;
  }

  // Store the written value, but clear the HW-set status bits (3, 4) since
  // the kernel is acknowledging them by writing the register back
  ch.control = value & ~(AUDIO_CTRL_FRAME_DONE | AUDIO_CTRL_UNDERRUN);

  // Enable (bits 28-26)
  if (value & AUDIO_CTRL_ENABLE) {
    ch.dmaEnabled = true;
    ch.control &= ~AUDIO_CTRL_IDLE;
    DEBUGP("{} DMA enabled (control={:#x})", chName, value);
  }

  // Kick (bit 24)
  if (value & AUDIO_CTRL_KICK) {
    if (ch.dmaEnabled) {
      ch.dmaKicked = true;
      ch.control &= ~AUDIO_CTRL_IDLE;
      dmaCV.notify_all();
      DEBUGP("{} DMA kicked (control={:#x})", chName, value);
    }
  } else {
    // Kick bit cleared — CloseI2SStream / CloseSPDIFStream
    if (ch.dmaKicked) {
      ch.dmaKicked = false;
      DEBUGP("{} DMA kick cleared (control={:#x})", chName, value);
    }
  }
}

void Xe::PCIDev::AUDIOCTRLR::handleStatusWrite(int chIdx, u32 value) {
  const char *chName = (chIdx == CH_I2S) ? "I2S" : "SPDIF";
  std::lock_guard<std::mutex> lock(channelMutex);
  AudioDMAChannel &ch = channels[chIdx];

  // SW writes the last-valid descriptor index into bits [12:8]
  u32 newLastValid = (value >> 8) & 0x1F;
  DEBUGP("{} SW last-valid descriptor: {} -> {}",
            chName, ch.swLastValidDescr, newLastValid);
  ch.swLastValidDescr = newLastValid;
}

void Xe::PCIDev::AUDIOCTRLR::SetAudioOutputEnabled(bool enabled) {
  audioOutEnabled.store(enabled);
  LOG_INFO(AudioController, "Audio output {}", enabled ? "enabled" : "disabled");
}

void Xe::PCIDev::AUDIOCTRLR::MemSet(u64 writeAddress, s32 data, u64 size) {}

void Xe::PCIDev::AUDIOCTRLR::ConfigRead(u64 readAddress, u8 *data, u64 size) {
  memcpy(data, &pciConfigSpace.data[static_cast<u8>(readAddress)], size);
}

void Xe::PCIDev::AUDIOCTRLR::ConfigWrite(u64 writeAddress, const u8 *data, u64 size) {
  u64 tmp = 0;
  memcpy(&tmp, data, size);
  if (static_cast<u8>(writeAddress) >= 0x10 && static_cast<u8>(writeAddress) < 0x34) {
    const u32 regOffset = (static_cast<u8>(writeAddress) - 0x10) >> 2;
    if (pciDevSizes[regOffset] != 0) {
      if (tmp == 0xFFFFFFFF) {
        u64 x = 2;
        for (int idx = 2; idx < 31; idx++) {
          tmp &= ~x;
          x <<= 1;
          if (x >= pciDevSizes[regOffset]) {
            break;
          }
        }
        tmp &= ~0x3;
      }
    }
    if (static_cast<u8>(writeAddress) == 0x30) {
      tmp = 0;
    }
  }

  memcpy(&pciConfigSpace.data[static_cast<u8>(writeAddress)], &tmp, size);
}
