/*
 * components.h
 * The hardware parts of the SPARTAN-16 CPU.
 *
 * Every type in this file is one box from the CPU diagram in our report:
 *
 *   Register      one register (PC, IR, MAR, MDR)
 *   RegisterFile  the 8 general registers R0..R7
 *   Flags         the status flags Z N C V
 *   ALU           does add, subtract, multiply, AND, OR
 *   Bus           one bus (a group of wires holding a value)
 *   SystemBus     address bus + data bus + control bus
 *   Memory        main memory (RAM)
 *   OutputPort    the output device (prints numbers)
 *   Clock         the CPU clock: one tick for each fetch, decode and execute
 *   control_*()   the control unit: decodes instructions, checks branch conditions
 *
 * cpu.c wires these parts together.
 */
#ifndef COMPONENTS_H
#define COMPONENTS_H

#include "isa.h"

#define MAX_STEPS   10000   /* we stop a program after this many instructions */
#define MAX_TRANSFERS 4     /* most bus transfers one instruction can make */

/* A single register, for example PC (8 bits) or IR (16 bits). */
typedef struct {
    const char *name;
    int bits;
    unsigned value;
} Register;

void register_init(Register *r, const char *name, int bits);
void register_load(Register *r, int value);

/* R0..R7. R0 always reads as 0. R7 is also used as the stack pointer (SP). */
typedef struct {
    uint16_t values[NUM_REGS];
} RegisterFile;

int  regfile_read(const RegisterFile *rf, int n);
int  regfile_write(RegisterFile *rf, int n, int value);   /* returns 0 for R0 */

/* Z = result was zero, N = result negative, C = carry/borrow, V = signed overflow. */
typedef struct {
    int z, n, c, v;
} Flags;

/* What the ALU did during the last instruction, for the trace. */
typedef struct {
    int used;
    const char *op;
    unsigned a, b, result;
} AluRecord;

/* Arithmetic Logic Unit. It gets two 16-bit inputs A and B plus an operation
 * from the control unit, gives back the result and updates the flags. */
typedef struct {
    Flags *flags;
    AluRecord last;
} ALU;

unsigned alu_compute(ALU *alu, int op, int a, int b);

/* One bus. Whatever is put on it can be read by every part connected to it. */
typedef struct {
    const char *name;
    int bits;
    unsigned value;
} Bus;

void bus_put(Bus *bus, int value);

/* Main memory: 256 words, 16 bits each. Holds both the program and its data. */
typedef struct {
    uint16_t words[MEM_SIZE];
} Memory;

/* A very simple output device. OUT sends a number here and it gets printed. */
typedef struct {
    unsigned values[MAX_STEPS];
    int count;
} OutputPort;

/* One transfer on the system bus. */
typedef enum { PHASE_FETCH, PHASE_EXECUTE } BusPhase;
typedef enum { XFER_READ, XFER_WRITE, XFER_OUT } XferKind;

typedef struct {
    BusPhase phase;
    XferKind kind;
    unsigned addr;
    unsigned data;
} BusTransfer;

/* The three buses between the CPU and the outside world:
 *   address bus (8 bits)   which memory word we want
 *   data bus (16 bits)     the value going to or coming from memory
 *   control bus            what to do: MEM READ, MEM WRITE or IO WRITE */
typedef struct {
    Bus address;
    Bus data;
    const char *control;
    Memory *memory;
    OutputPort *out_port;
    BusTransfer log[MAX_TRANSFERS];   /* transfers made by the current instruction */
    int log_count;
} SystemBus;

void     sysbus_init(SystemBus *bus, Memory *memory, OutputPort *out_port);
unsigned sysbus_read(SystemBus *bus, int addr, BusPhase phase);
void     sysbus_write(SystemBus *bus, int addr, int value, BusPhase phase);
void     sysbus_output(SystemBus *bus, int value, BusPhase phase);

/* The CPU clock. Each tick moves the CPU one step through the instruction cycle:
 * tick 1 = fetch, tick 2 = decode, tick 3 = execute, then the next instruction. */
typedef struct {
    unsigned long cycle;    /* how many ticks so far */
} Clock;

unsigned long clock_tick(Clock *clock);    /* one tick, returns the new cycle number */

/* The control unit */
Decoded control_decode(unsigned ir_value);
int     control_branch_taken(int cond, const Flags *flags);

#endif
