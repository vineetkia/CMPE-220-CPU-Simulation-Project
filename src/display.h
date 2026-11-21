/*
 * display.h
 * Prints the program and the machine state in the terminal.
 */
#ifndef DISPLAY_H
#define DISPLAY_H

#include <stdio.h>

#include "cpu.h"
#include "programs.h"

/* Step 1 to 3: the C program, our hand translation, and RAM after we put the binary in. */
void print_program(FILE *out, const HandProgram *prog, const char *c_source, const CPU *cpu, int color);

/* After each instruction: the clock ticks, the CPU contents and the RAM. */
void print_step(FILE *out, const HandProgram *prog, const StepRecord *rec, int color);

/* The RAM block as rows of: address, binary, hex, ASCII, what it is.
 * pc marks the next instruction (-1 for none), written marks words that just changed. */
void print_ram(FILE *out, const HandProgram *prog, const uint16_t *mem, int pc, const char *written, int color);

/* ASCII of the two bytes of a word, '.' for bytes that can't be printed. */
void word_ascii(uint16_t word, char out[3]);

/* ASCII of a register value: 1 character for an 8-bit register, 2 for a 16-bit one. */
void value_ascii(unsigned value, int bits, char out[3]);

/* What a RAM word is: the instruction for code, "name = value" for data. */
const char *word_content(const HandProgram *prog, int addr, uint16_t word, char *buf, size_t size);

#endif
