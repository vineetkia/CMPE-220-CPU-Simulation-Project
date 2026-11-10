/*
 * cpu.h
 * The SPARTAN-16 CPU. It connects the parts from components.h and runs the
 * instruction cycle (fetch, decode, execute).
 */
#ifndef CPU_H
#define CPU_H

#include "components.h"

/* Everything about the machine after one instruction ran.
 * The trace printer and the report are built from these. */
typedef struct {
    int step;
    int pc;                     /* address the instruction came from */
    unsigned ir;
    char text[MAX_NAME + 32];   /* the instruction as text, like "MUL R1, R1, R2" */
    char effect[MAX_NAME + 96]; /* what it did, like "R1 <- 1 x 3 = 3" */
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
    int cycles;                 /* clock cycles this instruction took */
} StepRecord;

typedef struct {
    /* the parts of the computer */
    Memory memory;
    OutputPort out_port;
    SystemBus bus;
    RegisterFile regs;
    Flags flags;
    ALU alu;
    Register pc, ir, mar, mdr;

    int halted;
    int steps;
    int cycles;
} CPU;

/* Power on: all registers 0, RAM empty. Use load_program() (loader.h) next. */
void cpu_init(CPU *cpu);

/* Run one instruction. Returns 0 if it worked. On a problem it returns -1
 * and writes the reason into err. */
int cpu_step(CPU *cpu, const SymbolTable *symbols, StepRecord *rec, char *err, size_t err_size);

/* Run until HALT without keeping the records. */
int cpu_run(CPU *cpu, const SymbolTable *symbols, char *err, size_t err_size);

#endif
