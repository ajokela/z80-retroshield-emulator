/*
 * RetroShield Z80 Emulator
 * Uses superzazu/z80 library for Z80 emulation
 * Emulates MC6850 ACIA at ports $80/$81
 *
 * Copyright (c) 2025 Alex Jokela
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <fcntl.h>
#include <sys/select.h>

#include "z80.h"

#define ROM_SIZE 0x2000   /* 8KB ROM */
#define RAM_START 0x2000
#define RAM_SIZE 0x6000   /* 24KB RAM */
#define MEM_SIZE 0x8000   /* 32KB total */

/* MC6850 ACIA ports */
#define ACIA_CTRL 0x80
#define ACIA_DATA 0x81

/* ACIA status bits */
#define ACIA_RDRF 0x01  /* Receive Data Register Full */
#define ACIA_TDRE 0x02  /* Transmit Data Register Empty */

static uint8_t memory[MEM_SIZE];
static z80 cpu;
static bool debug_mode = false;
static int max_cycles = 0;

/* Check if input available on stdin (non-blocking) */
static int kbhit(void) {
    struct timeval tv = {0, 0};
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    return select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv) > 0;
}

/* Memory read callback */
static uint8_t mem_read(void *userdata, uint16_t addr) {
    (void)userdata;
    if (addr < MEM_SIZE) {
        return memory[addr];
    }
    return 0xFF;
}

/* Memory write callback */
static void mem_write(void *userdata, uint16_t addr, uint8_t val) {
    (void)userdata;
    if (addr >= RAM_START && addr < MEM_SIZE) {
        memory[addr] = val;
    }
}

/* I/O port read callback */
static uint8_t port_in(z80 *z, uint8_t port) {
    (void)z;

    if (port == ACIA_CTRL) {
        /* Status register */
        uint8_t status = ACIA_TDRE;  /* Always ready to transmit */
        if (kbhit()) {
            status |= ACIA_RDRF;  /* Data available */
        }
        return status;
    }
    else if (port == ACIA_DATA) {
        /* Data register - read from stdin */
        if (kbhit()) {
            int c = getchar();
            if (c == EOF) {
                return 0;
            }
            return (uint8_t)c;
        }
        return 0;
    }

    return 0xFF;
}

/* I/O port write callback */
static void port_out(z80 *z, uint8_t port, uint8_t val) {
    (void)z;

    if (port == ACIA_DATA) {
        /* Write to stdout */
        putchar(val);
        fflush(stdout);
    }
    /* Control register writes ignored for now */
}

/* Load binary ROM file */
static int load_rom(const char *filename) {
    FILE *f = fopen(filename, "rb");
    if (!f) {
        perror("Failed to open ROM file");
        return -1;
    }

    size_t bytes = fread(memory, 1, ROM_SIZE, f);
    fclose(f);

    if (bytes == 0) {
        fprintf(stderr, "Failed to read ROM file\n");
        return -1;
    }

    if (debug_mode) {
        fprintf(stderr, "Loaded %zu bytes from %s\n", bytes, filename);
    }

    return 0;
}

/* Set terminal to raw mode */
static struct termios orig_termios;
static bool termios_saved = false;

static void restore_terminal(void) {
    if (termios_saved) {
        tcsetattr(STDIN_FILENO, TCSANOW, &orig_termios);
    }
}

static void set_raw_mode(void) {
    if (tcgetattr(STDIN_FILENO, &orig_termios) == 0) {
        termios_saved = true;
        atexit(restore_terminal);

        struct termios raw = orig_termios;
        raw.c_lflag &= ~(ICANON | ECHO);
        raw.c_cc[VMIN] = 0;
        raw.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    }
}

int main(int argc, char *argv[]) {
    const char *rom_file = NULL;

    /* Parse arguments */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--debug") == 0) {
            debug_mode = true;
        }
        else if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            max_cycles = atoi(argv[++i]);
        }
        else if (argv[i][0] != '-') {
            rom_file = argv[i];
        }
    }

    if (!rom_file) {
        fprintf(stderr, "Usage: %s [-d] [-c cycles] <rom.bin>\n", argv[0]);
        fprintf(stderr, "  -d         Debug mode\n");
        fprintf(stderr, "  -c cycles  Max cycles to run (0 = unlimited)\n");
        return 1;
    }

    /* Initialize memory */
    memset(memory, 0, sizeof(memory));

    /* Load ROM */
    if (load_rom(rom_file) < 0) {
        return 1;
    }

    /* Initialize CPU */
    z80_init(&cpu);
    cpu.read_byte = mem_read;
    cpu.write_byte = mem_write;
    cpu.port_in = port_in;
    cpu.port_out = port_out;

    /* Set terminal to raw mode for character-by-character input */
    set_raw_mode();

    if (debug_mode) {
        fprintf(stderr, "Starting Z80 emulation...\n");
    }

    /* Main emulation loop */
    unsigned long total_cycles = 0;
    while (1) {
        z80_step(&cpu);
        total_cycles = cpu.cyc;

        if (max_cycles > 0 && total_cycles >= (unsigned long)max_cycles) {
            break;
        }

        /* Check for HALT instruction */
        if (cpu.halted) {
            if (debug_mode) {
                fprintf(stderr, "\nCPU halted at PC=%04X after %lu cycles\n",
                        cpu.pc, total_cycles);
            }
            break;
        }
    }

    return 0;
}
