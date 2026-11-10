/*
 * assembler.h
 * The SPARTAN-16 assembler. See assembler.c for the language rules.
 */
#ifndef ASSEMBLER_H
#define ASSEMBLER_H

#include <stdio.h>

#include "isa.h"

typedef enum { KIND_CODE, KIND_DATA, KIND_STACK } WordKind;

/* One source line, for the listing. addr is -1 if the line makes no words. */
typedef struct {
    int addr;
    int word_count;
    char *text;
} ListingLine;

/* What the assembler gives back. */
typedef struct {
    uint16_t words[MEM_SIZE];   /* the program in memory, starting at address 0 */
    int word_count;
    WordKind kinds[MEM_SIZE];   /* code, data or stack for each word */
    SymbolTable symbols;        /* every label */
    SymbolTable variables;      /* the label of each .word, shown in the trace */
    ListingLine *listing;
    int listing_count;
    char source_name[256];
} Program;

/* Assemble source text into *prog. Returns 0 if it worked. On a problem it
 * returns -1 and writes the reason into err. Call program_free() when done. */
int assemble(const char *source, const char *name, Program *prog, char *err, size_t err_size);
void program_free(Program *prog);

const char *kind_name(WordKind kind);

/* Print the address, hex, binary (split into fields) and source of every line. */
void print_listing(FILE *out, const Program *prog);

#endif
