/*
 * Z80 Disassembler - Header
 * Simple disassembler for the TUI debugger
 *
 * Copyright (c) 2025 Alex Jokela
 * SPDX-License-Identifier: MIT
 */

#ifndef Z80_DISASM_H
#define Z80_DISASM_H

#include <stdint.h>

/* Disassemble one instruction
 * Returns: number of bytes consumed
 * Output: mnemonic string written to 'buf' (max 32 chars)
 */
int z80_disasm(uint8_t *mem, uint16_t addr, char *buf, int bufsize);

#endif /* Z80_DISASM_H */
