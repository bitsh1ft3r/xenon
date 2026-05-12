/**
 * Capstone Disassembly Engine stub for Xenon/Xenia integration.
 * This is a minimal stub that satisfies the Xenia x64 backend's compile
 * requirements without linking the real Capstone library.
 *
 * cs_open always returns CS_ERR_OK; cs_disasm_iter always returns false.
 * All functions are inline to avoid requiring a separate compilation unit.
 */

#ifndef CAPSTONE_CAPSTONE_H
#define CAPSTONE_CAPSTONE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Capstone handle type
typedef uintptr_t csh;

// Error codes
typedef enum cs_err {
  CS_ERR_OK      = 0,   // No error
  CS_ERR_MEM     = 1,   // Out of memory
  CS_ERR_ARCH    = 2,   // Unsupported architecture
  CS_ERR_HANDLE  = 3,   // Invalid handle
  CS_ERR_CSH     = 4,   // Invalid csh argument
  CS_ERR_MODE    = 5,   // Invalid/unsupported mode
  CS_ERR_OPTION  = 6,   // Invalid/unsupported option
  CS_ERR_DETAIL  = 7,   // Information not available
  CS_ERR_MEMSETUP = 8,  // Dynamic memory management uninitialized
  CS_ERR_VERSION = 9,   // Unsupported version
  CS_ERR_DIET    = 10,  // Access irrelevant data in "diet" engine
  CS_ERR_SKIPDATA = 11, // Access irrelevant data for "data" instruction
  CS_ERR_X86_ATT = 12,  // X86 AT&T syntax not supported
  CS_ERR_X86_INTEL = 13, // X86 Intel syntax not supported
  CS_ERR_X86_MASM = 14, // X86 MASM syntax not supported
} cs_err;

// Architectures
typedef enum cs_arch {
  CS_ARCH_ARM   = 0,
  CS_ARCH_ARM64 = 1,
  CS_ARCH_MIPS  = 2,
  CS_ARCH_X86   = 3,
  CS_ARCH_PPC   = 4,
  CS_ARCH_SPARC = 5,
  CS_ARCH_SYSZ  = 6,
  CS_ARCH_XCORE = 7,
  CS_ARCH_MAX,
  CS_ARCH_ALL   = 0xFFFF,
} cs_arch;

// Modes
typedef enum cs_mode {
  CS_MODE_LITTLE_ENDIAN = 0,
  CS_MODE_ARM           = 0,
  CS_MODE_16            = 1 << 1,
  CS_MODE_32            = 1 << 2,
  CS_MODE_64            = 1 << 3,
  CS_MODE_THUMB         = 1 << 4,
  CS_MODE_MCLASS        = 1 << 5,
  CS_MODE_V8            = 1 << 6,
  CS_MODE_MICRO         = 1 << 4,
  CS_MODE_MIPS3         = 1 << 5,
  CS_MODE_MIPS32R6      = 1 << 6,
  CS_MODE_MIPSGP64      = 1 << 7,
  CS_MODE_V9            = 1 << 4,
  CS_MODE_BIG_ENDIAN    = 1 << 31,
  CS_MODE_MIPS32        = CS_MODE_32,
  CS_MODE_MIPS64        = CS_MODE_64,
} cs_mode;

// Option types
typedef enum cs_opt_type {
  CS_OPT_INVALID    = 0,
  CS_OPT_SYNTAX     = 1,   // Assembly output syntax
  CS_OPT_DETAIL     = 2,   // Break down instruction structure into details
  CS_OPT_MODE       = 3,   // Change engine's mode at runtime
  CS_OPT_MEM        = 4,   // User-defined dynamic memory related functions
  CS_OPT_SKIPDATA   = 5,   // Skip data when disassembling
  CS_OPT_SKIPDATA_SETUP = 6, // Setup user-defined function for SKIPDATA option
  CS_OPT_MNEMONIC   = 7,   // Customize instruction mnemonic
  CS_OPT_UNSIGNED   = 8,   // Print immediate operands in unsigned form
} cs_opt_type;

// Option values
typedef enum cs_opt_value {
  CS_OPT_OFF            = 0,    // Turn OFF an option
  CS_OPT_ON             = 3,    // Turn ON an option (CS_OPT_DETAIL, CS_OPT_SKIPDATA)
  CS_OPT_SYNTAX_DEFAULT = 0,    // Default assembly syntax
  CS_OPT_SYNTAX_INTEL   = 1,    // X86 Intel syntax
  CS_OPT_SYNTAX_ATT     = 2,    // X86 AT&T syntax
  CS_OPT_SYNTAX_NOREGNAME = 3,  // Print register name with only number
  CS_OPT_SYNTAX_MASM    = 4,    // X86 Intel MASM syntax
} cs_opt_value;

// Instruction structure
typedef struct cs_insn {
  unsigned int  id;             // Instruction ID
  uint64_t      address;        // Address (EIP) of this instruction
  uint16_t      size;           // Size of this instruction
  uint8_t       bytes[16];      // Machine bytes of this instruction
  char          mnemonic[32];   // ASCII text of instruction mnemonic
  char          op_str[160];    // ASCII text of instruction operands
  void*         detail;         // Pointer to cs_detail (NULL if CS_OPT_DETAIL is OFF)
} cs_insn;

// Open handle to the Capstone library.
// This stub always succeeds.
static inline cs_err cs_open(cs_arch arch, cs_mode mode, csh* handle) {
  (void)arch;
  (void)mode;
  if (handle) {
    *handle = (csh)1;  // Non-zero sentinel handle
  }
  return CS_ERR_OK;
}

// Close a handle previously opened by cs_open.
static inline cs_err cs_close(csh* handle) {
  if (handle) {
    *handle = 0;
  }
  return CS_ERR_OK;
}

// Set option for disassembly engine at runtime.
static inline cs_err cs_option(csh handle, cs_opt_type type, size_t value) {
  (void)handle;
  (void)type;
  (void)value;
  return CS_ERR_OK;
}

// Disassemble binary code, one instruction at a time.
// This stub always returns false (no instruction disassembled).
static inline bool cs_disasm_iter(csh handle,
                                  const uint8_t** code,
                                  size_t* size,
                                  uint64_t* address,
                                  cs_insn* insn) {
  (void)handle;
  (void)code;
  (void)size;
  (void)address;
  (void)insn;
  return false;
}

// Return friendly name of a group id (only available if CS_OPT_DETAIL is ON)
static inline const char* cs_group_name(csh handle, unsigned int group_id) {
  (void)handle;
  (void)group_id;
  return "";
}

// Return friendly name of an instruction in a string.
static inline const char* cs_insn_name(csh handle, unsigned int insn_id) {
  (void)handle;
  (void)insn_id;
  return "";
}

// Return friendly name of a register in a string.
static inline const char* cs_reg_name(csh handle, unsigned int reg_id) {
  (void)handle;
  (void)reg_id;
  return "";
}

// Return last error number when some API function fail.
static inline cs_err cs_errno(csh handle) {
  (void)handle;
  return CS_ERR_OK;
}

// Return a string describing given error code.
static inline const char* cs_strerror(cs_err code) {
  (void)code;
  return "No error";
}

// Free memory allocated in previous disassemble using cs_disasm().
static inline void cs_free(cs_insn* insn, size_t count) {
  (void)insn;
  (void)count;
}

// Allocate memory for 1 instruction to be used with cs_disasm_iter().
static inline cs_insn* cs_malloc(csh handle) {
  (void)handle;
  return (cs_insn*)0;  // Stub: returns null
}

#ifdef __cplusplus
}
#endif

#endif  // CAPSTONE_CAPSTONE_H
