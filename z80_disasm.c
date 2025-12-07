/*
 * Z80 Disassembler
 * Supports main instruction set, CB prefix, ED prefix, DD/FD (IX/IY)
 *
 * Copyright (c) 2025 Alex Jokela
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <string.h>
#include "z80_disasm.h"

static const char *r8[] = {"B", "C", "D", "E", "H", "L", "(HL)", "A"};
static const char *r16[] = {"BC", "DE", "HL", "SP"};
static const char *r16af[] = {"BC", "DE", "HL", "AF"};
static const char *cc[] = {"NZ", "Z", "NC", "C", "PO", "PE", "P", "M"};
static const char *alu[] = {"ADD A,", "ADC A,", "SUB", "SBC A,", "AND", "XOR", "OR", "CP"};

/* CB prefix instructions */
static int disasm_cb(uint8_t *mem, uint16_t addr, char *buf, int bufsize, const char *ixiy, int8_t d) {
    uint8_t op = mem[(addr + (ixiy ? 1 : 0)) & 0xFFFF];
    int x = (op >> 6) & 3;
    int y = (op >> 3) & 7;
    int z = op & 7;

    const char *rot[] = {"RLC", "RRC", "RL", "RR", "SLA", "SRA", "SLL", "SRL"};

    if (ixiy && z == 6) {
        /* (IX+d) or (IY+d) form */
        if (x == 0) {
            snprintf(buf, bufsize, "%s (%s%+d)", rot[y], ixiy, d);
        } else if (x == 1) {
            snprintf(buf, bufsize, "BIT %d,(%s%+d)", y, ixiy, d);
        } else if (x == 2) {
            snprintf(buf, bufsize, "RES %d,(%s%+d)", y, ixiy, d);
        } else {
            snprintf(buf, bufsize, "SET %d,(%s%+d)", y, ixiy, d);
        }
        return 4;
    }

    if (x == 0) {
        snprintf(buf, bufsize, "%s %s", rot[y], r8[z]);
    } else if (x == 1) {
        snprintf(buf, bufsize, "BIT %d,%s", y, r8[z]);
    } else if (x == 2) {
        snprintf(buf, bufsize, "RES %d,%s", y, r8[z]);
    } else {
        snprintf(buf, bufsize, "SET %d,%s", y, r8[z]);
    }
    return 2;
}

/* ED prefix instructions */
static int disasm_ed(uint8_t *mem, uint16_t addr, char *buf, int bufsize) {
    uint8_t op = mem[(addr) & 0xFFFF];
    int x = (op >> 6) & 3;
    int y = (op >> 3) & 7;
    int z = op & 7;
    int p = y >> 1;
    int q = y & 1;

    if (x == 1) {
        if (z == 0) {
            if (y == 6) snprintf(buf, bufsize, "IN (C)");
            else snprintf(buf, bufsize, "IN %s,(C)", r8[y]);
            return 2;
        } else if (z == 1) {
            if (y == 6) snprintf(buf, bufsize, "OUT (C),0");
            else snprintf(buf, bufsize, "OUT (C),%s", r8[y]);
            return 2;
        } else if (z == 2) {
            if (q == 0) snprintf(buf, bufsize, "SBC HL,%s", r16[p]);
            else snprintf(buf, bufsize, "ADC HL,%s", r16[p]);
            return 2;
        } else if (z == 3) {
            uint16_t nn = mem[(addr+1) & 0xFFFF] | (mem[(addr+2) & 0xFFFF] << 8);
            if (q == 0) snprintf(buf, bufsize, "LD ($%04X),%s", nn, r16[p]);
            else snprintf(buf, bufsize, "LD %s,($%04X)", r16[p], nn);
            return 4;
        } else if (z == 4) {
            snprintf(buf, bufsize, "NEG");
            return 2;
        } else if (z == 5) {
            if (y == 1) snprintf(buf, bufsize, "RETI");
            else snprintf(buf, bufsize, "RETN");
            return 2;
        } else if (z == 6) {
            const char *im[] = {"0", "0/1", "1", "2", "0", "0/1", "1", "2"};
            snprintf(buf, bufsize, "IM %s", im[y]);
            return 2;
        } else {
            const char *misc[] = {"LD I,A", "LD R,A", "LD A,I", "LD A,R", "RRD", "RLD", "NOP", "NOP"};
            snprintf(buf, bufsize, "%s", misc[y]);
            return 2;
        }
    } else if (x == 2 && z <= 3 && y >= 4) {
        const char *blk[][4] = {
            {"LDI", "CPI", "INI", "OUTI"},
            {"LDD", "CPD", "IND", "OUTD"},
            {"LDIR", "CPIR", "INIR", "OTIR"},
            {"LDDR", "CPDR", "INDR", "OTDR"}
        };
        snprintf(buf, bufsize, "%s", blk[y-4][z]);
        return 2;
    }

    snprintf(buf, bufsize, "DB $ED,$%02X", op);
    return 2;
}

int z80_disasm(uint8_t *mem, uint16_t addr, char *buf, int bufsize) {
    uint8_t op = mem[addr & 0xFFFF];
    int x = (op >> 6) & 3;
    int y = (op >> 3) & 7;
    int z = op & 7;
    int p = y >> 1;
    int q = y & 1;

    const char *ixiy = NULL;
    int prefix_len = 0;

    /* Check for DD/FD prefix */
    if (op == 0xDD || op == 0xFD) {
        ixiy = (op == 0xDD) ? "IX" : "IY";
        prefix_len = 1;
        addr++;
        op = mem[addr & 0xFFFF];
        x = (op >> 6) & 3;
        y = (op >> 3) & 7;
        z = op & 7;
        p = y >> 1;
        q = y & 1;
    }

    /* CB prefix */
    if (op == 0xCB) {
        if (ixiy) {
            int8_t d = (int8_t)mem[(addr + 1) & 0xFFFF];
            return disasm_cb(mem, addr + 1, buf, bufsize, ixiy, d);
        }
        return disasm_cb(mem, addr + 1, buf, bufsize, NULL, 0);
    }

    /* ED prefix */
    if (op == 0xED) {
        return prefix_len + disasm_ed(mem, addr + 1, buf, bufsize);
    }

    /* Main opcodes */
    if (x == 0) {
        if (z == 0) {
            if (y == 0) { snprintf(buf, bufsize, "NOP"); return 1 + prefix_len; }
            if (y == 1) { snprintf(buf, bufsize, "EX AF,AF'"); return 1 + prefix_len; }
            if (y == 2) {
                int8_t d = (int8_t)mem[(addr + 1) & 0xFFFF];
                snprintf(buf, bufsize, "DJNZ $%04X", (addr + 2 + d) & 0xFFFF);
                return 2 + prefix_len;
            }
            if (y == 3) {
                int8_t d = (int8_t)mem[(addr + 1) & 0xFFFF];
                snprintf(buf, bufsize, "JR $%04X", (addr + 2 + d) & 0xFFFF);
                return 2 + prefix_len;
            }
            int8_t d = (int8_t)mem[(addr + 1) & 0xFFFF];
            snprintf(buf, bufsize, "JR %s,$%04X", cc[y-4], (addr + 2 + d) & 0xFFFF);
            return 2 + prefix_len;
        }
        if (z == 1) {
            if (q == 0) {
                uint16_t nn = mem[(addr+1) & 0xFFFF] | (mem[(addr+2) & 0xFFFF] << 8);
                if (ixiy && p == 2) snprintf(buf, bufsize, "LD %s,$%04X", ixiy, nn);
                else snprintf(buf, bufsize, "LD %s,$%04X", r16[p], nn);
                return 3 + prefix_len;
            } else {
                if (ixiy && p == 2) snprintf(buf, bufsize, "ADD %s,%s", ixiy, r16[p]);
                else snprintf(buf, bufsize, "ADD HL,%s", r16[p]);
                return 1 + prefix_len;
            }
        }
        if (z == 2) {
            const char *ld2[] = {"LD (BC),A", "LD A,(BC)", "LD (DE),A", "LD A,(DE)"};
            if (y < 4) { snprintf(buf, bufsize, "%s", ld2[y]); return 1 + prefix_len; }
            uint16_t nn = mem[(addr+1) & 0xFFFF] | (mem[(addr+2) & 0xFFFF] << 8);
            if (y == 4) {
                if (ixiy) snprintf(buf, bufsize, "LD ($%04X),%s", nn, ixiy);
                else snprintf(buf, bufsize, "LD ($%04X),HL", nn);
            }
            else if (y == 5) {
                if (ixiy) snprintf(buf, bufsize, "LD %s,($%04X)", ixiy, nn);
                else snprintf(buf, bufsize, "LD HL,($%04X)", nn);
            }
            else if (y == 6) snprintf(buf, bufsize, "LD ($%04X),A", nn);
            else snprintf(buf, bufsize, "LD A,($%04X)", nn);
            return 3 + prefix_len;
        }
        if (z == 3) {
            if (ixiy && p == 2) {
                snprintf(buf, bufsize, "%s %s", q ? "DEC" : "INC", ixiy);
            } else {
                snprintf(buf, bufsize, "%s %s", q ? "DEC" : "INC", r16[p]);
            }
            return 1 + prefix_len;
        }
        if (z == 4) {
            if (ixiy && y == 6) {
                int8_t d = (int8_t)mem[(addr + 1) & 0xFFFF];
                snprintf(buf, bufsize, "INC (%s%+d)", ixiy, d);
                return 3;
            }
            snprintf(buf, bufsize, "INC %s", r8[y]);
            return 1 + prefix_len;
        }
        if (z == 5) {
            if (ixiy && y == 6) {
                int8_t d = (int8_t)mem[(addr + 1) & 0xFFFF];
                snprintf(buf, bufsize, "DEC (%s%+d)", ixiy, d);
                return 3;
            }
            snprintf(buf, bufsize, "DEC %s", r8[y]);
            return 1 + prefix_len;
        }
        if (z == 6) {
            uint8_t n = mem[(addr + 1) & 0xFFFF];
            if (ixiy && y == 6) {
                int8_t d = (int8_t)mem[(addr + 1) & 0xFFFF];
                n = mem[(addr + 2) & 0xFFFF];
                snprintf(buf, bufsize, "LD (%s%+d),$%02X", ixiy, d, n);
                return 4;
            }
            snprintf(buf, bufsize, "LD %s,$%02X", r8[y], n);
            return 2 + prefix_len;
        }
        if (z == 7) {
            const char *misc[] = {"RLCA", "RRCA", "RLA", "RRA", "DAA", "CPL", "SCF", "CCF"};
            snprintf(buf, bufsize, "%s", misc[y]);
            return 1 + prefix_len;
        }
    }

    if (x == 1) {
        if (z == 6 && y == 6) {
            snprintf(buf, bufsize, "HALT");
            return 1 + prefix_len;
        }
        if (ixiy && (y == 6 || z == 6)) {
            int8_t d = (int8_t)mem[(addr + 1) & 0xFFFF];
            if (y == 6) snprintf(buf, bufsize, "LD (%s%+d),%s", ixiy, d, r8[z]);
            else snprintf(buf, bufsize, "LD %s,(%s%+d)", r8[y], ixiy, d);
            return 3;
        }
        snprintf(buf, bufsize, "LD %s,%s", r8[y], r8[z]);
        return 1 + prefix_len;
    }

    if (x == 2) {
        if (ixiy && z == 6) {
            int8_t d = (int8_t)mem[(addr + 1) & 0xFFFF];
            snprintf(buf, bufsize, "%s (%s%+d)", alu[y], ixiy, d);
            return 3;
        }
        snprintf(buf, bufsize, "%s %s", alu[y], r8[z]);
        return 1 + prefix_len;
    }

    if (x == 3) {
        if (z == 0) {
            snprintf(buf, bufsize, "RET %s", cc[y]);
            return 1 + prefix_len;
        }
        if (z == 1) {
            if (q == 0) {
                if (ixiy && p == 2) snprintf(buf, bufsize, "POP %s", ixiy);
                else snprintf(buf, bufsize, "POP %s", r16af[p]);
            } else {
                const char *misc[] = {"RET", "EXX", "JP (HL)", "LD SP,HL"};
                if (ixiy && p >= 2) {
                    if (p == 2) snprintf(buf, bufsize, "JP (%s)", ixiy);
                    else snprintf(buf, bufsize, "LD SP,%s", ixiy);
                } else {
                    snprintf(buf, bufsize, "%s", misc[p]);
                }
            }
            return 1 + prefix_len;
        }
        if (z == 2) {
            uint16_t nn = mem[(addr+1) & 0xFFFF] | (mem[(addr+2) & 0xFFFF] << 8);
            snprintf(buf, bufsize, "JP %s,$%04X", cc[y], nn);
            return 3 + prefix_len;
        }
        if (z == 3) {
            if (y == 0) {
                uint16_t nn = mem[(addr+1) & 0xFFFF] | (mem[(addr+2) & 0xFFFF] << 8);
                snprintf(buf, bufsize, "JP $%04X", nn);
                return 3 + prefix_len;
            }
            if (y == 2) {
                uint8_t n = mem[(addr + 1) & 0xFFFF];
                snprintf(buf, bufsize, "OUT ($%02X),A", n);
                return 2 + prefix_len;
            }
            if (y == 3) {
                uint8_t n = mem[(addr + 1) & 0xFFFF];
                snprintf(buf, bufsize, "IN A,($%02X)", n);
                return 2 + prefix_len;
            }
            if (y == 4) {
                if (ixiy) snprintf(buf, bufsize, "EX (SP),%s", ixiy);
                else snprintf(buf, bufsize, "EX (SP),HL");
                return 1 + prefix_len;
            }
            if (y == 5) { snprintf(buf, bufsize, "EX DE,HL"); return 1 + prefix_len; }
            if (y == 6) { snprintf(buf, bufsize, "DI"); return 1 + prefix_len; }
            if (y == 7) { snprintf(buf, bufsize, "EI"); return 1 + prefix_len; }
        }
        if (z == 4) {
            uint16_t nn = mem[(addr+1) & 0xFFFF] | (mem[(addr+2) & 0xFFFF] << 8);
            snprintf(buf, bufsize, "CALL %s,$%04X", cc[y], nn);
            return 3 + prefix_len;
        }
        if (z == 5) {
            if (q == 0) {
                if (ixiy && p == 2) snprintf(buf, bufsize, "PUSH %s", ixiy);
                else snprintf(buf, bufsize, "PUSH %s", r16af[p]);
                return 1 + prefix_len;
            }
            if (p == 0) {
                uint16_t nn = mem[(addr+1) & 0xFFFF] | (mem[(addr+2) & 0xFFFF] << 8);
                snprintf(buf, bufsize, "CALL $%04X", nn);
                return 3 + prefix_len;
            }
        }
        if (z == 6) {
            uint8_t n = mem[(addr + 1) & 0xFFFF];
            snprintf(buf, bufsize, "%s $%02X", alu[y], n);
            return 2 + prefix_len;
        }
        if (z == 7) {
            snprintf(buf, bufsize, "RST $%02X", y * 8);
            return 1 + prefix_len;
        }
    }

    snprintf(buf, bufsize, "DB $%02X", op);
    return 1 + prefix_len;
}
