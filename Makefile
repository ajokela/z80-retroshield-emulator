# RetroShield Z80 Emulator Makefile
# Uses superzazu/z80 (MIT license)

CC = cc
CFLAGS = -O2 -Wall -Wextra -std=c99
LDFLAGS =

# ROM file - override with: make run ROM=myrom.bin
ROM ?= rom.bin

# Standard emulator (passthrough I/O)
TARGET = retroshield
SOURCES = retroshield.c z80.c
OBJECTS = $(SOURCES:.c=.o)

# TUI emulator with ncurses debugger
TUI_TARGET = retroshield_tui
TUI_SOURCES = retroshield_tui.c z80.c z80_disasm.c
TUI_OBJECTS = $(TUI_SOURCES:.c=.o)
TUI_LDFLAGS = -lncurses

# Notcurses TUI emulator (modern TUI)
NC_TARGET = retroshield_nc
NC_SOURCES = retroshield_nc.c z80.c z80_disasm.c
NC_OBJECTS = $(NC_SOURCES:.c=.o)
NC_CFLAGS = $(shell pkg-config --cflags notcurses 2>/dev/null)
NC_LDFLAGS = $(shell pkg-config --libs notcurses 2>/dev/null)

all: $(TARGET) $(TUI_TARGET)

# Build notcurses version if available
ifneq ($(NC_LDFLAGS),)
all: $(NC_TARGET)
endif

$(TARGET): $(OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $(OBJECTS)

$(TUI_TARGET): $(TUI_OBJECTS)
	$(CC) $(LDFLAGS) $(TUI_LDFLAGS) -o $@ $(TUI_OBJECTS)

$(NC_TARGET): $(NC_OBJECTS)
	$(CC) $(LDFLAGS) $(NC_LDFLAGS) -o $@ $(NC_OBJECTS)

retroshield.o: retroshield.c z80.h
	$(CC) $(CFLAGS) -c -o $@ $<

retroshield_tui.o: retroshield_tui.c z80.h z80_disasm.h
	$(CC) $(CFLAGS) -c -o $@ $<

retroshield_nc.o: retroshield_nc.c z80.h z80_disasm.h
	$(CC) $(CFLAGS) $(NC_CFLAGS) -c -o $@ $<

z80.o: z80.c z80.h
	$(CC) $(CFLAGS) -c -o $@ $<

z80_disasm.o: z80_disasm.c z80_disasm.h
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(TARGET) $(TUI_TARGET) $(NC_TARGET) *.o

# Run emulator (passthrough mode)
run: $(TARGET)
	./$(TARGET) $(ROM)

# Run ncurses TUI debugger
tui: $(TUI_TARGET)
	./$(TUI_TARGET) $(ROM)

# Run notcurses TUI debugger
nc: $(NC_TARGET)
	./$(NC_TARGET) $(ROM)

.PHONY: all clean run tui nc
