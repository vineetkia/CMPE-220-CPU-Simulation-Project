/*
 * loader.h
 * The loader puts an assembled program into RAM before the CPU starts.
 */
#ifndef LOADER_H
#define LOADER_H

#include "cpu.h"

/* What the system bus carried for one word the loader wrote. */
typedef struct {
    int addr;               /* value on the address bus */
    uint16_t word;          /* value on the data bus */
    const char *control;    /* signal on the control bus (MEM WRITE) */
} LoadStep;

/* Write words[0..count-1] into RAM at addresses 0x00, 0x01, ... one word at a
 * time over the system bus. If steps is not NULL, each write is recorded there
 * (steps needs room for count entries). Returns the number of words written. */
int load_program(CPU *cpu, const uint16_t *words, int count, LoadStep *steps);

#endif
