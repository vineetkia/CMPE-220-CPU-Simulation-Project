/*
 * cpu.h
 * The SPARTAN-16 CPU. It connects the parts from components.h and runs the
 * instruction cycle (fetch, decode, execute), one clock tick per phase.
 */
#ifndef CPU_H
#define CPU_H

#include "components.h"

/* Everything about the machine after one instruction ran.
 * The trace printer and the report are built from these. */
typedef struct {
    int step;                   /* 1 for the first instruction, 2 for the next ... */
    unsigned long clock[3];     /* the clock tick of the fetch, decode and execute */
    char fetch[96];             /* what the fetch did */
    char decode[96];            /* what the decode found */
    char effect[96];            /* what the execute did, like "R1 <- 1 x 3 = 3" */
    int pc;                     /* address the instruction came from */
    unsigned ir;
    char text[MAX_NAME + 32];   /* the instruction as text, like "ADD R3, R1, R2" */
    uint16_t regs[NUM_REGS];
    Flags flags;
    int next_pc;
    unsigned mar, mdr;
    uint16_t mem[MEM_SIZE];
    char mem_written[MEM_SIZE]; /* 1 for each word this instruction wrote */
    char reg_written[NUM_REGS]; /* 1 for each register this instruction wrote */
    int has_output;
    unsigned output;
    int halted;
    BusTransfer bus_log[MAX_TRANSFERS];
    int bus_count;
    AluRecord alu;
} StepRecord;

/* The whole computer. */
typedef struct {
    /* special purpose registers */
    Register pc, ir, mar, mdr;
    Flags flags;
    /* general purpose registers R0..R7 */
    RegisterFile regs;
    /* the rest of the CPU */
    ALU alu;
    Clock clock;
    /* outside the CPU, connected by the system bus */
    SystemBus bus;
    Memory memory;
    OutputPort out_port;

    int halted;
    int steps;
} CPU;

/* Power on: all registers 0, RAM empty, clock at 0. */
void cpu_init(CPU *cpu);

/* Run one instruction (fetch, decode, execute = 3 clock ticks). Returns 0 if it
 * worked. On a problem it returns -1 and writes the reason into err. */
int cpu_step(CPU *cpu, const SymbolTable *symbols, StepRecord *rec, char *err, size_t err_size);

/* Run until HALT without keeping the records. */
int cpu_run(CPU *cpu, const SymbolTable *symbols, char *err, size_t err_size);

#endif
