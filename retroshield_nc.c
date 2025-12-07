/*
 * RetroShield Z80 Emulator - Notcurses TUI
 * Modern TUI debugger using notcurses library
 *
 * Copyright (c) 2025 Alex Jokela
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <locale.h>
#include <time.h>
#include <unistd.h>
#include <termios.h>
#include <sys/resource.h>
#include <mach/mach.h>

#include <notcurses/notcurses.h>
#include "z80.h"
#include "z80_disasm.h"

/* Memory configuration */
#define MEM_SIZE 0x10000      /* Full 64KB address space */

/* ROM size - configurable per ROM type */
static uint16_t rom_size = 0x2000;  /* Default 8KB ROM */

/* RAM configuration - can be changed via command line or per-ROM */
/* Grant's BASIC: $2000-$37FF (6KB) */
/* EFEX monitor:  $E800-$FFFF (6KB) */
static uint16_t ram_start = 0x2000;
static uint16_t ram_end = 0x37FF;

/* MC6850 ACIA ports (used by our Pascal firmware) */
#define ACIA_CTRL 0x80
#define ACIA_DATA 0x81
#define ACIA_RDRF 0x01
#define ACIA_TDRE 0x02

/* Intel 8251 USART ports (used by Grant's BASIC) - matches Arduino exactly */
#define USART_DATA 0x00
#define USART_CTRL 0x01
#define STAT_8251_TxRDY 0x01
#define STAT_8251_RxRDY 0x02
#define STAT_8251_TxE   0x04
#define STAT_DSR        0x80
/* Initial status: TxRDY + TxE + DSR = 0x85 */
#define USART_STATUS_INIT (STAT_8251_TxRDY | STAT_8251_TxE | STAT_DSR)

/* Terminal buffer */
#define TERM_COLS 80
#define TERM_ROWS 24
#define TERM_BUF_SIZE (TERM_COLS * TERM_ROWS)

/* Input buffer for emulated system */
#define INPUT_BUF_SIZE 256

/* Global state */
static uint8_t memory[MEM_SIZE];
static z80 cpu;

/* Terminal emulation state */
static char term_buffer[TERM_BUF_SIZE];
static int term_cursor_x = 0;
static int term_cursor_y = 0;

/* Input buffer (keys to send to emulated system) */
static char input_buffer[INPUT_BUF_SIZE];
static int input_head = 0;
static int input_tail = 0;
static bool int_signaled = false;  /* Track if interrupt was signaled for current input */
static bool uses_8251 = false;     /* Track if ROM uses 8251 (for interrupt support) */

/* Emulator state */
static bool running = false;
static bool paused = true;
static int cycles_per_frame = 50000;
static unsigned long total_cycles = 0;
static uint16_t mem_view_addr = 0x0000;

/* Notcurses state */
static struct notcurses *nc = NULL;
static struct ncplane *stdp = NULL;
static struct ncplane *reg_plane = NULL;
static struct ncplane *dis_plane = NULL;
static struct ncplane *metrics_plane = NULL;
static struct ncplane *mem_plane = NULL;
static struct ncplane *term_plane = NULL;
static struct ncplane *help_plane = NULL;
static struct ncplane *status_plane = NULL;

/* Metrics tracking */
static struct timespec last_metrics_time;
static unsigned long last_cycles = 0;
static double cycles_per_sec = 0.0;
static double cpu_percent = 0.0;

/* Colors */
#define COL_BORDER    0x4488cc
#define COL_TITLE     0x88ccff
#define COL_LABEL     0x888888
#define COL_VALUE     0xffffff
#define COL_CHANGED   0xff8844
#define COL_PC        0x44ff44
#define COL_ADDR      0x888888
#define COL_OPCODE    0xcccccc
#define COL_MNEMONIC  0xffffff
#define COL_HEX       0x88aacc
#define COL_ASCII     0xaaccaa
#define COL_CURSOR    0xffff00
#define COL_STATUS_RUN  0x44ff44
#define COL_STATUS_PAUSE 0xffaa00
#define COL_STATUS_HALT  0xff4444
#define COL_HELP_KEY  0xffcc44
#define COL_HELP_DESC 0xaaaaaa

/* Previous register values for change highlighting */
static uint16_t prev_pc, prev_sp, prev_ix, prev_iy;
static uint8_t prev_a, prev_b, prev_c, prev_d, prev_e, prev_h, prev_l;
static uint8_t prev_flags;

/* Forward declarations */
static void term_putchar(char c);
static bool input_available(void);
static char input_getchar(void);

/* Memory callbacks */
static uint8_t mem_read(void *userdata, uint16_t addr) {
    (void)userdata;
    return memory[addr];
}

static void mem_write(void *userdata, uint16_t addr, uint8_t val) {
    (void)userdata;
    /* Protect ROM area */
    if (addr >= rom_size) {
        memory[addr] = val;
    }
}

static uint8_t port_in(z80 *z, uint8_t port) {
    (void)z;
    /* MC6850 ACIA (ports $80/$81) */
    if (port == ACIA_CTRL) {
        uint8_t status = ACIA_TDRE;
        if (input_available()) status |= ACIA_RDRF;
        return status;
    } else if (port == ACIA_DATA) {
        return input_available() ? input_getchar() : 0;
    }
    /* Intel 8251 USART (ports $00/$01) - Grant's BASIC */
    else if (port == USART_CTRL) {
        uses_8251 = true;  /* ROM uses 8251, enable interrupt support */
        /* Match Arduino: status = 0x85 (TxRDY + TxE + DSR), add RxRDY when input available */
        uint8_t status = USART_STATUS_INIT;
        if (input_available()) status |= STAT_8251_RxRDY;
        return status;
    } else if (port == USART_DATA) {
        uses_8251 = true;  /* ROM uses 8251, enable interrupt support */
        /* Reading data clears RxRDY and deasserts interrupt */
        if (input_available()) {
            char c = input_getchar();
            /* Arduino does toupper() on input */
            if (c >= 'a' && c <= 'z') c = c - 'a' + 'A';
            return (uint8_t)c;
        }
        return 0;
    }
    return 0xFF;
}

static void port_out(z80 *z, uint8_t port, uint8_t val) {
    (void)z;
    /* MC6850 ACIA (port $81) */
    if (port == ACIA_DATA) {
        term_putchar(val);
    }
    /* Intel 8251 USART (port $00) - data */
    else if (port == USART_DATA) {
        term_putchar(val);
    }
    /* Intel 8251 USART (port $01) - mode/command register */
    else if (port == USART_CTRL) {
        /* We mostly ignore this, but could track state if needed */
        /* Commands like $37 enable TX/RX and clear errors */
        /* Mode bytes like $4D set 8-N-1 format */
    }
}

/* Terminal emulation */
static void term_clear(void) {
    memset(term_buffer, ' ', TERM_BUF_SIZE);
    term_cursor_x = 0;
    term_cursor_y = 0;
}

static void term_scroll(void) {
    memmove(term_buffer, term_buffer + TERM_COLS, TERM_COLS * (TERM_ROWS - 1));
    memset(term_buffer + TERM_COLS * (TERM_ROWS - 1), ' ', TERM_COLS);
}

static void term_putchar(char c) {
    if (c == '\r') {
        term_cursor_x = 0;
    } else if (c == '\n') {
        term_cursor_y++;
        if (term_cursor_y >= TERM_ROWS) {
            term_scroll();
            term_cursor_y = TERM_ROWS - 1;
        }
    } else if (c == '\b') {
        if (term_cursor_x > 0) term_cursor_x--;
    } else if (c >= 32 && c < 127) {
        int idx = term_cursor_y * TERM_COLS + term_cursor_x;
        if (idx < TERM_BUF_SIZE) {
            term_buffer[idx] = c;
        }
        term_cursor_x++;
        if (term_cursor_x >= TERM_COLS) {
            term_cursor_x = 0;
            term_cursor_y++;
            if (term_cursor_y >= TERM_ROWS) {
                term_scroll();
                term_cursor_y = TERM_ROWS - 1;
            }
        }
    }
}

/* Input buffer */
static bool input_available(void) {
    return input_head != input_tail;
}

static char input_getchar(void) {
    if (input_head == input_tail) return 0;
    char c = input_buffer[input_tail];
    input_tail = (input_tail + 1) % INPUT_BUF_SIZE;
    int_signaled = false;  /* Allow new interrupt for next character */
    return c;
}

static void input_putchar(char c) {
    int next = (input_head + 1) % INPUT_BUF_SIZE;
    if (next != input_tail) {
        input_buffer[input_head] = c;
        input_head = next;
        int_signaled = false;  /* New input, allow interrupt */
    }
}

/* Configure ROM size based on ROM type */
static void configure_rom(const char *filename) {
    const char *basename = strrchr(filename, '/');
    if (basename) basename++; else basename = filename;

    /* MINT: small ROM (~2KB), rest is RAM */
    if (strstr(basename, "mint") != NULL) {
        rom_size = 0x0800;  /* 2KB ROM */
    }
    /* Default: 8KB ROM */
    else {
        rom_size = 0x2000;
    }
}

/* ROM loading - reads up to 64KB for ROMs that include RAM init */
static int load_rom(const char *filename) {
    FILE *f = fopen(filename, "rb");
    if (!f) return -1;
    size_t bytes = fread(memory, 1, MEM_SIZE, f);
    fclose(f);
    return (bytes > 0) ? 0 : -1;
}

/* Get flags as byte */
static uint8_t get_flags(void) {
    return (cpu.sf << 7) | (cpu.zf << 6) | (cpu.yf << 5) | (cpu.hf << 4) |
           (cpu.xf << 3) | (cpu.pf << 2) | (cpu.nf << 1) | cpu.cf;
}

/* Draw a box with title using Unicode box-drawing chars */
static void draw_box(struct ncplane *p, const char *title) {
    unsigned rows, cols;
    ncplane_dim_yx(p, &rows, &cols);

    ncplane_set_fg_rgb(p, COL_BORDER);

    /* Draw corners and edges */
    ncplane_putstr_yx(p, 0, 0, "╭");
    ncplane_putstr_yx(p, 0, cols - 1, "╮");
    ncplane_putstr_yx(p, rows - 1, 0, "╰");
    ncplane_putstr_yx(p, rows - 1, cols - 1, "╯");

    for (unsigned x = 1; x < cols - 1; x++) {
        ncplane_putstr_yx(p, 0, x, "─");
        ncplane_putstr_yx(p, rows - 1, x, "─");
    }
    for (unsigned y = 1; y < rows - 1; y++) {
        ncplane_putstr_yx(p, y, 0, "│");
        ncplane_putstr_yx(p, y, cols - 1, "│");
    }

    /* Title */
    if (title) {
        ncplane_set_fg_rgb(p, COL_TITLE);
        int title_x = (cols - strlen(title) - 4) / 2;
        if (title_x < 2) title_x = 2;
        ncplane_printf_yx(p, 0, title_x, "┤ %s ├", title);
    }
}

/* Draw registers panel */
static void draw_registers(void) {
    ncplane_erase(reg_plane);
    draw_box(reg_plane, "Registers");

    uint8_t flags = get_flags();

    /* Main registers */
    int y = 1;

    ncplane_set_fg_rgb(reg_plane, COL_LABEL);
    ncplane_putstr_yx(reg_plane, y, 2, "PC");
    ncplane_set_fg_rgb(reg_plane, (cpu.pc != prev_pc) ? COL_CHANGED : COL_PC);
    ncplane_printf_yx(reg_plane, y, 5, "%04X", cpu.pc);

    ncplane_set_fg_rgb(reg_plane, COL_LABEL);
    ncplane_putstr_yx(reg_plane, y, 11, "SP");
    ncplane_set_fg_rgb(reg_plane, (cpu.sp != prev_sp) ? COL_CHANGED : COL_VALUE);
    ncplane_printf_yx(reg_plane, y++, 14, "%04X", cpu.sp);

    ncplane_set_fg_rgb(reg_plane, COL_LABEL);
    ncplane_putstr_yx(reg_plane, y, 2, "AF");
    ncplane_set_fg_rgb(reg_plane, (cpu.a != prev_a || flags != prev_flags) ? COL_CHANGED : COL_VALUE);
    ncplane_printf_yx(reg_plane, y, 5, "%02X%02X", cpu.a, flags);

    ncplane_set_fg_rgb(reg_plane, COL_LABEL);
    ncplane_putstr_yx(reg_plane, y, 11, "BC");
    ncplane_set_fg_rgb(reg_plane, (cpu.b != prev_b || cpu.c != prev_c) ? COL_CHANGED : COL_VALUE);
    ncplane_printf_yx(reg_plane, y++, 14, "%02X%02X", cpu.b, cpu.c);

    ncplane_set_fg_rgb(reg_plane, COL_LABEL);
    ncplane_putstr_yx(reg_plane, y, 2, "DE");
    ncplane_set_fg_rgb(reg_plane, (cpu.d != prev_d || cpu.e != prev_e) ? COL_CHANGED : COL_VALUE);
    ncplane_printf_yx(reg_plane, y, 5, "%02X%02X", cpu.d, cpu.e);

    ncplane_set_fg_rgb(reg_plane, COL_LABEL);
    ncplane_putstr_yx(reg_plane, y, 11, "HL");
    ncplane_set_fg_rgb(reg_plane, (cpu.h != prev_h || cpu.l != prev_l) ? COL_CHANGED : COL_VALUE);
    ncplane_printf_yx(reg_plane, y++, 14, "%02X%02X", cpu.h, cpu.l);

    ncplane_set_fg_rgb(reg_plane, COL_LABEL);
    ncplane_putstr_yx(reg_plane, y, 2, "IX");
    ncplane_set_fg_rgb(reg_plane, (cpu.ix != prev_ix) ? COL_CHANGED : COL_VALUE);
    ncplane_printf_yx(reg_plane, y, 5, "%04X", cpu.ix);

    ncplane_set_fg_rgb(reg_plane, COL_LABEL);
    ncplane_putstr_yx(reg_plane, y, 11, "IY");
    ncplane_set_fg_rgb(reg_plane, (cpu.iy != prev_iy) ? COL_CHANGED : COL_VALUE);
    ncplane_printf_yx(reg_plane, y++, 14, "%04X", cpu.iy);

    /* Flags */
    ncplane_set_fg_rgb(reg_plane, COL_LABEL);
    ncplane_putstr_yx(reg_plane, y, 2, "Flags:");
    ncplane_set_fg_rgb(reg_plane, COL_VALUE);
    ncplane_printf_yx(reg_plane, y, 9, "%c%c%c%c%c%c%c%c",
        cpu.sf ? 'S' : '-', cpu.zf ? 'Z' : '-',
        cpu.yf ? 'Y' : '-', cpu.hf ? 'H' : '-',
        cpu.xf ? 'X' : '-', cpu.pf ? 'P' : '-',
        cpu.nf ? 'N' : '-', cpu.cf ? 'C' : '-');
}

/* Draw disassembly panel */
static void draw_disassembly(void) {
    ncplane_erase(dis_plane);
    draw_box(dis_plane, "Disassembly");

    unsigned rows, cols;
    ncplane_dim_yx(dis_plane, &rows, &cols);

    uint16_t addr = cpu.pc;
    char buf[64];

    for (unsigned y = 1; y < rows - 1 && addr < MEM_SIZE; y++) {
        int len = z80_disasm(memory, addr, buf, sizeof(buf));

        /* Highlight current PC */
        bool is_pc = (addr == cpu.pc);

        /* Address */
        ncplane_set_fg_rgb(dis_plane, is_pc ? COL_PC : COL_ADDR);
        if (is_pc) {
            ncplane_putstr_yx(dis_plane, y, 2, "▶");
        }
        ncplane_printf_yx(dis_plane, y, 4, "%04X", addr);

        /* Opcodes (up to 4 bytes) */
        ncplane_set_fg_rgb(dis_plane, COL_OPCODE);
        int x = 9;
        for (int i = 0; i < len && i < 4; i++) {
            ncplane_printf_yx(dis_plane, y, x, "%02X", memory[(addr + i) & 0xFFFF]);
            x += 3;
        }

        /* Mnemonic */
        ncplane_set_fg_rgb(dis_plane, is_pc ? COL_PC : COL_MNEMONIC);
        ncplane_printf_yx(dis_plane, y, 22, "%-20s", buf);

        addr += len;
    }
}

/* Draw memory panel */
static void draw_memory(void) {
    ncplane_erase(mem_plane);
    draw_box(mem_plane, "Memory");

    unsigned rows, cols;
    ncplane_dim_yx(mem_plane, &rows, &cols);

    uint16_t addr = mem_view_addr;

    for (unsigned y = 1; y < rows - 1; y++) {
        /* Address */
        ncplane_set_fg_rgb(mem_plane, COL_ADDR);
        ncplane_printf_yx(mem_plane, y, 2, "%04X:", addr);

        /* Hex bytes */
        ncplane_set_fg_rgb(mem_plane, COL_HEX);
        for (int i = 0; i < 16 && (addr + i) < 0x10000; i++) {
            uint16_t a = (addr + i) & 0xFFFF;
            if (a == cpu.pc) {
                ncplane_set_fg_rgb(mem_plane, COL_PC);
            } else if (a == cpu.sp) {
                ncplane_set_fg_rgb(mem_plane, COL_CHANGED);
            } else {
                ncplane_set_fg_rgb(mem_plane, COL_HEX);
            }
            ncplane_printf_yx(mem_plane, y, 8 + i * 3, "%02X",
                             (a < MEM_SIZE) ? memory[a] : 0xFF);
        }

        /* ASCII */
        ncplane_set_fg_rgb(mem_plane, COL_ASCII);
        ncplane_putstr_yx(mem_plane, y, 57, "│");
        for (int i = 0; i < 16 && (addr + i) < 0x10000; i++) {
            uint16_t a = (addr + i) & 0xFFFF;
            uint8_t c = (a < MEM_SIZE) ? memory[a] : 0xFF;
            char ch = (c >= 32 && c < 127) ? c : '.';
            ncplane_printf_yx(mem_plane, y, 58 + i, "%c", ch);
        }

        addr += 16;
    }
}

/* Draw terminal panel */
static void draw_terminal(void) {
    ncplane_erase(term_plane);
    draw_box(term_plane, "Terminal");

    for (int y = 0; y < TERM_ROWS; y++) {
        char line[TERM_COLS + 1];
        memcpy(line, term_buffer + y * TERM_COLS, TERM_COLS);
        line[TERM_COLS] = '\0';

        ncplane_set_fg_rgb(term_plane, COL_VALUE);
        ncplane_putstr_yx(term_plane, y + 1, 1, line);
    }

    /* Draw cursor */
    ncplane_set_fg_rgb(term_plane, COL_CURSOR);
    ncplane_putstr_yx(term_plane, term_cursor_y + 1, term_cursor_x + 1, "█");
}

/* Draw help bar */
static void draw_help(void) {
    ncplane_erase(help_plane);

    struct { const char *key; const char *desc; } help[] = {
        {"F5", "Run"},
        {"F6", "Step"},
        {"F7", "Pause"},
        {"F8", "Reset"},
        {"PgUp/Dn", "Mem"},
        {"Home", "MemPC"},
        {"F12", "Quit"},
        {NULL, NULL}
    };

    int x = 1;
    for (int i = 0; help[i].key; i++) {
        ncplane_set_fg_rgb(help_plane, COL_HELP_KEY);
        ncplane_putstr_yx(help_plane, 0, x, help[i].key);
        x += strlen(help[i].key);

        ncplane_set_fg_rgb(help_plane, COL_HELP_DESC);
        ncplane_printf_yx(help_plane, 0, x, ":%s ", help[i].desc);
        x += strlen(help[i].desc) + 2;
    }
}

/* Get host process metrics */
static void update_metrics(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    double elapsed = (now.tv_sec - last_metrics_time.tv_sec) +
                     (now.tv_nsec - last_metrics_time.tv_nsec) / 1e9;

    if (elapsed >= 0.5) { /* Update every 500ms */
        unsigned long cycle_diff = total_cycles - last_cycles;
        cycles_per_sec = cycle_diff / elapsed;

        /* Get CPU usage via rusage */
        struct rusage usage;
        if (getrusage(RUSAGE_SELF, &usage) == 0) {
            static struct timeval last_utime = {0, 0};
            static struct timeval last_stime = {0, 0};

            double user = (usage.ru_utime.tv_sec - last_utime.tv_sec) +
                         (usage.ru_utime.tv_usec - last_utime.tv_usec) / 1e6;
            double sys = (usage.ru_stime.tv_sec - last_stime.tv_sec) +
                        (usage.ru_stime.tv_usec - last_stime.tv_usec) / 1e6;

            cpu_percent = ((user + sys) / elapsed) * 100.0;

            last_utime = usage.ru_utime;
            last_stime = usage.ru_stime;
        }

        last_metrics_time = now;
        last_cycles = total_cycles;
    }
}

/* Get memory usage in KB */
static size_t get_memory_usage_kb(void) {
    struct mach_task_basic_info info;
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                  (task_info_t)&info, &count) == KERN_SUCCESS) {
        return info.resident_size / 1024;
    }
    return 0;
}

/* Calculate emulated RAM usage */
static int get_emu_ram_usage(void) {
    int used = 0;
    int ram_size = (int)ram_end - (int)ram_start + 1;
    /* Count non-zero bytes in RAM area */
    for (int i = 0; i < ram_size; i++) {
        if (memory[ram_start + i] != 0) used++;
    }
    return (used * 100) / ram_size;
}

/* Draw metrics panel */
static void draw_metrics(void) {
    ncplane_erase(metrics_plane);
    draw_box(metrics_plane, "Metrics");

    update_metrics();

    int y = 1;

    /* Z80 Emulation metrics */
    ncplane_set_fg_rgb(metrics_plane, COL_TITLE);
    ncplane_putstr_yx(metrics_plane, y++, 2, "── Z80 ──");

    ncplane_set_fg_rgb(metrics_plane, COL_LABEL);
    ncplane_putstr_yx(metrics_plane, y, 2, "Speed:");
    ncplane_set_fg_rgb(metrics_plane, COL_VALUE);
    if (cycles_per_sec >= 1e6) {
        ncplane_printf_yx(metrics_plane, y++, 9, "%.2f MHz", cycles_per_sec / 1e6);
    } else if (cycles_per_sec >= 1e3) {
        ncplane_printf_yx(metrics_plane, y++, 9, "%.1f kHz", cycles_per_sec / 1e3);
    } else {
        ncplane_printf_yx(metrics_plane, y++, 9, "%.0f Hz", cycles_per_sec);
    }

    ncplane_set_fg_rgb(metrics_plane, COL_LABEL);
    ncplane_putstr_yx(metrics_plane, y, 2, "Cycles:");
    ncplane_set_fg_rgb(metrics_plane, COL_VALUE);
    if (total_cycles >= 1e9) {
        ncplane_printf_yx(metrics_plane, y++, 10, "%.2fG", total_cycles / 1e9);
    } else if (total_cycles >= 1e6) {
        ncplane_printf_yx(metrics_plane, y++, 10, "%.2fM", total_cycles / 1e6);
    } else if (total_cycles >= 1e3) {
        ncplane_printf_yx(metrics_plane, y++, 10, "%.1fK", total_cycles / 1e3);
    } else {
        ncplane_printf_yx(metrics_plane, y++, 10, "%lu", total_cycles);
    }

    ncplane_set_fg_rgb(metrics_plane, COL_LABEL);
    ncplane_putstr_yx(metrics_plane, y, 2, "RAM:");
    ncplane_set_fg_rgb(metrics_plane, COL_VALUE);
    ncplane_printf_yx(metrics_plane, y++, 7, "%d%% used", get_emu_ram_usage());

    ncplane_set_fg_rgb(metrics_plane, COL_LABEL);
    ncplane_putstr_yx(metrics_plane, y, 2, "Stack:");
    ncplane_set_fg_rgb(metrics_plane, COL_VALUE);
    int stack_depth = (0x3800 - cpu.sp) / 2; /* Assuming stack at top of RAM */
    if (stack_depth < 0) stack_depth = 0;
    ncplane_printf_yx(metrics_plane, y++, 9, "%d words", stack_depth);

    ncplane_set_fg_rgb(metrics_plane, COL_LABEL);
    ncplane_putstr_yx(metrics_plane, y, 2, "ROM:");
    ncplane_set_fg_rgb(metrics_plane, COL_VALUE);
    ncplane_printf_yx(metrics_plane, y++, 7, "%d KB", rom_size / 1024);

    y++;

    /* I/O metrics */
    ncplane_set_fg_rgb(metrics_plane, COL_TITLE);
    ncplane_putstr_yx(metrics_plane, y++, 2, "── I/O ──");

    ncplane_set_fg_rgb(metrics_plane, COL_LABEL);
    ncplane_putstr_yx(metrics_plane, y, 2, "InBuf:");
    ncplane_set_fg_rgb(metrics_plane, COL_VALUE);
    int pending = (input_head - input_tail + INPUT_BUF_SIZE) % INPUT_BUF_SIZE;
    ncplane_printf_yx(metrics_plane, y++, 9, "%d chars", pending);

    ncplane_set_fg_rgb(metrics_plane, COL_LABEL);
    ncplane_putstr_yx(metrics_plane, y, 2, "Term:");
    ncplane_set_fg_rgb(metrics_plane, COL_VALUE);
    ncplane_printf_yx(metrics_plane, y++, 8, "%dx%d", TERM_COLS, TERM_ROWS);

    ncplane_set_fg_rgb(metrics_plane, COL_LABEL);
    ncplane_putstr_yx(metrics_plane, y, 2, "INT:");
    ncplane_set_fg_rgb(metrics_plane, COL_VALUE);
    ncplane_printf_yx(metrics_plane, y++, 7, "IM%d %s", cpu.interrupt_mode, cpu.iff1 ? "EI" : "DI");

    y++;

    /* Host metrics */
    ncplane_set_fg_rgb(metrics_plane, COL_TITLE);
    ncplane_putstr_yx(metrics_plane, y++, 2, "── Host ──");

    ncplane_set_fg_rgb(metrics_plane, COL_LABEL);
    ncplane_putstr_yx(metrics_plane, y, 2, "CPU:");
    ncplane_set_fg_rgb(metrics_plane, COL_VALUE);
    ncplane_printf_yx(metrics_plane, y++, 7, "%.1f%%", cpu_percent);

    ncplane_set_fg_rgb(metrics_plane, COL_LABEL);
    ncplane_putstr_yx(metrics_plane, y, 2, "Mem:");
    ncplane_set_fg_rgb(metrics_plane, COL_VALUE);
    size_t mem_kb = get_memory_usage_kb();
    if (mem_kb >= 1024) {
        ncplane_printf_yx(metrics_plane, y++, 7, "%.1f MB", mem_kb / 1024.0);
    } else {
        ncplane_printf_yx(metrics_plane, y++, 7, "%zu KB", mem_kb);
    }
}

/* Draw status bar */
static void draw_status(void) {
    ncplane_erase(status_plane);

    /* Status indicator */
    const char *status;
    uint32_t color;
    if (cpu.halted) {
        status = "HALTED";
        color = COL_STATUS_HALT;
    } else if (paused) {
        status = "PAUSED";
        color = COL_STATUS_PAUSE;
    } else {
        status = "RUNNING";
        color = COL_STATUS_RUN;
    }

    ncplane_set_fg_rgb(status_plane, color);
    ncplane_printf_yx(status_plane, 0, 1, "● %s", status);

    /* Cycle count */
    ncplane_set_fg_rgb(status_plane, COL_LABEL);
    ncplane_printf_yx(status_plane, 0, 15, "Cycles: ");
    ncplane_set_fg_rgb(status_plane, COL_VALUE);
    ncplane_printf_yx(status_plane, 0, 23, "%lu", total_cycles);

    /* Memory view address */
    ncplane_set_fg_rgb(status_plane, COL_LABEL);
    ncplane_printf_yx(status_plane, 0, 40, "Mem: ");
    ncplane_set_fg_rgb(status_plane, COL_VALUE);
    ncplane_printf_yx(status_plane, 0, 45, "$%04X", mem_view_addr);
}

/* Create planes for the TUI */
static int create_planes(void) {
    unsigned term_rows, term_cols;
    ncplane_dim_yx(stdp, &term_rows, &term_cols);

    /* Adaptive layout based on terminal size
     * Minimum: 80x24
     *
     * For small terminals (< 46 rows): simplified layout
     * For large terminals: full debugger layout
     */

    struct ncplane_options opts = {0};

    int metrics_width = 22;
    int left_width = (int)term_cols - metrics_width;
    if (left_width < 60) left_width = 60;

    int dis_width = left_width - 20;
    if (dis_width < 30) dis_width = 30;

    /* Calculate available space for terminal */
    int top_section_height = 18;  /* registers + memory */
    int bottom_bars = 2;          /* help + status */
    int term_height = (int)term_rows - top_section_height - bottom_bars;
    if (term_height < 6) term_height = 6;
    if (term_height > TERM_ROWS + 2) term_height = TERM_ROWS + 2;

    /* Registers: top-left */
    opts.y = 0;
    opts.x = 0;
    opts.rows = 8;
    opts.cols = 20;
    reg_plane = ncplane_create(stdp, &opts);
    if (!reg_plane) return -1;

    /* Disassembly: middle top */
    opts.y = 0;
    opts.x = 20;
    opts.rows = 8;
    opts.cols = dis_width;
    dis_plane = ncplane_create(stdp, &opts);
    if (!dis_plane) return -1;

    /* Metrics: right side */
    opts.y = 0;
    opts.x = left_width;
    opts.rows = 18;
    opts.cols = metrics_width;
    metrics_plane = ncplane_create(stdp, &opts);
    if (!metrics_plane) return -1;

    /* Memory: below registers/disassembly */
    opts.y = 8;
    opts.x = 0;
    opts.rows = 10;
    opts.cols = left_width;
    mem_plane = ncplane_create(stdp, &opts);
    if (!mem_plane) return -1;

    /* Terminal: below debug panels */
    opts.y = top_section_height;
    opts.x = 0;
    opts.rows = term_height;
    opts.cols = term_cols;
    term_plane = ncplane_create(stdp, &opts);
    if (!term_plane) return -1;

    /* Help bar */
    opts.y = term_rows - 2;
    opts.x = 0;
    opts.rows = 1;
    opts.cols = term_cols;
    help_plane = ncplane_create(stdp, &opts);
    if (!help_plane) return -1;

    /* Status bar */
    opts.y = term_rows - 1;
    opts.x = 0;
    opts.rows = 1;
    opts.cols = term_cols;
    status_plane = ncplane_create(stdp, &opts);
    if (!status_plane) return -1;

    return 0;
}

/* Render all panels */
static void render_all(void) {
    draw_registers();
    draw_disassembly();
    draw_metrics();
    draw_memory();
    draw_terminal();
    draw_help();
    draw_status();
    notcurses_render(nc);
}

/* Save previous register values */
static void save_prev_regs(void) {
    prev_pc = cpu.pc;
    prev_sp = cpu.sp;
    prev_ix = cpu.ix;
    prev_iy = cpu.iy;
    prev_a = cpu.a;
    prev_b = cpu.b;
    prev_c = cpu.c;
    prev_d = cpu.d;
    prev_e = cpu.e;
    prev_h = cpu.h;
    prev_l = cpu.l;
    prev_flags = get_flags();
}

/* Configure RAM based on ROM type (for metrics display) */
static void configure_ram_for_rom(const char *rom_file) {
    /* Extract base filename from path */
    const char *basename = strrchr(rom_file, '/');
    if (basename) {
        basename++;
    } else {
        basename = rom_file;
    }

    /* EFEX monitor: RAM at $E800-$FFFF */
    if (strstr(basename, "efex") != NULL) {
        ram_start = 0xE800;
        ram_end = 0xFFFF;
    }
    /* Grant's BASIC / default: RAM at $2000-$37FF */
    else {
        ram_start = 0x2000;
        ram_end = 0x37FF;
    }
}

/* Main function */
int main(int argc, char *argv[]) {
    const char *rom_file = NULL;

    /* Parse arguments */
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] != '-') {
            rom_file = argv[i];
        }
    }

    if (!rom_file) {
        fprintf(stderr, "Usage: %s <rom.bin>\n", argv[0]);
        return 1;
    }

    /* Configure ROM and RAM based on ROM type before loading */
    configure_rom(rom_file);
    configure_ram_for_rom(rom_file);

    /* Initialize memory and load ROM */
    memset(memory, 0, sizeof(memory));
    if (load_rom(rom_file) < 0) {
        fprintf(stderr, "Failed to load ROM: %s\n", rom_file);
        return 1;
    }

    /* Initialize CPU */
    z80_init(&cpu);
    cpu.read_byte = mem_read;
    cpu.write_byte = mem_write;
    cpu.port_in = port_in;
    cpu.port_out = port_out;

    /* Grant's BASIC cold start has a loop at $0150-$015F that does DEC D
     * and expects D to eventually become 1 so DEC D sets Z flag.
     * Real Z80 has undefined register values at power-on.
     * Initialize D=1 so the loop exits on first DEC D. */
    cpu.d = 1;

    /* Clear terminal */
    term_clear();

    /* Initialize notcurses */
    setlocale(LC_ALL, "");

    struct notcurses_options ncopts = {
        .flags = NCOPTION_SUPPRESS_BANNERS,
    };

    nc = notcurses_init(&ncopts, NULL);
    if (!nc) {
        fprintf(stderr, "Failed to initialize notcurses\n");
        return 1;
    }

    stdp = notcurses_stdplane(nc);

    /* Clear the emulated system's input buffer */
    input_head = 0;
    input_tail = 0;

    if (create_planes() < 0) {
        notcurses_stop(nc);
        fprintf(stderr, "Failed to create planes\n");
        return 1;
    }

    running = true;
    save_prev_regs();
    clock_gettime(CLOCK_MONOTONIC, &last_metrics_time);
    render_all();

    /* Main loop */
    struct timespec ts = {0, 10000000}; /* 10ms timeout for input */

    while (running) {
        ncinput ni;
        uint32_t id = notcurses_get(nc, &ts, &ni);

        if (id == (uint32_t)-1) {
            break; /* Error */
        }

        if (id == NCKEY_RESIZE) {
            /* Handle terminal resize */
            notcurses_refresh(nc, NULL, NULL);
            /* Recreate planes - for now just re-render */
            render_all();
            continue;
        }

        if (id != 0) {
            /* Only handle key press events, not releases */
            if (ni.evtype == NCTYPE_RELEASE) {
                continue;
            }

            bool need_render = true;

            switch (id) {
                case NCKEY_F12:
                    running = false;
                    break;

                case NCKEY_F05:  /* Run */
                    paused = false;
                    break;

                case NCKEY_F06:  /* Step */
                    if (!cpu.halted) {
                        save_prev_regs();
                        z80_step(&cpu);
                        total_cycles = cpu.cyc;
                    }
                    break;

                case NCKEY_F07:  /* Pause */
                    paused = true;
                    break;

                case NCKEY_F08:  /* Reset */
                    z80_init(&cpu);
                    cpu.read_byte = mem_read;
                    cpu.write_byte = mem_write;
                    cpu.port_in = port_in;
                    cpu.port_out = port_out;
                    total_cycles = 0;
                    term_clear();
                    paused = true;
                    save_prev_regs();
                    break;

                case NCKEY_PGUP:
                    if (mem_view_addr >= 0x80) {
                        mem_view_addr -= 0x80;
                    } else {
                        mem_view_addr = 0;
                    }
                    break;

                case NCKEY_PGDOWN:
                    if (mem_view_addr + 0x80 < MEM_SIZE) {
                        mem_view_addr += 0x80;
                    }
                    break;

                case NCKEY_HOME:  /* Memory view to PC */
                    mem_view_addr = cpu.pc & 0xFFF0;
                    break;

                case NCKEY_END:  /* Memory view to $2000 (input buffer) */
                    mem_view_addr = 0x2000;
                    break;

                default:
                    /* Send printable characters to the emulated system */
                    if (id >= 32 && id < 127) {
                        input_putchar((char)id);
                    } else if (id == NCKEY_ENTER || id == '\r' || id == '\n') {
                        input_putchar('\r');
                    } else if (id == NCKEY_BACKSPACE || id == 127) {
                        input_putchar('\b');
                    }
                    need_render = false;
                    break;
            }

            if (need_render) {
                render_all();
            }
        }

        /* Run CPU if not paused */
        if (!paused && !cpu.halted) {
            save_prev_regs();
            for (int i = 0; i < cycles_per_frame && !cpu.halted; i++) {
                z80_step(&cpu);
                /* Trigger interrupt if input available (for 8251 USART ROMs only) */
                /* Check after step so iff_delay has been processed */
                if (uses_8251 && input_available() && cpu.iff1 && !int_signaled && cpu.iff_delay == 0) {
                    z80_gen_int(&cpu, 0xFF);  /* RST 38H in IM1 mode */
                    int_signaled = true;
                }
            }
            total_cycles = cpu.cyc;
            render_all();
        }
    }

    /* Cleanup */
    notcurses_stop(nc);

    /* Drain any remaining terminal responses */
    usleep(100000); /* 100ms for terminal to finish responding */

    /* Consume any garbage left in stdin */
    {
        struct termios oldt, newt;
        tcgetattr(STDIN_FILENO, &oldt);
        newt = oldt;
        newt.c_lflag &= ~(ICANON | ECHO);
        newt.c_cc[VMIN] = 0;
        newt.c_cc[VTIME] = 1; /* 100ms timeout */
        tcsetattr(STDIN_FILENO, TCSANOW, &newt);
        while (read(STDIN_FILENO, &(char){0}, 1) > 0) {
            /* drain */
        }
        tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
    }

    return 0;
}
