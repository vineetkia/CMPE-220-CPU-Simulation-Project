/*
 * isa.h
 * The SPARTAN-16 instruction set.
 *
 * Quick summary of the machine:
 *   - 16-bit words, 256 words of RAM (so addresses are 8 bits)
 *   - 8 general purpose registers R0..R7. R0 is always 0. R7 is the stack pointer (SP)
 *   - special purpose registers: PC, IR, MAR, MDR and the flags Z N C V
 *   - every instruction is exactly one 16-bit word
 */
#ifndef ISA_H
#define ISA_H

#include <stddef.h>
#include <stdint.h>

#define MEM_SIZE  256
#define WORD_MASK 0xFFFF
#define ADDR_MASK 0xFF
#define NUM_REGS  8
#define SP        7         /* R7 is the stack pointer */

#define MAX_NAME    32      /* longest label name */
#define MAX_SYMBOLS 16      /* most labels in one program */

/* Opcodes go in bits 15..12 of every instruction. */
enum {
    OP_HALT, OP_ADD, OP_SUB, OP_MUL,
    OP_AND,  OP_OR,  OP_ADDI, OP_LI,
    OP_LD,   OP_ST,  OP_CMP,
    OP_BR,          /* all the jumps and branches, condition is in bits 11..8 */
    OP_CALL, OP_RET,
    OP_STK,         /* PUSH when bit 8 is 0, POP when bit 8 is 1 */
    OP_OUT
};

/* The instruction formats. FORMATS[] in isa.c lists the fields of each one. */
typedef enum { FMT_R, FMT_I, FMT_L, FMT_M, FMT_MX, FMT_B, FMT_S, FMT_N, FMT_COUNT } Format;

typedef struct {
    const char *name;
    int width;
} Field;

/* One row of the instruction table printed by "./spartan isa". */
typedef struct {
    const char *name;
    int opcode;
    const char *format;
    const char *syntax;
    const char *what;
    const char *flags;
} IsaRow;

/* A name for an address, like "a" for 0x06. Used to make the trace easier to read. */
typedef struct {
    char name[MAX_NAME];
    int value;
} Symbol;

typedef struct {
    Symbol items[MAX_SYMBOLS];
    int count;
} SymbolTable;

/* All the fields of one instruction word. This is what the control unit looks at. */
typedef struct {
    uint16_t word;
    int op, rd, rs, rt;
    int imm6, imm9, imm5;
    int mode;           /* LD/ST addressing mode, or PUSH/POP */
    int addr8;
    int rb;
    int cond;
} Decoded;

extern const char *const REG_NAMES[NUM_REGS];
extern const char *const FORMAT_NAMES[FMT_COUNT];
extern const Field *const FORMATS[FMT_COUNT];
extern const IsaRow ISA_TABLE[];
extern const int ISA_TABLE_SIZE;

/* Small number helpers */
int sign_extend(int value, int bits);
int to_signed16(int value);

/* Splitting an instruction word into its fields */
Decoded decode(uint16_t word);
Format decoded_format(const Decoded *d);
const char *decoded_mnemonic(const Decoded *d, char *buf, size_t size);
const char *cond_name(int cond);    /* "JMP", "BEQ", ... or "?" */

/* Text helpers. symbols can be NULL. */
const char *addr_text(int addr, const SymbolTable *symbols, char *buf, size_t size);
const char *disassemble(uint16_t word, const SymbolTable *symbols, char *buf, size_t size);
const char *binary_fields(uint16_t word, char *buf, size_t size);   /* "0011 001 001 010 000" */
const char *binary_plain(uint16_t word, char *buf, size_t size);    /* "0011001001010000" */
const char *opcode_bits(int opcode, char *buf, size_t size);       /* "0011" */

#endif
