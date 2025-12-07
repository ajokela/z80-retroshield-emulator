/*
 * RetroShield Z80 Emulator
 * Uses superzazu/z80 library for Z80 emulation
 * Emulates MC6850 ACIA at ports $80/$81 and Intel 8251 at ports $00/$01
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

#define MEM_SIZE 0x10000  /* Full 64KB address space */

/* ROM size - configurable per ROM type */
static uint16_t rom_size = 0x2000;  /* Default 8KB ROM */

/* MC6850 ACIA ports (Pascal firmware, etc.) */
#define ACIA_CTRL 0x80
#define ACIA_DATA 0x81
#define ACIA_RDRF 0x01  /* Receive Data Register Full */
#define ACIA_TDRE 0x02  /* Transmit Data Register Empty */
#define ACIA_IRQ_EN 0x80  /* Bit 7: Receive Interrupt Enable */

static uint8_t acia_control = 0;  /* Track ACIA control register */
static bool uses_8251 = false;     /* Track if ROM uses 8251 (for interrupt support) */

/* Intel 8251 USART ports (Grant's BASIC, EFEX, etc.) */
#define USART_DATA 0x00
#define USART_CTRL 0x01
#define STAT_8251_TxRDY 0x01
#define STAT_8251_RxRDY 0x02
#define STAT_8251_TxE   0x04
#define STAT_DSR        0x80
#define USART_STATUS_INIT (STAT_8251_TxRDY | STAT_8251_TxE | STAT_DSR)

static uint8_t memory[MEM_SIZE];
static z80 cpu;
static bool debug_mode = false;
static int max_cycles = 0;
static bool stdin_eof = false;  /* Track if we've hit EOF on stdin */

/* Check if input available on stdin (non-blocking) */
static int kbhit(void) {
    if (stdin_eof) return 0;  /* No more input after EOF */
    struct timeval tv = {0, 0};
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    return select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv) > 0;
}

/* Memory read callback */
static uint8_t mem_read(void *userdata, uint16_t addr) {
    (void)userdata;
    return memory[addr];
}

/* Memory write callback */
static void mem_write(void *userdata, uint16_t addr, uint8_t val) {
    (void)userdata;
    /* Protect ROM area */
    if (addr >= rom_size) {
        memory[addr] = val;
    }
}

/* I/O port read callback */
static uint8_t port_in(z80 *z, uint8_t port) {
    (void)z;

    /* MC6850 ACIA (ports $80/$81) */
    if (port == ACIA_CTRL) {
        uint8_t status = ACIA_TDRE;  /* Always ready to transmit */
        if (kbhit()) {
            status |= ACIA_RDRF;  /* Data available */
        }
        return status;
    }
    else if (port == ACIA_DATA) {
        if (kbhit()) {
            int c = getchar();
            if (c == EOF) {
                stdin_eof = true;
                return 0;
            }
            return (uint8_t)c;
        }
        return 0;
    }

    /* Intel 8251 USART (ports $00/$01) */
    else if (port == USART_CTRL) {
        uses_8251 = true;  /* ROM uses 8251, enable interrupt support */
        uint8_t status = USART_STATUS_INIT;  /* TxRDY + TxE + DSR */
        if (kbhit()) {
            status |= STAT_8251_RxRDY;  /* Data available */
        }
        return status;
    }
    else if (port == USART_DATA) {
        uses_8251 = true;  /* ROM uses 8251, enable interrupt support */
        if (kbhit()) {
            int c = getchar();
            if (c == EOF) {
                stdin_eof = true;
                return 0;
            }
            /* Convert lowercase to uppercase like Arduino does */
            if (c >= 'a' && c <= 'z') c = c - 'a' + 'A';
            return (uint8_t)c;
        }
        return 0;
    }

    return 0xFF;
}

/* I/O port write callback */
static void port_out(z80 *z, uint8_t port, uint8_t val) {
    (void)z;

    /* MC6850 ACIA control (port $80) */
    if (port == ACIA_CTRL) {
        acia_control = val;
    }
    /* MC6850 ACIA data (port $81) */
    else if (port == ACIA_DATA) {
        putchar(val);
        fflush(stdout);
    }
    /* Intel 8251 USART data (port $00) */
    else if (port == USART_DATA) {
        putchar(val);
        fflush(stdout);
    }
    /* Control/mode register writes ignored */
}

/* Configure ROM size based on ROM type */
static void configure_rom(const char *filename) {
    const char *basename = strrchr(filename, '/');
    if (basename) basename++; else basename = filename;

    /* MINT: small ROM (~2KB), rest is RAM */
    if (strstr(basename, "mint") != NULL) {
        rom_size = 0x0800;  /* 2KB ROM */
        if (debug_mode) fprintf(stderr, "MINT ROM: %d bytes protected\n", rom_size);
    }
    /* Default: 8KB ROM */
    else {
        rom_size = 0x2000;
        if (debug_mode) fprintf(stderr, "Default ROM: %d bytes protected\n", rom_size);
    }
}

/* Load binary ROM file */
static int load_rom(const char *filename) {
    FILE *f = fopen(filename, "rb");
    if (!f) {
        perror("Failed to open ROM file");
        return -1;
    }

    /* Read up to full 64KB - some ROMs include RAM initialization */
    size_t bytes = fread(memory, 1, MEM_SIZE, f);
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

    /* Configure ROM size based on ROM type */
    configure_rom(rom_file);

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
    bool int_pending = false;

    while (1) {
        z80_step(&cpu);
        total_cycles = cpu.cyc;

        /* Trigger interrupt when input is available (for 8251-based ROMs only) */
        if (uses_8251 && kbhit() && cpu.iff1 && !int_pending && cpu.iff_delay == 0) {
            z80_gen_int(&cpu, 0xFF);  /* RST 38H vector for IM 1 */
            int_pending = true;
        }

        /* Clear pending flag when interrupts are disabled (char was read) */
        if (!cpu.iff1) {
            int_pending = false;
        }

        if (max_cycles > 0 && total_cycles >= (unsigned long)max_cycles) {
            if (debug_mode) {
                fprintf(stderr, "Stopped at PC=%04X after %lu cycles\n", cpu.pc, total_cycles);
            }
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
