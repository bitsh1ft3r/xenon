/***************************************************************/
/* Copyright 2026 Xenon Emulator Project. All rights reserved. */
/***************************************************************/

#pragma once

#include "Core/RAM/RAM.h"
#include "Core/XCPU/Interpreter/PPCInterpreter.h"
#include "Core/HLE/Input/XinputBackend/XInputBackend.h"

#ifdef _WIN32
#include <Windows.h>
#include <xinput.h>
#endif // _WIN32

namespace Xe::Core::HLE {
  template <typename T, std::endian E>
  struct endian_store {
    endian_store() = default;
    endian_store(const T &src) { set(src); }
    endian_store(const endian_store &other) { set(other); }
    operator T() const { return get(); }

    void set(const T &src) {
      if constexpr (std::endian::native == E) {
        value = src;
      }
      else {
        value = byteswap_be(src);
      }
    }
    void set(const endian_store &other) { value = other.value; }
    T get() const {
      if constexpr (std::endian::native == E) {
        return value;
      }
      return byteswap_be(value);
    }

    endian_store<T, E> &operator+=(int a) {
      *this = *this + a;
      return *this;
    }
    endian_store<T, E> &operator-=(int a) {
      *this = *this - a;
      return *this;
    }
    endian_store<T, E> &operator++() {
      *this += 1;
      return *this;
    }  // ++a
    endian_store<T, E> operator++(int) {
      *this += 1;
      return (*this - 1);
    }  // a++
    endian_store<T, E> &operator--() {
      *this -= 1;
      return *this;
    }  // --a
    endian_store<T, E> operator--(int) {
      *this -= 1;
      return (*this + 1);
    }  // a--

    T value;
  };

  template <typename T>
  using be = endian_store<T, std::endian::big>;
  template <typename T>
  using le = endian_store<T, std::endian::little>;

  struct X_INPUT_GAMEPAD {
    be<uint16_t> buttons;
    uint8_t left_trigger;
    uint8_t right_trigger;
    be<int16_t> thumb_lx;
    be<int16_t> thumb_ly;
    be<int16_t> thumb_rx;
    be<int16_t> thumb_ry;
  };

  struct X_INPUT_STATE {
    be<uint32_t> packet_number;
    X_INPUT_GAMEPAD gamepad;
  };

  struct X_INPUT_VIBRATION {
    be<uint16_t> left_motor_speed;
    be<uint16_t> right_motor_speed;
  };

  struct X_INPUT_CAPABILITIES {
    uint8_t type;
    uint8_t sub_type;
    be<uint16_t> flags;
    X_INPUT_GAMEPAD gamepad;
    X_INPUT_VIBRATION vibration;
  };

  // https://msdn.microsoft.com/en-us/library/windows/desktop/microsoft.directx_sdk.reference.xinput_keystroke(v=vs.85).aspx
  struct X_INPUT_KEYSTROKE {
    be<uint16_t> virtual_key;
    be<uint16_t> unicode;
    be<uint16_t> flags;
    uint8_t user_index;
    uint8_t hid_code;
  };

  XInputBackend::XInputBackend(RAM *inRamPtr) : ramPtr(inRamPtr) {}

  XInputBackend::~XInputBackend() {
    // Unload library if loaded and clean up function pointers
    if (moduleHandle) {
      FreeLibrary((HMODULE)moduleHandle);
    }
    moduleHandle = nullptr;
    XInputGetCapabilitiesPtr = nullptr;
    XInputGetStatePtr = nullptr;
    XInputGetKeystrokePtr = nullptr;
    XInputSetStatePtr = nullptr;
    XInputEnablePtr = nullptr;
  }

  bool XInputBackend::Setup() {
#ifdef _WIN32
    
    // Try to load XInput library
    HMODULE moduleHndl = LoadLibraryW(L"xinput1_4.dll");
    if (!moduleHndl) {
      moduleHndl = LoadLibraryW(L"xinput1_3.dll");
    }
    if (!moduleHndl) {
      return false;
    }

    // Function pointers for XInput related routines
    // Required:
    auto xinputGetCapabilities = GetProcAddress(moduleHndl, "XInputGetCapabilities");
    auto xinputGetState = GetProcAddress(moduleHndl, "XInputGetState");
    auto xinputGetKeystroke = GetProcAddress(moduleHndl, "XInputGetKeystroke");
    auto xinputSetState = GetProcAddress(moduleHndl, "XInputSetState");

    // Not required
    auto xinputEnable = GetProcAddress(moduleHndl, "XInputEnable");

    // Check for the needed modules
    if (!xinputGetCapabilities || !xinputGetState || !xinputGetKeystroke || !xinputSetState) {
      // Free library handle
      FreeLibrary(moduleHndl);
      // Return false as we got either one or more missing functions that are required
      return false;
    }

    // Set pointers
    moduleHandle = moduleHndl;
    XInputGetCapabilitiesPtr = xinputGetCapabilities;
    XInputGetStatePtr = xinputGetState;
    XInputGetKeystrokePtr = xinputGetKeystroke;
    XInputSetStatePtr = xinputSetState;
    XInputEnablePtr = xinputEnable;

    // Library was present and all the required modules are here, great!

    SetName("XInput");
    SetPresent(true);

    LOG_INFO(HLE, "[Input]: HLE XInput backend initialized successfully.");

#else 
    // Not supported outside windows :/
    SetPresent(false);
#endif // _WIN32

  }

  void XInputBackend::GetState(sPPEState *ppeState) {
    u64 userIndex = curThread.GPR[3];
    u64 flags = curThread.GPR[4];
    u64 statePtr = curThread.GPR[5];

    XINPUT_STATE native_state;
    auto xigs = (decltype(&XInputGetState))XInputGetStatePtr;
    DWORD result = xigs(userIndex, &native_state);
    if (result) {
      return;
    }

    X_INPUT_STATE *out_state = nullptr;
    u64 address = statePtr;
    PPCInterpreter::MMUTranslateAddress(&address, ppeState, false);
    out_state = reinterpret_cast<X_INPUT_STATE *>(ramPtr->GetPointerToAddress(address));

    out_state->packet_number = native_state.dwPacketNumber;
    out_state->gamepad.buttons = native_state.Gamepad.wButtons;
    out_state->gamepad.left_trigger = native_state.Gamepad.bLeftTrigger;
    out_state->gamepad.right_trigger = native_state.Gamepad.bRightTrigger;
    out_state->gamepad.thumb_lx = native_state.Gamepad.sThumbLX;
    out_state->gamepad.thumb_ly = native_state.Gamepad.sThumbLY;
    out_state->gamepad.thumb_rx = native_state.Gamepad.sThumbRX;
    out_state->gamepad.thumb_ry = native_state.Gamepad.sThumbRY;
    curThread.GPR[3] = 0;
    return;
  }

  void XInputBackend::SetState(sPPEState *ppeState) {
    u64 userIndex = curThread.GPR[3];
    u64 flags = curThread.GPR[4];
    u64 statePtr = curThread.GPR[5];

    X_INPUT_VIBRATION *vibration = nullptr;
    u64 address = statePtr;
    PPCInterpreter::MMUTranslateAddress(&address, ppeState, false);
    vibration = reinterpret_cast<X_INPUT_VIBRATION *>(ramPtr->GetPointerToAddress(address));

    XINPUT_VIBRATION native_vibration;
    native_vibration.wLeftMotorSpeed = vibration->left_motor_speed;
    native_vibration.wRightMotorSpeed = vibration->right_motor_speed;
    auto xiss = (decltype(&XInputSetState))XInputSetStatePtr;
    DWORD result = xiss(userIndex, &native_vibration);

    curThread.GPR[3] = 0;
    return;
  }

  void XInputBackend::GetCapabilities(sPPEState *ppeState) {
    u64 userIndex = curThread.GPR[3];
    u64 flags = curThread.GPR[4];
    u64 statePtr = curThread.GPR[5];

    XINPUT_CAPABILITIES native_caps;
    auto xigc = (decltype(&XInputGetCapabilities))XInputGetCapabilitiesPtr;
    DWORD result = xigc(userIndex, flags, &native_caps);
    if (result) {
      return;
    }

    X_INPUT_CAPABILITIES *out_caps = nullptr;
    u64 address = statePtr;
    PPCInterpreter::MMUTranslateAddress(&address, ppeState, false);
    out_caps = reinterpret_cast<X_INPUT_CAPABILITIES *>(ramPtr->GetPointerToAddress(address));

    out_caps->type = native_caps.Type;
    out_caps->sub_type = native_caps.SubType;
    out_caps->flags = native_caps.Flags;
    out_caps->gamepad.buttons = native_caps.Gamepad.wButtons;
    out_caps->gamepad.left_trigger = native_caps.Gamepad.bLeftTrigger;
    out_caps->gamepad.right_trigger = native_caps.Gamepad.bRightTrigger;
    out_caps->gamepad.thumb_lx = native_caps.Gamepad.sThumbLX;
    out_caps->gamepad.thumb_ly = native_caps.Gamepad.sThumbLY;
    out_caps->gamepad.thumb_rx = native_caps.Gamepad.sThumbRX;
    out_caps->gamepad.thumb_ry = native_caps.Gamepad.sThumbRY;
    out_caps->vibration.left_motor_speed = native_caps.Vibration.wLeftMotorSpeed;
    out_caps->vibration.right_motor_speed = native_caps.Vibration.wRightMotorSpeed;

    curThread.GPR[3] = 0;
    return;
  }

  void XInputBackend::GetKeystroke(sPPEState *ppeState) {

    u64 userIndexPtr = curThread.GPR[3];
    u64 flags = curThread.GPR[4];
    u64 statePtr = curThread.GPR[5];

    
    // XInputGetKeystroke on Windows has a bug where it will return
    // ERROR_SUCCESS (0) even if the device is not connected:
    // https://stackoverflow.com/questions/23669238/xinputgetkeystroke-returning-error-success-while-controller-is-unplugged
    //
    // So we first check if the device is connected via XInputGetCapabilities, so
    // we are not passing back an uninitialized X_INPUT_KEYSTROKE structure.
    // If any user (0xFF) is polled this bug does not occur but GetCapabilities
    // would fail so we need to skip it.

    DWORD result;
    PPCInterpreter::MMUTranslateAddress(&userIndexPtr, ppeState, false);
    u32 userIndex = 0;
    memcpy(&userIndex, ramPtr->GetPointerToAddress(userIndexPtr), 4);
    userIndex = byteswap_be(userIndex);
    if (userIndex != 0xFF) {
      XINPUT_CAPABILITIES caps;
      auto xigc = (decltype(&XInputGetCapabilities))XInputGetCapabilitiesPtr;
      result = xigc(userIndex, 0, &caps);
      if (result) {
        curThread.GPR[3] = result;
        return;
      }
    }

    X_INPUT_KEYSTROKE *out_keystroke = nullptr;
    u64 address = statePtr;
    PPCInterpreter::MMUTranslateAddress(&address, ppeState, false);
    out_keystroke = reinterpret_cast<X_INPUT_KEYSTROKE *>(ramPtr->GetPointerToAddress(address));

    XINPUT_KEYSTROKE native_keystroke;
    auto xigk = (decltype(&XInputGetKeystroke))XInputGetKeystrokePtr;
    result = xigk(userIndex, 0, &native_keystroke);
    if (result) {
      curThread.GPR[3] = result;
      return;
    }

    out_keystroke->virtual_key = native_keystroke.VirtualKey;
    out_keystroke->unicode = native_keystroke.Unicode;
    out_keystroke->flags = native_keystroke.Flags;
    out_keystroke->user_index = native_keystroke.UserIndex;
    out_keystroke->hid_code = native_keystroke.HidCode;

    curThread.GPR[3] = 0;
    return;
  }

}
