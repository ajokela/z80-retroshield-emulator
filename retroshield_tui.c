/*
 * RetroShield Z80 Emulator - TUI Version
 * Uses ncurses for the text user interface
 * Features: register view, disassembly, memory dump, terminal output
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
#include <ncurses.h>
#include <locale.h>

#include "z80.h"
#include "z80_disasm.h"

#define ROM_SIZE 0x2000   /* 8KB ROM */
#define RAM_START 0x2000
#define RAM_SIZE 0x6000   /* 24KB RAM */
#define MEM_SIZE 0x8000   /* 32KB total */

/* MC6850 ACIA ports */
#define ACIA_CTRL 0x80
#define ACIA_DATA 0x81
#define ACIA_RDRF 0x01
#define ACIA_TDRE 0x02

/* Terminal buffer */
#define TERM_COLS 78
#define TERM_ROWS 12
static char term_buffer[TERM_ROWS][TERM_COLS + 1];
static int term_row = 0;
static int term_col = 0;

/* Input buffer for the emulated system */
#define INPUT_BUF_SIZE 256
static char input_buffer[INPUT_BUF_SIZE];
static int input_head = 0;
static int input_tail = 0;

static uint8_t memory[MEM_SIZE];
static z80 cpu;

/* Emulator state */
typedef enum {
    MODE_RUN,
    MODE_PAUSE,
    MODE_STEP
} emu_mode_t;

static emu_mode_t emu_mode = MODE_PAUSE;
static uint16_t mem_view_addr = RAM_START;
static bool quit_flag = false;
static int run_speed = 10000;  /* Instructions per frame */

/* Windows */
static WINDOW *win_regs;
static WINDOW *win_disasm;
static WINDOW *win_mem;
static WINDOW *win_term;
static WINDOW *win_status;

/* Colors */
#define COLOR_TITLE   1
#define COLOR_HILITE  2
#define COLOR_DIM     3
#define COLOR_CURSOR  4
#define COLOR_STATUS  5

/* Terminal buffer functions */
static void term_clear(void) {
    for (int r = 0; r < TERM_ROWS; r++) {
        memset(term_buffer[r], ' ', TERM_COLS);
        term_buffer[r][TERM_COLS] = '\0';
    }
    term_row = 0;
    term_col = 0;
}

static void term_scroll(void) {
    for (int r = 0; r < TERM_ROWS - 1; r++) {
        memcpy(term_buffer[r], term_buffer[r + 1], TERM_COLS);
    }
    memset(term_buffer[TERM_ROWS - 1], ' ', TERM_COLS);
    term_buffer[TERM_ROWS - 1][TERM_COLS] = '\0';
}

static void term_putchar(char c) {
    if (c == '\r') {
        term_col = 0;
    } else if (c == '\n') {
        term_col = 0;
        term_row++;
        if (term_row >= TERM_ROWS) {
            term_scroll();
            term_row = TERM_ROWS - 1;
        }
    } else if (c == '\b') {
        if (term_col > 0) term_col--;
    } else if (c >= 32 && c < 127) {
        if (term_col < TERM_COLS) {
            term_buffer[term_row][term_col++] = c;
        }
    }
}

/* Input buffer functions */
static bool input_available(void) {
    return input_head != input_tail;
}

static char input_get(void) {
    if (input_head == input_tail) return 0;
    char c = input_buffer[input_tail];
    input_tail = (input_tail + 1) % INPUT_BUF_SIZE;
    return c;
}

static void input_put(char c) {
    int next = (input_head + 1) % INPUT_BUF_SIZE;
    if (next != input_tail) {
        input_buffer[input_head] = c;
        input_head = next;
    }
}

/* Memory callbacks */
static uint8_t mem_read(void *userdata, uint16_t addr) {
    (void)userdata;
    return (addr < MEM_SIZE) ? memory[addr] : 0xFF;
}

static void mem_write(void *userdata, uint16_t addr, uint8_t val) {
    (void)userdata;
    if (addr >= RAM_START && addr < MEM_SIZE) {
        memory[addr] = val;
    }
}

/* I/O callbacks */
static uint8_t port_in(z80 *z, uint8_t port) {
    (void)z;
    if (port == ACIA_CTRL) {
        uint8_t status = ACIA_TDRE;
        if (input_available()) status |= ACIA_RDRF;
        return status;
    } else if (port == ACIA_DATA) {
        return input_get();
    }
    return 0xFF;
}

static void port_out(z80 *z, uint8_t port, uint8_t val) {
    (void)z;
    if (port == ACIA_DATA) {
        term_putchar(val);
    }
}

/* Load ROM */
static int load_rom(const char *filename) {
    FILE *f = fopen(filename, "rb");
    if (!f) return -1;
    size_t bytes = fread(memory, 1, ROM_SIZE, f);
    fclose(f);
    return (bytes > 0) ? 0 : -1;
}

/* Create bordered window */
static WINDOW *create_win(int h, int w, int y, int x, const char *title) {
    WINDOW *win = newwin(h, w, y, x);
    box(win, 0, 0);
    if (title) {
        wattron(win, COLOR_PAIR(COLOR_TITLE) | A_BOLD);
        mvwprintw(win, 0, 2, " %s ", title);
        wattroff(win, COLOR_PAIR(COLOR_TITLE) | A_BOLD);
    }
    return win;
}

/* Draw register window */
static void draw_regs(void) {
    werase(win_regs);
    box(win_regs, 0, 0);
    wattron(win_regs, COLOR_PAIR(COLOR_TITLE) | A_BOLD);
    mvwprintw(win_regs, 0, 2, " Registers ");
    wattroff(win_regs, COLOR_PAIR(COLOR_TITLE) | A_BOLD);

    /* Compute F register from flags */
    uint8_t f = (cpu.sf << 7) | (cpu.zf << 6) | (cpu.yf << 5) | (cpu.hf << 4) |
                (cpu.xf << 3) | (cpu.pf << 2) | (cpu.nf << 1) | cpu.cf;

    mvwprintw(win_regs, 1, 2, "PC:%04X  SP:%04X", cpu.pc, cpu.sp);
    mvwprintw(win_regs, 2, 2, "AF:%02X%02X  AF':%02X%02X", cpu.a, f, cpu.a_, cpu.f_);
    mvwprintw(win_regs, 3, 2, "BC:%02X%02X  BC':%02X%02X", cpu.b, cpu.c, cpu.b_, cpu.c_);
    mvwprintw(win_regs, 4, 2, "DE:%02X%02X  DE':%02X%02X", cpu.d, cpu.e, cpu.d_, cpu.e_);
    mvwprintw(win_regs, 5, 2, "HL:%02X%02X  HL':%02X%02X", cpu.h, cpu.l, cpu.h_, cpu.l_);
    mvwprintw(win_regs, 6, 2, "IX:%04X  IY:%04X", cpu.ix, cpu.iy);
    mvwprintw(win_regs, 7, 2, "I:%02X R:%02X  IM:%d", cpu.i, cpu.r, cpu.interrupt_mode);

    /* Flags */
    mvwprintw(win_regs, 8, 2, "Flags: S Z - H - P N C");
    mvwprintw(win_regs, 9, 9, "%d %d %d %d %d %d %d %d",
              cpu.sf, cpu.zf, cpu.yf, cpu.hf, cpu.xf, cpu.pf, cpu.nf, cpu.cf);

    mvwprintw(win_regs, 10, 2, "Cyc: %lu", cpu.cyc);
    if (cpu.halted) {
        wattron(win_regs, COLOR_PAIR(COLOR_HILITE) | A_BOLD);
        mvwprintw(win_regs, 10, 15, "HALT");
        wattroff(win_regs, COLOR_PAIR(COLOR_HILITE) | A_BOLD);
    }

    wrefresh(win_regs);
}

/* Draw disassembly window */
static void draw_disasm(void) {
    werase(win_disasm);
    box(win_disasm, 0, 0);
    wattron(win_disasm, COLOR_PAIR(COLOR_TITLE) | A_BOLD);
    mvwprintw(win_disasm, 0, 2, " Disassembly ");
    wattroff(win_disasm, COLOR_PAIR(COLOR_TITLE) | A_BOLD);

    /* Disassemble around PC */
    uint16_t addr = cpu.pc;
    /* Try to show a few instructions before PC */
    uint16_t scan = (cpu.pc > 16) ? cpu.pc - 16 : 0;
    uint16_t before[8];
    int before_count = 0;

    while (scan < cpu.pc && before_count < 8) {
        char buf[32];
        int len = z80_disasm(memory, scan, buf, sizeof(buf));
        if (scan + len > cpu.pc) break;
        before[before_count++] = scan;
        scan += len;
    }

    /* Show 3 instructions before PC if possible */
    int start_idx = (before_count > 3) ? before_count - 3 : 0;
    int row = 1;
    int max_rows = 10;

    for (int i = start_idx; i < before_count && row < max_rows; i++) {
        char buf[32];
        z80_disasm(memory, before[i], buf, sizeof(buf));
        wattron(win_disasm, COLOR_PAIR(COLOR_DIM));
        mvwprintw(win_disasm, row++, 2, "%04X: %s", before[i], buf);
        wattroff(win_disasm, COLOR_PAIR(COLOR_DIM));
    }

    /* Current instruction */
    addr = cpu.pc;
    for (int i = 0; i < 7 && row < max_rows; i++) {
        char buf[32];
        int len = z80_disasm(memory, addr, buf, sizeof(buf));

        if (i == 0) {
            wattron(win_disasm, COLOR_PAIR(COLOR_CURSOR) | A_BOLD);
            mvwprintw(win_disasm, row++, 1, ">%04X: %-24s", addr, buf);
            wattroff(win_disasm, COLOR_PAIR(COLOR_CURSOR) | A_BOLD);
        } else {
            mvwprintw(win_disasm, row++, 2, "%04X: %s", addr, buf);
        }
        addr += len;
    }

    wrefresh(win_disasm);
}

/* Draw memory window */
static void draw_mem(void) {
    werase(win_mem);
    box(win_mem, 0, 0);
    wattron(win_mem, COLOR_PAIR(COLOR_TITLE) | A_BOLD);
    mvwprintw(win_mem, 0, 2, " Memory @ $%04X ", mem_view_addr);
    wattroff(win_mem, COLOR_PAIR(COLOR_TITLE) | A_BOLD);

    uint16_t addr = mem_view_addr;
    for (int row = 1; row <= 8; row++) {
        mvwprintw(win_mem, row, 2, "%04X:", addr);
        for (int col = 0; col < 8; col++) {
            uint8_t b = memory[(addr + col) & 0xFFFF];
            if (addr + col == cpu.pc) {
                wattron(win_mem, COLOR_PAIR(COLOR_CURSOR) | A_BOLD);
            }
            wprintw(win_mem, " %02X", b);
            if (addr + col == cpu.pc) {
                wattroff(win_mem, COLOR_PAIR(COLOR_CURSOR) | A_BOLD);
            }
        }
        wprintw(win_mem, "  ");
        for (int col = 0; col < 8; col++) {
            uint8_t b = memory[(addr + col) & 0xFFFF];
            char c = (b >= 32 && b < 127) ? b : '.';
            wprintw(win_mem, "%c", c);
        }
        addr += 8;
    }

    wrefresh(win_mem);
}

/* Draw terminal window */
static void draw_term(void) {
    werase(win_term);
    box(win_term, 0, 0);
    wattron(win_term, COLOR_PAIR(COLOR_TITLE) | A_BOLD);
    mvwprintw(win_term, 0, 2, " Terminal ");
    wattroff(win_term, COLOR_PAIR(COLOR_TITLE) | A_BOLD);

    for (int r = 0; r < TERM_ROWS; r++) {
        mvwprintw(win_term, r + 1, 1, "%.*s", TERM_COLS, term_buffer[r]);
    }

    /* Show cursor */
    if (term_row < TERM_ROWS && term_col < TERM_COLS) {
        wmove(win_term, term_row + 1, term_col + 1);
    }

    wrefresh(win_term);
}

/* Draw status bar */
static void draw_status(void) {
    werase(win_status);

    wattron(win_status, COLOR_PAIR(COLOR_STATUS));
    for (int i = 0; i < COLS; i++) {
        mvwaddch(win_status, 0, i, ' ');
    }

    const char *mode_str = (emu_mode == MODE_RUN) ? "RUNNING" :
                           (emu_mode == MODE_STEP) ? "STEP" : "PAUSED";

    mvwprintw(win_status, 0, 1, "[%s]", mode_str);
    mvwprintw(win_status, 0, 15,
              "F1:Help F5:Run F6:Step F7:Pause F8:Reset F9:MemUp F10:MemDn F12:Quit");
    wattroff(win_status, COLOR_PAIR(COLOR_STATUS));

    wrefresh(win_status);
}

/* Draw help popup */
static void show_help(void) {
    int h = 16, w = 50;
    int y = (LINES - h) / 2;
    int x = (COLS - w) / 2;

    WINDOW *help = newwin(h, w, y, x);
    box(help, 0, 0);
    wattron(help, COLOR_PAIR(COLOR_TITLE) | A_BOLD);
    mvwprintw(help, 0, 2, " Help ");
    wattroff(help, COLOR_PAIR(COLOR_TITLE) | A_BOLD);

    mvwprintw(help, 2, 2,  "F1         - Show this help");
    mvwprintw(help, 3, 2,  "F5         - Run continuously");
    mvwprintw(help, 4, 2,  "F6         - Step one instruction");
    mvwprintw(help, 5, 2,  "F7         - Pause execution");
    mvwprintw(help, 6, 2,  "F8         - Reset CPU");
    mvwprintw(help, 7, 2,  "F9         - Memory view up");
    mvwprintw(help, 8, 2,  "F10        - Memory view down");
    mvwprintw(help, 9, 2,  "F12        - Quit");
    mvwprintw(help, 10, 2, "+/-        - Adjust run speed");
    mvwprintw(help, 11, 2, "Other keys - Send to terminal");
    mvwprintw(help, 13, 2, "Press any key to close...");

    wrefresh(help);
    nodelay(stdscr, FALSE);
    getch();
    nodelay(stdscr, TRUE);
    delwin(help);
}

/* Reset CPU */
static void reset_cpu(void) {
    z80_init(&cpu);
    cpu.read_byte = mem_read;
    cpu.write_byte = mem_write;
    cpu.port_in = port_in;
    cpu.port_out = port_out;
    term_clear();
    input_head = input_tail = 0;
}

/* Main TUI loop */
static void tui_main(void) {
    int ch;

    while (!quit_flag) {
        /* Handle input */
        ch = getch();
        if (ch != ERR) {
            switch (ch) {
                case KEY_F(1):
                    show_help();
                    break;
                case KEY_F(5):
                    emu_mode = MODE_RUN;
                    break;
                case KEY_F(6):
                    emu_mode = MODE_STEP;
                    break;
                case KEY_F(7):
                    emu_mode = MODE_PAUSE;
                    break;
                case KEY_F(8):
                    reset_cpu();
                    emu_mode = MODE_PAUSE;
                    break;
                case KEY_F(9):
                    if (mem_view_addr >= 64) mem_view_addr -= 64;
                    break;
                case KEY_F(10):
                    if (mem_view_addr < 0xFF00) mem_view_addr += 64;
                    break;
                case KEY_F(12):
                    quit_flag = true;
                    break;
                case '+':
                case '=':
                    if (run_speed < 100000) run_speed += 1000;
                    break;
                case '-':
                case '_':
                    if (run_speed > 1000) run_speed -= 1000;
                    break;
                case '\n':
                case '\r':
                    input_put('\r');
                    input_put('\n');
                    break;
                default:
                    if (ch >= 0 && ch < 256) {
                        input_put(ch);
                    }
                    break;
            }
        }

        /* Execute instructions */
        if (emu_mode == MODE_RUN && !cpu.halted) {
            for (int i = 0; i < run_speed && !cpu.halted; i++) {
                z80_step(&cpu);
            }
        } else if (emu_mode == MODE_STEP && !cpu.halted) {
            z80_step(&cpu);
            emu_mode = MODE_PAUSE;
        }

        /* Update display */
        draw_regs();
        draw_disasm();
        draw_mem();
        draw_term();
        draw_status();

        /* Small delay to prevent 100% CPU */
        usleep(16000);  /* ~60 FPS */
    }
}

/* Initialize ncurses and create windows */
static void init_tui(void) {
    setlocale(LC_ALL, "");
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    nodelay(stdscr, TRUE);
    curs_set(0);

    if (has_colors()) {
        start_color();
        use_default_colors();
        init_pair(COLOR_TITLE, COLOR_CYAN, -1);
        init_pair(COLOR_HILITE, COLOR_RED, -1);
        init_pair(COLOR_DIM, COLOR_BLUE, -1);
        init_pair(COLOR_CURSOR, COLOR_BLACK, COLOR_YELLOW);
        init_pair(COLOR_STATUS, COLOR_WHITE, COLOR_BLUE);
    }

    /* Layout:
     * Registers (12h x 25w) | Disassembly (12h x rest)
     * Memory (10h x full)
     * Terminal (14h x full)
     * Status (1h x full)
     */
    int term_h = TERM_ROWS + 2;
    int mem_h = 10;
    int regs_h = 12;
    int regs_w = 26;

    win_regs = create_win(regs_h, regs_w, 0, 0, NULL);
    win_disasm = create_win(regs_h, COLS - regs_w, 0, regs_w, NULL);
    win_mem = create_win(mem_h, COLS, regs_h, 0, NULL);
    win_term = create_win(term_h, COLS, regs_h + mem_h, 0, NULL);
    win_status = newwin(1, COLS, LINES - 1, 0);

    refresh();
}

/* Cleanup ncurses */
static void cleanup_tui(void) {
    delwin(win_regs);
    delwin(win_disasm);
    delwin(win_mem);
    delwin(win_term);
    delwin(win_status);
    endwin();
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <rom.bin>\n", argv[0]);
        return 1;
    }

    memset(memory, 0, sizeof(memory));

    if (load_rom(argv[1]) < 0) {
        perror("Failed to load ROM");
        return 1;
    }

    /* Initialize CPU */
    z80_init(&cpu);
    cpu.read_byte = mem_read;
    cpu.write_byte = mem_write;
    cpu.port_in = port_in;
    cpu.port_out = port_out;

    term_clear();

    init_tui();
    tui_main();
    cleanup_tui();

    return 0;
}
