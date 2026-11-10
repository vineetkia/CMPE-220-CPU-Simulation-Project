/*
 * isa.c
 * Everything about the instruction set lives here: register names, opcodes,
 * instruction formats, and the code that turns instructions into 16-bit
 * words and back. The assembler, the CPU and the report all use this file.
 */
#include <stdio.h>
#include <string.h>

#include "isa.h"

const char *const REG_NAMES[NUM_REGS] = { "R0", "R1", "R2", "R3", "R4", "R5", "R6", "SP" };

/* Branch condition codes go in bits 11..8 of a branch. The position in this
 * list is the code. For signed compares we look at N and V together, so it
 * still works on overflow. */
static const char *const COND_NAMES[] = {
    "JMP",  /* always */
    "BEQ",  /* Z = 1 */
    "BNE",  /* Z = 0 */
    "BLT",  /* N != V */
    "BGE",  /* N == V */
    "BGT",  /* Z = 0 and N == V */
    "BLE",  /* Z = 1 or N != V */
};
#define NUM_CONDS 7

/* The position in this list is the opcode. OP_BR and OP_STK have more than
 * one name, so decoded_mnemonic() handles those two by itself. */
static const char *const OP_NAMES[16] = {
    "HALT", "ADD", "SUB", "MUL", "AND", "OR", "ADDI", "LI",
    "LD", "ST", "CMP", "B", "CALL", "RET", "PUSH", "OUT"
};

/* Instruction formats: each one is a list of fields, starting from bit 15. */
static const Field F_R[]  = { {"op", 4}, {"rd", 3}, {"rs", 3}, {"rt", 3}, {"000", 3}, {NULL, 0} };
static const Field F_I[]  = { {"op", 4}, {"rd", 3}, {"rs", 3}, {"imm6", 6}, {NULL, 0} };
static const Field F_L[]  = { {"op", 4}, {"rd", 3}, {"imm9", 9}, {NULL, 0} };
static const Field F_M[]  = { {"op", 4}, {"r", 3}, {"m=0", 1}, {"addr8", 8}, {NULL, 0} };
static const Field F_MX[] = { {"op", 4}, {"r", 3}, {"m=1", 1}, {"rb", 3}, {"imm5", 5}, {NULL, 0} };
static const Field F_B[]  = { {"op", 4}, {"cond", 4}, {"addr8", 8}, {NULL, 0} };
static const Field F_S[]  = { {"op", 4}, {"r", 3}, {"s", 1}, {"00000000", 8}, {NULL, 0} };
static const Field F_N[]  = { {"op", 4}, {"000000000000", 12}, {NULL, 0} };

const char *const FORMAT_NAMES[FMT_COUNT] = { "R", "I", "L", "M", "MX", "B", "S", "N" };
const Field *const FORMATS[FMT_COUNT] = { F_R, F_I, F_L, F_M, F_MX, F_B, F_S, F_N };

/* The assembler's lookup table. To translate a line, the assembler finds its
 * mnemonic here, which gives the opcode bits and the format, and the format
 * says where the operands go. MOV and NOP are shortcuts handled by the
 * assembler itself (they become ADDI). */
const OpcodeEntry OPCODE_TABLE[] = {
    {"HALT", OP_HALT, FMT_N, 0},
    {"ADD",  OP_ADD,  FMT_R, 0},
    {"SUB",  OP_SUB,  FMT_R, 0},
    {"MUL",  OP_MUL,  FMT_R, 0},
    {"AND",  OP_AND,  FMT_R, 0},
    {"OR",   OP_OR,   FMT_R, 0},
    {"ADDI", OP_ADDI, FMT_I, 0},
    {"LI",   OP_LI,   FMT_L, 0},
    {"LD",   OP_LD,   FMT_M, 0},
    {"ST",   OP_ST,   FMT_M, 0},
    {"CMP",  OP_CMP,  FMT_R, 0},
    {"JMP",  OP_BR,   FMT_B, 0},
    {"BEQ",  OP_BR,   FMT_B, 1},
    {"BNE",  OP_BR,   FMT_B, 2},
    {"BLT",  OP_BR,   FMT_B, 3},
    {"BGE",  OP_BR,   FMT_B, 4},
    {"BGT",  OP_BR,   FMT_B, 5},
    {"BLE",  OP_BR,   FMT_B, 6},
    {"CALL", OP_CALL, FMT_B, 0},
    {"RET",  OP_RET,  FMT_N, 0},
    {"PUSH", OP_STK,  FMT_S, 0},
    {"POP",  OP_STK,  FMT_S, 1},
    {"OUT",  OP_OUT,  FMT_S, 0},
};
const int OPCODE_TABLE_SIZE = sizeof(OPCODE_TABLE) / sizeof(OPCODE_TABLE[0]);

const IsaRow ISA_TABLE[] = {
    {"HALT", OP_HALT, "N", "HALT", "stop the CPU", "-"},
    {"ADD", OP_ADD, "R", "ADD rd, rs, rt", "rd = rs + rt", "ZNCV"},
    {"SUB", OP_SUB, "R", "SUB rd, rs, rt", "rd = rs - rt", "ZNCV"},
    {"MUL", OP_MUL, "R", "MUL rd, rs, rt", "rd = rs * rt (low 16 bits)", "ZNCV"},
    {"AND", OP_AND, "R", "AND rd, rs, rt", "rd = rs & rt", "ZN"},
    {"OR", OP_OR, "R", "OR rd, rs, rt", "rd = rs | rt", "ZN"},
    {"ADDI", OP_ADDI, "I", "ADDI rd, rs, imm6", "rd = rs + imm6", "ZNCV"},
    {"LI", OP_LI, "L", "LI rd, imm9", "rd = imm9", "-"},
    {"LD", OP_LD, "M/MX", "LD r, addr | LD r, [rb+imm5]", "r = M[address]", "-"},
    {"ST", OP_ST, "M/MX", "ST r, addr | ST r, [rb+imm5]", "M[address] = r", "-"},
    {"CMP", OP_CMP, "R", "CMP rs, rt", "set flags from rs - rt", "ZNCV"},
    {"Bcc", OP_BR, "B", "JMP|BEQ|BNE|BLT|BGE|BGT|BLE addr", "if condition: PC = addr", "-"},
    {"CALL", OP_CALL, "B", "CALL addr", "push PC, then PC = addr", "-"},
    {"RET", OP_RET, "N", "RET", "pop PC from the stack", "-"},
    {"PUSH/POP", OP_STK, "S", "PUSH r | POP r", "put r on / take r off the stack", "-"},
    {"OUT", OP_OUT, "S", "OUT r", "print r", "-"},
};
const int ISA_TABLE_SIZE = sizeof(ISA_TABLE) / sizeof(ISA_TABLE[0]);


/* Read the low `bits` bits of value as a signed (two's complement) number. */
int sign_extend(int value, int bits)
{
    value &= (1 << bits) - 1;
    if (value & (1 << (bits - 1)))
        value -= 1 << bits;
    return value;
}

int to_signed16(int value)
{
    return sign_extend(value, 16);
}

int fits_signed(int value, int bits)
{
    return value >= -(1 << (bits - 1)) && value < (1 << (bits - 1));
}

const OpcodeEntry *lookup_mnemonic(const char *mnemonic)
{
    int i;
    for (i = 0; i < OPCODE_TABLE_SIZE; i++)
        if (strcmp(OPCODE_TABLE[i].mnemonic, mnemonic) == 0)
            return &OPCODE_TABLE[i];
    return NULL;
}

int branch_cond(const char *name)
{
    const OpcodeEntry *entry = lookup_mnemonic(name);
    if (entry && entry->opcode == OP_BR)
        return entry->extra;
    return -1;
}


/* ----- building instruction words from their fields ----- */

uint16_t enc_r(int op, int rd, int rs, int rt)
{
    return (uint16_t)((op << 12) | (rd << 9) | (rs << 6) | (rt << 3));
}

uint16_t enc_i(int op, int rd, int rs, int imm6)
{
    return (uint16_t)((op << 12) | (rd << 9) | (rs << 6) | (imm6 & 0x3F));
}

uint16_t enc_l(int op, int rd, int imm9)
{
    return (uint16_t)((op << 12) | (rd << 9) | (imm9 & 0x1FF));
}

uint16_t enc_m_abs(int op, int r, int addr8)
{
    return (uint16_t)((op << 12) | (r << 9) | (addr8 & 0xFF));
}

uint16_t enc_m_ind(int op, int r, int rb, int imm5)
{
    return (uint16_t)((op << 12) | (r << 9) | (1 << 8) | (rb << 5) | (imm5 & 0x1F));
}

uint16_t enc_b(int op, int cond, int addr8)
{
    return (uint16_t)((op << 12) | (cond << 8) | (addr8 & 0xFF));
}

uint16_t enc_s(int op, int r, int s)
{
    return (uint16_t)((op << 12) | (r << 9) | (s << 8));
}


/* ----- splitting an instruction word back into fields ----- */

Decoded decode(uint16_t word)
{
    Decoded d;
    d.word = word;
    d.op   = (word >> 12) & 0xF;
    d.rd   = (word >> 9) & 0x7;
    d.rs   = (word >> 6) & 0x7;
    d.rt   = (word >> 3) & 0x7;
    d.imm6 = sign_extend(word, 6);
    d.imm9 = sign_extend(word, 9);
    d.mode = (word >> 8) & 0x1;
    d.addr8 = word & 0xFF;
    d.rb   = (word >> 5) & 0x7;
    d.imm5 = sign_extend(word, 5);
    d.cond = (word >> 8) & 0xF;
    return d;
}

Format decoded_format(const Decoded *d)
{
    switch (d->op) {
    case OP_ADD: case OP_SUB: case OP_MUL: case OP_AND: case OP_OR: case OP_CMP:
        return FMT_R;
    case OP_ADDI:
        return FMT_I;
    case OP_LI:
        return FMT_L;
    case OP_LD: case OP_ST:
        return d->mode ? FMT_MX : FMT_M;
    case OP_BR: case OP_CALL:
        return FMT_B;
    case OP_STK: case OP_OUT:
        return FMT_S;
    default:
        return FMT_N;
    }
}

const char *decoded_mnemonic(const Decoded *d, char *buf, size_t size)
{
    if (d->op == OP_BR) {
        if (d->cond < NUM_CONDS)
            snprintf(buf, size, "%s", COND_NAMES[d->cond]);
        else
            snprintf(buf, size, "B?%d", d->cond);
    } else if (d->op == OP_STK) {
        snprintf(buf, size, "%s", d->mode ? "POP" : "PUSH");
    } else {
        snprintf(buf, size, "%s", OP_NAMES[d->op]);
    }
    return buf;
}

/* Show an address using its label if it has one, else as hex. */
const char *addr_text(int addr, const SymbolTable *symbols, char *buf, size_t size)
{
    if (symbols) {
        int i;
        for (i = 0; i < symbols->count; i++) {
            if (symbols->items[i].value == addr) {
                snprintf(buf, size, "%s", symbols->items[i].name);
                return buf;
            }
        }
    }
    snprintf(buf, size, "0x%02X", addr);
    return buf;
}

/* Turn a 16-bit word back into assembly text, like "MUL R1, R1, R2". */
const char *disassemble(uint16_t word, const SymbolTable *symbols, char *buf, size_t size)
{
    Decoded d = decode(word);
    char m[16], where[MAX_NAME + 8], offset[16] = "";

    decoded_mnemonic(&d, m, sizeof m);
    switch (decoded_format(&d)) {
    case FMT_R:
        if (d.op == OP_CMP)
            snprintf(buf, size, "CMP %s, %s", REG_NAMES[d.rs], REG_NAMES[d.rt]);
        else
            snprintf(buf, size, "%s %s, %s, %s", m, REG_NAMES[d.rd], REG_NAMES[d.rs], REG_NAMES[d.rt]);
        break;
    case FMT_I:
        snprintf(buf, size, "ADDI %s, %s, %d", REG_NAMES[d.rd], REG_NAMES[d.rs], d.imm6);
        break;
    case FMT_L:
        snprintf(buf, size, "LI %s, %d", REG_NAMES[d.rd], d.imm9);
        break;
    case FMT_M:
        addr_text(d.addr8, symbols, where, sizeof where);
        snprintf(buf, size, "%s %s, %s", m, REG_NAMES[d.rd], where);
        break;
    case FMT_MX:
        if (d.imm5 != 0)
            snprintf(offset, sizeof offset, "%+d", d.imm5);
        snprintf(buf, size, "%s %s, [%s%s]", m, REG_NAMES[d.rd], REG_NAMES[d.rb], offset);
        break;
    case FMT_B:
        addr_text(d.addr8, symbols, where, sizeof where);
        snprintf(buf, size, "%s %s", m, where);
        break;
    case FMT_S:
        snprintf(buf, size, "%s %s", m, REG_NAMES[d.rd]);
        break;
    default:
        snprintf(buf, size, "%s", m);   /* HALT or RET */
        break;
    }
    return buf;
}

/* The word in binary, all 16 bits together. */
const char *binary_plain(uint16_t word, char *buf, size_t size)
{
    int bit;
    if (size < 17)
        return "";
    for (bit = 15; bit >= 0; bit--)
        buf[15 - bit] = ((word >> bit) & 1) ? '1' : '0';
    buf[16] = '\0';
    return buf;
}

/* The word in binary with a space between fields, like "0011 001 001 010 000". */
const char *binary_fields(uint16_t word, char *buf, size_t size)
{
    Decoded d = decode(word);
    const Field *field = FORMATS[decoded_format(&d)];
    char bits[17];
    size_t out = 0;
    int pos = 0;

    if (size < 24)
        return "";
    binary_plain(word, bits, sizeof bits);
    for (; field->name; field++) {
        if (pos > 0)
            buf[out++] = ' ';
        memcpy(buf + out, bits + pos, (size_t)field->width);
        out += (size_t)field->width;
        pos += field->width;
    }
    buf[out] = '\0';
    return buf;
}
