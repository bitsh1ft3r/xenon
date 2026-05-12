#***************************************************************
#* Copyright 2025 Xenon Emulator Project. All rights reserved. *
#***************************************************************

cmake_minimum_required(VERSION 3.22)

project(Xenon)

set(CMAKE_C_STANDARD 17)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED TRUE)
set(CMAKE_CXX_STANDARD_REQUIRED TRUE)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if (NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(COMPILER ${CMAKE_CXX_COMPILER_ID})
message(STATUS "Using ${COMPILER} Version ${CMAKE_CXX_COMPILER_VERSION}")

# Set compiler ID
if (COMPILER STREQUAL "MSVC")
  set(msvc ON)
elseif (COMPILER STREQUAL "GNU")
  set(GCC ON)
elseif (COMPILER STREQUAL "Clang")
  set(Clang ON)
endif()

# Optimizations
if (NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
  set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
  if (msvc)
    add_compile_options($<$<CONFIG:Release>:/Oi> $<$<CONFIG:Release>:/Ot>)
  endif()
endif()

# Silences clang-cl: warning: argument unused during compilation: '/Zc:preprocessor' [-Wunused-command-line-argument]
if (MSVC AND Clang)
  add_compile_options(-Wno-unused-command-line-argument)
endif()

# Options
option(GFX_ENABLED "Enable graphics" ON)
option(XENIA_D3D12_GPU "Enable xenia D3D12 GPU backend (Windows-only, experimental)" ON)
option(XENON_USE_SYSTEM_DEPS "Prefer system-installed packages (find_package first)" ON)
option(XENON_ALLOW_BUNDLED_DEPS "If a package isn't found, fall back to bundled subdirs" ON)
set(XENON_THIRDPARTY_DIR "${CMAKE_CURRENT_SOURCE_DIR}/Deps/ThirdParty" CACHE PATH "Bundled deps root")

# Version
set(EMULATOR_VERSION_MAJOR "0")
set(EMULATOR_VERSION_MINOR "0")
set(EMULATOR_VERSION_PATCH "1")

set_property(SOURCE Xenon/Xenon.rc APPEND PROPERTY COMPILE_DEFINITIONS
  EMULATOR_VERSION_MAJOR=${EMULATOR_VERSION_MAJOR}
  EMULATOR_VERSION_MINOR=${EMULATOR_VERSION_MINOR}
  EMULATOR_VERSION_PATCH=${EMULATOR_VERSION_PATCH}
)
add_compile_definitions(APP_VERSION="${EMULATOR_VERSION_MAJOR}.${EMULATOR_VERSION_MINOR}.${EMULATOR_VERSION_PATCH}")

# COMMIT_COUNT on Xenon (with Git)
execute_process(
  COMMAND git rev-list --count HEAD
  WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
  OUTPUT_VARIABLE COMMIT_COUNT
  OUTPUT_STRIP_TRAILING_WHITESPACE
)

# Xenon BRANCH (with Git)
execute_process(
  COMMAND git rev-parse --abbrev-ref HEAD
  WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
  OUTPUT_VARIABLE BRANCH
  OUTPUT_STRIP_TRAILING_WHITESPACE
)

if (NOT "${COMMIT_COUNT}" STREQUAL "" AND NOT "${BRANCH}" STREQUAL "")
  add_compile_definitions(COMMIT_COUNT="${COMMIT_COUNT}")
  if (NOT "${BRANCH}" STREQUAL "main")
    add_compile_definitions(BRANCH="${BRANCH}")
    message(STATUS "Building: Xenon Experimental ${EMULATOR_VERSION_MAJOR}.${EMULATOR_VERSION_MINOR}.${EMULATOR_VERSION_PATCH}-${COMMIT_COUNT} - ${BRANCH}")
  else()
    message(STATUS "Building: Xenon Experimental ${EMULATOR_VERSION_MAJOR}.${EMULATOR_VERSION_MINOR}.${EMULATOR_VERSION_PATCH}-${COMMIT_COUNT}")
  endif()
endif()

# CPU features
if (msvc)
  add_compile_options(/arch:SSE2)
elseif (NOT APPLE)
  add_compile_options(-mssse3)
endif()

# Try system first, else add_subdirectory for the specific dep
macro(xenon_dep pkg target subdir)
  set(found_system OFF)
  if (XENON_USE_SYSTEM_DEPS)
    # Try config mode first, then module mode
    find_package(${pkg} CONFIG QUIET)
    if (NOT TARGET ${target})
      find_package(${pkg} QUIET)
    endif()

    if (TARGET ${target})
      # Check for library or header-only target
      get_target_property(lib_location ${target} IMPORTED_LOCATION)
      get_target_property(lib_location_debug ${target} IMPORTED_LOCATION_DEBUG)
      get_target_property(lib_location_release ${target} IMPORTED_LOCATION_RELEASE)
      get_target_property(type ${target} TYPE)

      if (lib_location OR lib_location_debug OR lib_location_release)
        message(STATUS "Using system ${pkg} (prebuilt library)")
        set(found_system ON)
      elseif (type STREQUAL "STATIC_LIBRARY" OR type STREQUAL "MODULE_LIBRARY")
        message(STATUS "Using system ${pkg} (build target)")
        set(found_system ON)
      elseif (type STREQUAL "INTERFACE_LIBRARY")
        message(STATUS "Using system ${pkg} (header-only)")
        set(found_system ON)
      else()
        message(WARNING "System ${pkg} found but unusable - falling back to bundled")
      endif()
    endif()
  endif()

  if (NOT found_system)
    if (NOT XENON_ALLOW_BUNDLED_DEPS)
      message(FATAL_ERROR "Package ${pkg} not found and bundled deps disabled (XENON_ALLOW_BUNDLED_DEPS=OFF).")
    endif()

    if (NOT TARGET ${target})
      # Per-dep settings
      if (${pkg} STREQUAL "toml11")
        set(TOML11_INSTALL OFF CACHE BOOL "" FORCE)
      elseif (${pkg} STREQUAL "SDL3")
        set(SDL_DISKAUDIO OFF CACHE BOOL "" FORCE)
        set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
        set(SDL_PIPEWIRE OFF CACHE BOOL "" FORCE)
        set(SDL_SHARED OFF CACHE BOOL "" FORCE)
        set(SDL_STATIC ON CACHE BOOL "" FORCE)
      elseif (${pkg} STREQUAL "asmjit")
        set(ASMJIT_NO_CUSTOM_FLAGS TRUE CACHE BOOL "" FORCE)
        set(ASMJIT_STATIC TRUE CACHE BOOL "" FORCE)
      elseif (${pkg} STREQUAL "Vulkan-Headers")
        # Vulkan-Headers is header-only, need to expose the include directory
        set(VULKAN_HEADERS_INSTALL OFF CACHE BOOL "" FORCE)
        add_subdirectory(${subdir})
        # Ensure include dir is propagated
        set_target_properties(Vulkan::Headers PROPERTIES
          INTERFACE_INCLUDE_DIRECTORIES
          ${CMAKE_CURRENT_SOURCE_DIR}/Deps/ThirdParty/Vulkan-Headers/include
        )
        set(found_system ON)
      elseif (${pkg} STREQUAL "glslang")
        set(ENABLE_OPT OFF CACHE BOOL "" FORCE)
        set(BUILD_EXTERNAL OFF CACHE BOOL "" FORCE)
      endif()

      message(STATUS "Using bundled ${pkg}")
      add_subdirectory(${XENON_THIRDPARTY_DIR}/${subdir} ${subdir} EXCLUDE_FROM_ALL)
    endif()
  endif()
endmacro()

xenon_dep(fmt fmt::fmt fmt)
xenon_dep(asmjit asmjit::asmjit asmjit)
xenon_dep(toml11 toml11::toml11 toml11)

# FFmpeg for XMA audio decoding (xenia-project fork, built from source)
option(XENON_ENABLE_XMA_DECODE "Enable XMA audio decoding via bundled FFmpeg" ON)

if (XENON_ENABLE_XMA_DECODE)
  if (EXISTS "${XENON_THIRDPARTY_DIR}/FFmpeg/libavcodec/wmaprodec.c")
    message(STATUS "Using bundled xenia-project FFmpeg (building from source)")
    add_subdirectory(${XENON_THIRDPARTY_DIR}/FFmpeg FFmpeg EXCLUDE_FROM_ALL)
  else()
    message(STATUS "")
    message(STATUS "========================================")
    message(STATUS "FFmpeg sources not found - XMA audio decoding will be DISABLED")
    message(STATUS "")
    message(STATUS "To enable XMA decoding, init the FFmpeg submodule:")
    message(STATUS "  git submodule update --init Deps/ThirdParty/FFmpeg")
    message(STATUS "========================================")
    message(STATUS "")
    set(XENON_ENABLE_XMA_DECODE OFF)
  endif()
endif()

if (GFX_ENABLED)
  xenon_dep(glad glad glad)
  xenon_dep(SDL3 SDL3::SDL3 SDL3)
  xenon_dep(sirit sirit Sirit)
  xenon_dep(VulkanHeaders Vulkan::Headers Vulkan-Headers)
  xenon_dep(VulkanMemoryAllocator VulkanMemoryAllocator VulkanMemoryAllocator)
  xenon_dep(vk-bootstrap vk-bootstrap::vk-bootstrap vk-bootstrap)
  xenon_dep(glslang glslang glslang)
endif()

# Include Files
include_directories(Xenon)

file(GLOB_RECURSE Base Xenon/Base/*.cpp Xenon/Base/*.h)
file(GLOB_RECURSE Core Xenon/Core/*.cpp Xenon/Core/*.h)

if (GFX_ENABLED)
  add_compile_definitions(VK_NO_PROTOTYPES)
  set(ImGuiDir ${XENON_THIRDPARTY_DIR}/ImGui)
  set(ImGuiBackends
    ${ImGuiDir}/backends/imgui_impl_opengl3.cpp
    ${ImGuiDir}/backends/imgui_impl_opengl3.h
    ${ImGuiDir}/backends/imgui_impl_vulkan.cpp
    ${ImGuiDir}/backends/imgui_impl_vulkan.h
    ${ImGuiDir}/backends/imgui_impl_sdl3.cpp
    ${ImGuiDir}/backends/imgui_impl_sdl3.h
  )
  file(GLOB ImGui ${ImGuiDir}/*.cpp ${ImGuiDir}/*.h)
  file(GLOB_RECURSE XenonRender Xenon/Render/*.cpp Xenon/Render/*.h)
  set(Render
    ${ImGuiBackends}
    ${ImGui}
    ${XenonRender}
  )
endif()

file(GLOB_RECURSE include Xenon/include/*.cpp Xenon/include/*.h)

add_compile_definitions(MICROPROFILE_USE_CONFIG)
set(microprofile_dir ${XENON_THIRDPARTY_DIR}/microprofile)
file(GLOB microprofile ${microprofile_dir}/*.cpp ${microprofile_dir}/*.h)

add_executable(Xenon
  ${Base}
  ${Core}
  ${Render}
  ${include}
  ${microprofile}
  Xenon/Main.cpp
)

# --- Xenia D3D12 GPU backend (opt-in, Windows-only) ---
if (XENIA_D3D12_GPU AND WIN32)
  message(STATUS "Xenia D3D12 GPU backend ENABLED")
  add_compile_definitions(XENIA_D3D12_GPU)

  # Glob all xenia .cc files, then exclude standalone mains and platform-specific files
  file(GLOB_RECURSE XENIA_ALL_SOURCES Xenon/xenia/**/*.cc)
  list(FILTER XENIA_ALL_SOURCES EXCLUDE REGEX "shader_compiler_main\\.cc$")
  list(FILTER XENIA_ALL_SOURCES EXCLUDE REGEX "console_app_main_win\\.cc$")
  list(FILTER XENIA_ALL_SOURCES EXCLUDE REGEX "windowed_app_main_win\\.cc$")
  list(FILTER XENIA_ALL_SOURCES EXCLUDE REGEX "main_win\\.cc$")
  list(FILTER XENIA_ALL_SOURCES EXCLUDE REGEX "(posix|android|gnulinux|gtk|mac|_test\\.cc)")
  list(FILTER XENIA_ALL_SOURCES EXCLUDE REGEX "profiling\\.cc$")

  # Xenia stub .cpp files that bridge into Xenon (compiled with Xenon's PCH/types)
  set(XENIA_XENON_STUBS
    Xenon/xenia/cpu/processor.cpp
  )

  # DXBCChecksum.cpp (not a submodule, lives in third_party/dxbc)
  set(XENIA_DXBC_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/dxbc/DXBCChecksum.cpp
  )

  target_sources(Xenon PRIVATE ${XENIA_ALL_SOURCES} ${XENIA_DXBC_SOURCES} ${XENIA_XENON_STUBS})

  # Xenia was written for C++17. In C++20+ u8"" literals produce char8_t[]
  # which breaks std::string concatenation. Disable char8_t for xenia sources.
  # Also suppress xbyak operator-name conflict and enable Unicode Win32 API
  # (xenia expects wide Win32 calls; Xenon's own code uses explicit A suffixes).
  if (msvc)
    set_source_files_properties(${XENIA_ALL_SOURCES} ${XENIA_DXBC_SOURCES} PROPERTIES
      COMPILE_OPTIONS "/Zc:char8_t-"
    )
  endif()
  set_source_files_properties(${XENIA_ALL_SOURCES} ${XENIA_DXBC_SOURCES} PROPERTIES
    COMPILE_DEFINITIONS "XBYAK_NO_OP_NAMES;UNICODE;_UNICODE"
    SKIP_PRECOMPILE_HEADERS ON
  )

  # Include paths for xenia code:
  #   - Repo root so "third_party/..." and "build/..." includes resolve
  #   - Xenon/ so "xenia/..." includes resolve (already present via include_directories)
  target_include_directories(Xenon PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
  )

  # Third-party submodule include paths needed by xenia base/gpu/ui code
  target_include_directories(Xenon SYSTEM PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/disruptorplus/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/cpptoml/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/cxxopts/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/utfcpp/source
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/xxhash
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/date/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/xbyak
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/DirectXShaderCompiler/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/renderdoc
    ${XENON_THIRDPARTY_DIR}  # For <microprofile/microprofile.h> resolution
  )

  # Link D3D12 / DXGI libraries
  target_link_libraries(Xenon PRIVATE d3d12 dxgi dxguid dwmapi shlwapi)
endif()

target_precompile_headers(Xenon PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Xenon/Base/Global.h)

if (GFX_ENABLED)
  add_compile_definitions(GFX_ENABLED)
else()
  add_compile_definitions(NO_GFX)
endif()

if (NOT CMAKE_BUILD_TYPE STREQUAL "Release")
  add_compile_definitions(DEBUG_BUILD)
endif()

# Link libraries
target_link_libraries(Xenon PRIVATE fmt::fmt toml11::toml11 asmjit::asmjit)
if (GFX_ENABLED)
  target_link_libraries(Xenon PRIVATE glad glslang sirit SDL3::SDL3 VulkanMemoryAllocator vk-bootstrap::vk-bootstrap)
endif()
if (XENON_ENABLE_XMA_DECODE)
  target_compile_definitions(Xenon PRIVATE XMA_FFMPEG_ENABLED)
  # Include Deps/ThirdParty so #include "FFmpeg/libavcodec/avcodec.h" resolves
  target_include_directories(Xenon SYSTEM PRIVATE ${XENON_THIRDPARTY_DIR})
  target_link_libraries(Xenon PRIVATE libavcodec libavutil)
  message(STATUS "XMA audio decoding ENABLED")
endif()

# Includes
target_include_directories(Xenon PRIVATE
  ${microprofile_dir}
  Xenon/include
  Xenon
)
if (GFX_ENABLED)
  target_include_directories(Xenon SYSTEM PRIVATE
    ${ImGuiDir}
    ${VolkDir}
    ${VkHdrsDir}/include
    ${VMADir}/include
    ${XENON_THIRDPARTY_DIR}/glad
    ${XENON_THIRDPARTY_DIR}/SDL3/include
    ${XENON_THIRDPARTY_DIR}/glslang/include
    ${XENON_THIRDPARTY_DIR}/Sirit/include
    ${XENON_THIRDPARTY_DIR}/vk-bootstrap/src
    ${XENON_THIRDPARTY_DIR}/Vulkan-Headers/include
    ${XENON_THIRDPARTY_DIR}/VulkanMemoryAllocator/include
  )
endif()
target_include_directories(Xenon SYSTEM PRIVATE
  ${XENON_THIRDPARTY_DIR}/fmt/include
  ${XENON_THIRDPARTY_DIR}/toml11
  ${XENON_THIRDPARTY_DIR}/asmjit/src
  ${XENON_THIRDPARTY_DIR}/plusaes/include
  ${XENON_THIRDPARTY_DIR}/llvm/include
)

# Add defines specific to Apple
if (APPLE)
  add_compile_definitions(SIMPLE_FMT)
# Add defines specific to Windows
elseif (WIN32)
  # Synchronization is needed for WaitOnAddress/WakeByAddressSingle
  target_link_libraries(Xenon PRIVATE iphlpapi setupapi ws2_32 synchronization)
  add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN)

  # Disables Warnings
  add_compile_definitions(_WINSOCK_DEPRECATED_NO_WARNINGS _CRT_SECURE_NO_WARNINGS)

  # Add .rc file, for the app icon and executable properties
  target_sources(Xenon PRIVATE Xenon/Xenon.rc)
endif()

# Specific to MSVC, forces system version so the autodetected 8.1 SDK isn't used
# Also enables multi-processor (multithreaded) compiling on MSVC
if (CMAKE_GENERATOR MATCHES "Visual Studio")
  set(CMAKE_GENERATOR_PLATFORM x64)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /MP")
endif()
