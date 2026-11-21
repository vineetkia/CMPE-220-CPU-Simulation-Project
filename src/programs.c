/*
 * programs.c
 * Our two example programs, translated by hand.
 *
 * For each program we did three things ourselves, on paper:
 *   1. wrote it in C               (programs/add.c, programs/multiply.c)
 *   2. translated the C into our assembly language
 *   3. turned every assembly line into 16 bits, using the instruction table
 *      and the formats in isa.c (opcode first, then the register and address fields)
 *
 * The tables below are the result. There is no assembler and no loader:
 * put_in_ram() just copies the binary we typed into RAM, word 0 at address 0x00.
 * Address 0x00 is also where PC starts, so that is where the program begins.
 */
#include <stddef.h>
#include <string.h>

#include "programs.h"

/*
 * Program 1: add two numbers.
 *
 *     c = a + b;          with a = 5, b = 7
 *
 * How we encoded "LD R1, a" (a is at address 0x06):
 *     LD = opcode 1000, format M:  op | r | m | addr8
 *     1000 | 001 (R1) | 0 (fixed address) | 00000110 (0x06)
 */
static const ProgramWord ADD_WORDS[] = {
    /*  binary (by hand)        assembly (by hand)   C line                    */
    { "1000 001 0 00000110",   "LD R1, a",          "c = a + b;" },          /* 0x00 */
    { "1000 010 0 00000111",   "LD R2, b",          "" },                    /* 0x01 */
    { "0001 011 001 010 000",  "ADD R3, R1, R2",    "" },                    /* 0x02 */
    { "1001 011 0 00001000",   "ST R3, c",          "" },                    /* 0x03 */
    { "1111 011 0 00000000",   "OUT R3",            "printf(\"%d\\n\", c);" }, /* 0x04 */
    { "0000 000000000000",     "HALT",              "return 0;" },           /* 0x05 */
    { "0000000000000101",      "a = 5",             "int a = 5;" },          /* 0x06 */
    { "0000000000000111",      "b = 7",             "int b = 7;" },          /* 0x07 */
    { "0000000000000000",      "c = 0",             "int c;" },              /* 0x08 */
};

/*
 * Program 2: multiply two numbers using only addition.
 *
 *     while (b > 0) { result = result + a; b = b - 1; }      with a = 5, b = 3
 *
 * How we encoded "BLE done" (done is address 0x0A):
 *     BLE = opcode 1011 with condition 0110, format B:  op | cond | addr8
 *     1011 | 0110 | 00001010 (0x0A)
 */
static const ProgramWord MULTIPLY_WORDS[] = {
    /*  binary (by hand)        assembly (by hand)   C line                         */
    { "1000 001 0 00001100",   "LD R1, a",          "/* get a and b */" },        /* 0x00 */
    { "1000 010 0 00001101",   "LD R2, b",          "" },                         /* 0x01 */
    { "0111 011 000000000",    "LI R3, 0",          "result = 0;" },              /* 0x02 */
    { "1010 000 010 000 000",  "CMP R2, R0",        "while (b > 0) {" },          /* 0x03 loop */
    { "1011 0110 00001010",    "BLE done",          "" },                         /* 0x04 */
    { "0001 011 011 001 000",  "ADD R3, R3, R1",    "result = result + a;" },     /* 0x05 */
    { "1001 011 0 00001110",   "ST R3, result",     "" },                         /* 0x06 */
    { "0110 010 010 111111",   "ADDI R2, R2, -1",   "b = b - 1;" },               /* 0x07 */
    { "1001 010 0 00001101",   "ST R2, b",          "" },                         /* 0x08 */
    { "1011 0000 00000011",    "JMP loop",          "}" },                        /* 0x09 */
    { "1111 011 0 00000000",   "OUT R3",            "printf(\"%d\\n\", result);" }, /* 0x0A done */
    { "0000 000000000000",     "HALT",              "return 0;" },                /* 0x0B */
    { "0000000000000101",      "a = 5",             "int a = 5;" },               /* 0x0C */
    { "0000000000000011",      "b = 3",             "int b = 3;" },               /* 0x0D */
    { "0000000000000000",      "result = 0",        "int result = 0;" },          /* 0x0E */
};

const HandProgram PROGRAMS[] = {
    {
        "add", "Add two numbers", "programs/add.c",
        ADD_WORDS, sizeof ADD_WORDS / sizeof ADD_WORDS[0], 6,
        { { {"a", 0x06}, {"b", 0x07}, {"c", 0x08} }, 3 },
    },
    {
        "multiply", "Multiply two numbers using only ADD", "programs/multiply.c",
        MULTIPLY_WORDS, sizeof MULTIPLY_WORDS / sizeof MULTIPLY_WORDS[0], 12,
        { { {"loop", 0x03}, {"done", 0x0A}, {"a", 0x0C}, {"b", 0x0D}, {"result", 0x0E} }, 5 },
    },
};
const int PROGRAM_COUNT = sizeof PROGRAMS / sizeof PROGRAMS[0];

const HandProgram *find_program(const char *name)
{
    int i;
    for (i = 0; i < PROGRAM_COUNT; i++)
        if (strcmp(PROGRAMS[i].name, name) == 0)
            return &PROGRAMS[i];
    return NULL;
}

/* "0111 001 000000101" -> 0x7205. Spaces are only there to make the fields readable. */
uint16_t bin(const char *bits)
{
    unsigned value = 0;

    for (; *bits; bits++) {
        if (*bits == '0' || *bits == '1')
            value = (value << 1) | (unsigned)(*bits - '0');
    }
    return (uint16_t)(value & WORD_MASK);
}

void put_in_ram(Memory *ram, const HandProgram *program)
{
    int addr;

    for (addr = 0; addr < program->word_count && addr < MEM_SIZE; addr++)
        ram->words[addr] = bin(program->words[addr].bits);
}
