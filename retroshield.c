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
#include "version.h"
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>

#define MEM_SIZE 0x10000  /* Full 64KB address space */

/* SD Card Emulation ports */
#define SD_CMD_PORT     0x10
#define SD_STATUS_PORT  0x11
#define SD_DATA_PORT    0x12
#define SD_FNAME_PORT   0x13
#define SD_SEEK_LO      0x14  /* Seek position low byte */
#define SD_SEEK_HI      0x15  /* Seek position high byte */

/* SD Commands (match kz80_db) */
#define SD_CMD_OPEN_READ   0x01
#define SD_CMD_CREATE      0x02  /* Create new file (truncate) */
#define SD_CMD_OPEN_APPEND 0x03  /* Open for append */
#define SD_CMD_SEEK_START  0x04  /* Seek to start of file */
#define SD_CMD_CLOSE       0x05
#define SD_CMD_DIR         0x06
#define SD_CMD_OPEN_RW     0x07  /* Open for read/write (no truncate) */
#define SD_CMD_SEEK_BYTE   0x08  /* Seek to byte position (set via SD_DATA_PORT first) */
#define SD_CMD_SEEK_16     0x09  /* Seek to 16-bit position (low byte first via SEEK_PORT) */

/* SD Status bits (match kz80_db) */
#define SD_STATUS_READY 0x01
#define SD_STATUS_ERROR 0x02
#define SD_STATUS_DATA  0x80  /* Data available to read */

/* SD Card emulation state */
static char sd_filename[256];
static int sd_filename_pos = 0;
static FILE *sd_file = NULL;
static uint8_t sd_status = SD_STATUS_READY;
static DIR *sd_dir = NULL;
static const char *sd_storage_dir = "storage";  /* Subdirectory for virtual SD files */
static char sd_dir_entry[64];
static int sd_dir_entry_pos = 0;
static uint16_t sd_seek_pos = 0;  /* Position for byte seek (16-bit) */

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
static bool dump_memory = false;
static uint16_t dump_addr = 0;
static uint16_t dump_len = 256;

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

    /* SD Card emulation ports */
    else if (port == SD_STATUS_PORT) {
        uint8_t status = sd_status;
        /* Add DATA flag if we have data to read */
        if (sd_file || sd_dir) {
            status |= SD_STATUS_DATA;
        }
        return status;
    }
    else if (port == SD_DATA_PORT) {
        if (sd_file) {
            int c = fgetc(sd_file);
            if (c == EOF) {
                fclose(sd_file);
                sd_file = NULL;
                sd_status = SD_STATUS_READY;  /* No more data */
                return 0;
            }
            return (uint8_t)c;
        } else if (sd_dir) {
            /* Return directory entry character by character */
            if (sd_dir_entry[sd_dir_entry_pos] == '\0') {
                /* Need next directory entry */
                struct dirent *de;
                while ((de = readdir(sd_dir)) != NULL) {
                    /* Skip . and .. */
                    if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
                        continue;
                    /* Copy filename with newline */
                    snprintf(sd_dir_entry, sizeof(sd_dir_entry), "%s\r\n", de->d_name);
                    sd_dir_entry_pos = 0;
                    break;
                }
                if (de == NULL) {
                    /* End of directory */
                    closedir(sd_dir);
                    sd_dir = NULL;
                    sd_status = SD_STATUS_READY;  /* No more data */
                    return 0;
                }
            }
            /* Return next character of directory entry */
            return (uint8_t)sd_dir_entry[sd_dir_entry_pos++];
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

    /* SD Card emulation ports */
    else if (port == SD_CMD_PORT) {
        switch (val) {
            case SD_CMD_OPEN_READ: {
                /* Build full path */
                char fullpath[512];
                snprintf(fullpath, sizeof(fullpath), "%s/%s", sd_storage_dir, sd_filename);

                /* Close any existing file */
                if (sd_file) {
                    fclose(sd_file);
                    sd_file = NULL;
                }

                /* Open for reading */
                sd_file = fopen(fullpath, "rb");

                if (sd_file) {
                    sd_status = SD_STATUS_READY;
                    if (debug_mode) {
                        fprintf(stderr, "[SD] Opened for read: %s\n", fullpath);
                    }
                } else {
                    sd_status = SD_STATUS_ERROR | SD_STATUS_READY;
                    if (debug_mode) {
                        fprintf(stderr, "[SD] Failed to open: %s (%s)\n", fullpath, strerror(errno));
                    }
                }
                break;
            }
            case SD_CMD_CREATE: {
                /* Create new file (truncate if exists) */
                char fullpath[512];
                snprintf(fullpath, sizeof(fullpath), "%s/%s", sd_storage_dir, sd_filename);

                if (sd_file) {
                    fclose(sd_file);
                    sd_file = NULL;
                }

                /* Create storage directory if needed */
                mkdir(sd_storage_dir, 0755);

                sd_file = fopen(fullpath, "w+b");

                if (sd_file) {
                    sd_status = SD_STATUS_READY;
                    if (debug_mode) {
                        fprintf(stderr, "[SD] Created: %s\n", fullpath);
                    }
                } else {
                    sd_status = SD_STATUS_ERROR | SD_STATUS_READY;
                    if (debug_mode) {
                        fprintf(stderr, "[SD] Failed to create: %s (%s)\n", fullpath, strerror(errno));
                    }
                }
                break;
            }
            case SD_CMD_OPEN_APPEND: {
                /* Open for append (read/write, seek to end) */
                char fullpath[512];
                snprintf(fullpath, sizeof(fullpath), "%s/%s", sd_storage_dir, sd_filename);

                if (sd_file) {
                    fclose(sd_file);
                    sd_file = NULL;
                }

                sd_file = fopen(fullpath, "r+b");
                if (sd_file) {
                    fseek(sd_file, 0, SEEK_END);
                    sd_status = SD_STATUS_READY;
                    if (debug_mode) {
                        fprintf(stderr, "[SD] Opened for append: %s\n", fullpath);
                    }
                } else {
                    sd_status = SD_STATUS_ERROR | SD_STATUS_READY;
                    if (debug_mode) {
                        fprintf(stderr, "[SD] Failed to open for append: %s (%s)\n", fullpath, strerror(errno));
                    }
                }
                break;
            }
            case SD_CMD_SEEK_START:
                if (sd_file) {
                    fseek(sd_file, 0, SEEK_SET);
                    sd_status = SD_STATUS_READY;
                    if (debug_mode) {
                        fprintf(stderr, "[SD] Seeked to start\n");
                    }
                } else {
                    sd_status = SD_STATUS_ERROR | SD_STATUS_READY;
                }
                break;
            case SD_CMD_OPEN_RW: {
                /* Open for read/write without truncating */
                char fullpath[512];
                snprintf(fullpath, sizeof(fullpath), "%s/%s", sd_storage_dir, sd_filename);

                if (sd_file) {
                    fclose(sd_file);
                    sd_file = NULL;
                }

                sd_file = fopen(fullpath, "r+b");
                if (sd_file) {
                    sd_status = SD_STATUS_READY;
                    if (debug_mode) {
                        fprintf(stderr, "[SD] Opened for read/write: %s\n", fullpath);
                    }
                } else {
                    sd_status = SD_STATUS_ERROR | SD_STATUS_READY;
                    if (debug_mode) {
                        fprintf(stderr, "[SD] Failed to open for read/write: %s (%s)\n", fullpath, strerror(errno));
                    }
                }
                break;
            }
            case SD_CMD_CLOSE:
                if (sd_file) {
                    fclose(sd_file);
                    sd_file = NULL;
                    if (debug_mode) {
                        fprintf(stderr, "[SD] Closed file\n");
                    }
                }
                if (sd_dir) {
                    closedir(sd_dir);
                    sd_dir = NULL;
                }
                sd_status = SD_STATUS_READY;
                break;

            case SD_CMD_DIR:
                /* Close any existing directory listing */
                if (sd_dir) {
                    closedir(sd_dir);
                }
                /* Create storage directory if it doesn't exist */
                mkdir(sd_storage_dir, 0755);

                sd_dir = opendir(sd_storage_dir);
                sd_dir_entry_pos = 0;
                sd_dir_entry[0] = '\0';

                if (sd_dir) {
                    sd_status = SD_STATUS_READY;
                    if (debug_mode) {
                        fprintf(stderr, "[SD] DIR: %s\n", sd_storage_dir);
                    }
                } else {
                    sd_status = SD_STATUS_ERROR | SD_STATUS_READY;
                }
                break;

            case SD_CMD_SEEK_BYTE:
            case SD_CMD_SEEK_16:
                if (sd_file) {
                    fseek(sd_file, sd_seek_pos, SEEK_SET);
                    sd_status = SD_STATUS_READY;
                    if (debug_mode) {
                        fprintf(stderr, "[SD] Seeked to position %d\n", sd_seek_pos);
                    }
                } else {
                    sd_status = SD_STATUS_ERROR | SD_STATUS_READY;
                }
                break;
        }
    }
    else if (port == SD_DATA_PORT) {
        if (sd_file) {
            fputc(val, sd_file);
        }
    }
    else if (port == SD_FNAME_PORT) {
        if (val == 0) {
            /* Null terminator - filename complete */
            sd_filename[sd_filename_pos] = '\0';
            sd_filename_pos = 0;
            if (debug_mode) {
                fprintf(stderr, "[SD] Filename set: %s\n", sd_filename);
            }
        } else if (sd_filename_pos < (int)sizeof(sd_filename) - 1) {
            sd_filename[sd_filename_pos++] = (char)val;
        }
    }
    else if (port == SD_SEEK_LO) {
        sd_seek_pos = (sd_seek_pos & 0xFF00) | val;
        if (debug_mode) {
            fprintf(stderr, "[SD] Seek position low: %d (pos=%d)\n", val, sd_seek_pos);
        }
    }
    else if (port == SD_SEEK_HI) {
        sd_seek_pos = (sd_seek_pos & 0x00FF) | ((uint16_t)val << 8);
        if (debug_mode) {
            fprintf(stderr, "[SD] Seek position high: %d (pos=%d)\n", val, sd_seek_pos);
        }
    }
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
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            fprintf(stderr, "RetroShield Z80 Emulator v%s\n\n", VERSION);
            fprintf(stderr, "Usage: %s [-d] [-c cycles] [-m addr [len]] [-s dir] <rom.bin>\n", argv[0]);
            fprintf(stderr, "  -h, --help      Show this help message\n");
            fprintf(stderr, "  -d, --debug     Debug mode\n");
            fprintf(stderr, "  -c cycles       Max cycles to run (0 = unlimited)\n");
            fprintf(stderr, "  -m addr [len]   Dump memory at addr after run\n");
            fprintf(stderr, "  -s, --storage   SD card storage directory (default: storage)\n");
            return 0;
        }
        else if (strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--debug") == 0) {
            debug_mode = true;
        }
        else if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            max_cycles = atoi(argv[++i]);
        }
        else if (strcmp(argv[i], "-m") == 0 && i + 1 < argc) {
            dump_memory = true;
            dump_addr = (uint16_t)strtol(argv[++i], NULL, 0);
            if (i + 1 < argc && argv[i+1][0] != '-') {
                dump_len = (uint16_t)strtol(argv[++i], NULL, 0);
            }
        }
        else if ((strcmp(argv[i], "-s") == 0 || strcmp(argv[i], "--storage") == 0) && i + 1 < argc) {
            sd_storage_dir = argv[++i];
        }
        else if (argv[i][0] != '-') {
            rom_file = argv[i];
        }
    }

    if (!rom_file) {
        fprintf(stderr, "Usage: %s [-d] [-c cycles] [-m addr [len]] [-s dir] <rom.bin>\n", argv[0]);
        fprintf(stderr, "Try '%s --help' for more information.\n", argv[0]);
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

    /* Dump memory if requested */
    if (dump_memory) {
        fprintf(stderr, "\nMemory dump at 0x%04X:\n", dump_addr);
        for (uint16_t i = 0; i < dump_len; i += 16) {
            fprintf(stderr, "%04X: ", dump_addr + i);
            for (int j = 0; j < 16 && i + j < dump_len; j++) {
                fprintf(stderr, "%02X ", memory[dump_addr + i + j]);
            }
            fprintf(stderr, "\n");
        }
    }

    return 0;
}
