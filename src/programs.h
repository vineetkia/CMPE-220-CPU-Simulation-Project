/*
 * programs.h
 * Our two example programs, translated from C into binary by hand.
 */
#ifndef PROGRAMS_H
#define PROGRAMS_H

#include "components.h"

/* One word of a program: the binary we wrote by hand, and where it came from. */
typedef struct {
    const char *bits;       /* the 16 bits, with spaces between the fields */
    const char *assembly;   /* our hand translation into assembly */
    const char *c_line;     /* the C line this word comes from ("" = same as above) */
} ProgramWord;

typedef struct {
    const char *name;           /* short name used on the command line, like "add" */
    const char *title;          /* like "Add two numbers" */
    const char *c_file;         /* the C program we started from */
    const ProgramWord *words;   /* word i goes into RAM address i */
    int word_count;
    int code_count;             /* words 0 .. code_count-1 are instructions, the rest is data */
    SymbolTable names;          /* names for addresses, like a = 0x06 */
} HandProgram;

extern const HandProgram PROGRAMS[];
extern const int PROGRAM_COUNT;

/* Find a program by name ("add" or "multiply"). Returns NULL if there is none. */
const HandProgram *find_program(const char *name);

/* Turn a string like "0111 001 000000101" into the number it stands for. */
uint16_t bin(const char *bits);

/* Put the program's binary into RAM by hand: word i goes to address i. */
void put_in_ram(Memory *ram, const HandProgram *program);

#endif
