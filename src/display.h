/*
 * display.h
 * Prints the machine state in the terminal.
 */
#ifndef DISPLAY_H
#define DISPLAY_H

#include <stdio.h>

#include "assembler.h"
#include "cpu.h"
#include "loader.h"

#define ROW 8   /* words shown per memory row */

/* We show memory from address 0 to the end of the program, in full rows of 8. */
int window_end(const Program *prog);

/* Phase 1: how the assembler translated each line, using the ISA lookup table. */
void print_assemble(FILE *out, const Program *prog);

/* Phase 2: each word the loader wrote into RAM over the system bus. */
void print_load(FILE *out, const Program *prog, const CPU *cpu, const LoadStep *steps, int count, int color);

/* Phase 3: the machine before the first instruction, then after each one. */
void print_initial(FILE *out, const Program *prog, const CPU *cpu, int color);
void print_step(FILE *out, const Program *prog, const StepRecord *rec, int color);

#endif
