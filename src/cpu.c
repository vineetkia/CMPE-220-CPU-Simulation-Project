/*
 * cpu.c
 * Connects the parts from components.c and runs the instruction cycle.
 * Every phase takes one tick of the clock:
 *
 *   tick 1  FETCH    MAR <- PC, read memory over the bus into MDR, IR <- MDR, PC <- PC + 1
 *   tick 2  DECODE   the control unit splits IR into opcode, registers and immediate
 *   tick 3  EXECUTE  use the ALU, read/write memory over the bus, update registers
 *
 * cpu_step() runs one instruction and fills in a StepRecord, which is a
 * snapshot of the whole machine after that instruction.
 */
#include <stdio.h>
#include <string.h>

#include "cpu.h"

/* Power on: every register is 0 (so PC = 0x00, where the program starts),
 * RAM is empty and the clock is at 0. We then put the program into RAM by hand
 * (put_in_ram() in programs.c). */
void cpu_init(CPU *cpu)
{
    memset(cpu, 0, sizeof *cpu);
    sysbus_init(&cpu->bus, &cpu->memory, &cpu->out_port);
    cpu->alu.flags = &cpu->flags;
    register_init(&cpu->pc, "PC", 8);
    register_init(&cpu->ir, "IR", 16);
    register_init(&cpu->mar, "MAR", 8);
    register_init(&cpu->mdr, "MDR", 16);
}

/* Memory access always goes through MAR, MDR and the system bus. */
static unsigned mem_read(CPU *cpu, int addr, BusPhase phase)
{
    register_load(&cpu->mar, addr);
    register_load(&cpu->mdr, (int)sysbus_read(&cpu->bus, (int)cpu->mar.value, phase));
    return cpu->mdr.value;
}

static void mem_write(CPU *cpu, int addr, int value, StepRecord *rec)
{
    register_load(&cpu->mar, addr);
    register_load(&cpu->mdr, value);
    sysbus_write(&cpu->bus, (int)cpu->mar.value, (int)cpu->mdr.value, PHASE_EXECUTE);
    rec->mem_written[cpu->mar.value] = 1;
}

static void set_reg(CPU *cpu, int n, int value, StepRecord *rec)
{
    if (regfile_write(&cpu->regs, n, value))
        rec->reg_written[n] = 1;
}

/* Run the ALU on two registers: ADD, SUB, MUL, AND, OR. */
static void exec_alu(CPU *cpu, const Decoded *d, StepRecord *rec)
{
    int a = regfile_read(&cpu->regs, d->rs);
    int b = regfile_read(&cpu->regs, d->rt);
    unsigned result = alu_compute(&cpu->alu, d->op, a, b);
    char sign;

    set_reg(cpu, d->rd, (int)result, rec);
    switch (d->op) {
    case OP_ADD: sign = '+'; break;
    case OP_SUB: sign = '-'; break;
    case OP_MUL: sign = 'x'; break;
    case OP_AND: sign = '&'; break;
    default:     sign = '|'; break;
    }
    snprintf(rec->effect, sizeof rec->effect, "%s <- %d %c %d = %u",
             REG_NAMES[d->rd], a, sign, b, result);
}

/* LD and ST, with a fixed address or with [rb + imm5]. */
static void exec_load_store(CPU *cpu, const Decoded *d, const SymbolTable *symbols, StepRecord *rec)
{
    char where[MAX_NAME + 16], label[MAX_NAME];
    int addr;

    if (d->mode == 1) {
        addr = (regfile_read(&cpu->regs, d->rb) + d->imm5) & ADDR_MASK;
        snprintf(where, sizeof where, "M[0x%02X]", addr);
    } else {
        addr = d->addr8;
        addr_text(addr, symbols, label, sizeof label);
        if (strncmp(label, "0x", 2) != 0)
            snprintf(where, sizeof where, "M[0x%02X] (%s)", addr, label);
        else
            snprintf(where, sizeof where, "M[0x%02X]", addr);
    }

    if (d->op == OP_LD) {
        unsigned value = mem_read(cpu, addr, PHASE_EXECUTE);
        set_reg(cpu, d->rd, (int)value, rec);
        snprintf(rec->effect, sizeof rec->effect, "%s <- %s = %u", REG_NAMES[d->rd], where, value);
    } else {
        int value = regfile_read(&cpu->regs, d->rd);
        mem_write(cpu, addr, value, rec);
        snprintf(rec->effect, sizeof rec->effect, "%s <- %s = %d", where, REG_NAMES[d->rd], value);
    }
}

/* CALL, RET, PUSH and POP all use the stack. */
static void exec_stack(CPU *cpu, const Decoded *d, StepRecord *rec)
{
    int sp = regfile_read(&cpu->regs, SP);

    if (d->op == OP_CALL) {
        sp = (sp - 1) & WORD_MASK;
        set_reg(cpu, SP, sp, rec);
        mem_write(cpu, sp, (int)cpu->pc.value, rec);    /* return address goes on the stack */
        snprintf(rec->effect, sizeof rec->effect, "push return address 0x%02X, PC <- 0x%02X",
                 cpu->pc.value, d->addr8);
        register_load(&cpu->pc, d->addr8);
    } else if (d->op == OP_RET) {
        unsigned ret = mem_read(cpu, sp, PHASE_EXECUTE);
        set_reg(cpu, SP, sp + 1, rec);
        register_load(&cpu->pc, (int)ret);
        snprintf(rec->effect, sizeof rec->effect, "pop return address 0x%02X, PC <- 0x%02X", ret, ret);
    } else if (d->mode == 0) {      /* PUSH */
        int value;
        sp = (sp - 1) & WORD_MASK;
        set_reg(cpu, SP, sp, rec);
        value = regfile_read(&cpu->regs, d->rd);
        mem_write(cpu, sp, value, rec);
        snprintf(rec->effect, sizeof rec->effect, "SP <- 0x%02X, M[0x%02X] <- %s = %d",
                 sp, sp, REG_NAMES[d->rd], value);
    } else {                        /* POP */
        unsigned value = mem_read(cpu, sp, PHASE_EXECUTE);
        set_reg(cpu, d->rd, (int)value, rec);
        set_reg(cpu, SP, sp + 1, rec);
        snprintf(rec->effect, sizeof rec->effect, "%s <- M[0x%02X] = %u, SP <- 0x%02X",
                 REG_NAMES[d->rd], sp, value, sp + 1);
    }
}

int cpu_step(CPU *cpu, const SymbolTable *symbols, StepRecord *rec, char *err, size_t err_size)
{
    Decoded d;
    int pc;
    char bits[32];

    if (cpu->halted) {
        snprintf(err, err_size, "CPU is halted");
        return -1;
    }
    if (cpu->steps >= MAX_STEPS) {
        snprintf(err, err_size, "stopped after %d steps, maybe an endless loop?", MAX_STEPS);
        return -1;
    }

    memset(rec, 0, sizeof *rec);
    cpu->bus.log_count = 0;
    cpu->alu.last.used = 0;

    /* 1. FETCH (clock tick 1) */
    rec->clock[0] = clock_tick(&cpu->clock);
    pc = (int)cpu->pc.value;
    register_load(&cpu->ir, (int)mem_read(cpu, pc, PHASE_FETCH));
    register_load(&cpu->pc, pc + 1);
    snprintf(rec->fetch, sizeof rec->fetch, "MAR <- PC = %02X, RAM[%02X] -> MDR = %04X, IR <- MDR, PC <- %02X",
             pc, pc, cpu->mdr.value, cpu->pc.value);

    /* 2. DECODE (clock tick 2) */
    rec->clock[1] = clock_tick(&cpu->clock);
    d = control_decode(cpu->ir.value);
    disassemble((uint16_t)cpu->ir.value, symbols, rec->text, sizeof rec->text);
    snprintf(rec->decode, sizeof rec->decode, "IR = %s -> %s",
             binary_fields((uint16_t)cpu->ir.value, bits, sizeof bits), rec->text);

    /* 3. EXECUTE (clock tick 3) */
    rec->clock[2] = clock_tick(&cpu->clock);
    switch (d.op) {
    case OP_HALT:
        cpu->halted = 1;
        snprintf(rec->effect, sizeof rec->effect, "processor halted");
        break;

    case OP_ADD: case OP_SUB: case OP_MUL: case OP_AND: case OP_OR:
        exec_alu(cpu, &d, rec);
        break;

    case OP_ADDI: {
        int a = regfile_read(&cpu->regs, d.rs);
        unsigned result = alu_compute(&cpu->alu, d.op, a, d.imm6);
        set_reg(cpu, d.rd, (int)result, rec);
        if (d.imm6 >= 0)
            snprintf(rec->effect, sizeof rec->effect, "%s <- %d + %d = %u",
                     REG_NAMES[d.rd], a, d.imm6, result);
        else
            snprintf(rec->effect, sizeof rec->effect, "%s <- %d + (%d) = %u",
                     REG_NAMES[d.rd], a, d.imm6, result);
        break;
    }

    case OP_LI:
        set_reg(cpu, d.rd, d.imm9, rec);
        snprintf(rec->effect, sizeof rec->effect, "%s <- %d", REG_NAMES[d.rd], d.imm9);
        break;

    case OP_LD: case OP_ST:
        exec_load_store(cpu, &d, symbols, rec);
        break;

    case OP_CMP: {
        int a = regfile_read(&cpu->regs, d.rs);
        int b = regfile_read(&cpu->regs, d.rt);
        alu_compute(&cpu->alu, d.op, a, b);     /* only the flags are kept */
        snprintf(rec->effect, sizeof rec->effect, "compare %d with %d, set flags",
                 to_signed16(a), to_signed16(b));
        break;
    }

    case OP_BR:
        if (control_branch_taken(d.cond, &cpu->flags)) {
            register_load(&cpu->pc, d.addr8);
            snprintf(rec->effect, sizeof rec->effect, "PC <- 0x%02X (%s)",
                     d.addr8, d.cond == 0 ? "jump" : "branch taken");
        } else {
            snprintf(rec->effect, sizeof rec->effect, "branch not taken");
        }
        break;

    case OP_CALL: case OP_RET: case OP_STK:
        exec_stack(cpu, &d, rec);
        break;

    default: {      /* OP_OUT */
        int value = regfile_read(&cpu->regs, d.rd);
        sysbus_output(&cpu->bus, value, PHASE_EXECUTE);
        rec->has_output = 1;
        rec->output = (unsigned)value;
        snprintf(rec->effect, sizeof rec->effect, "output %s = %d", REG_NAMES[d.rd], value);
        break;
    }
    }

    cpu->steps++;

    /* take the snapshot */
    rec->step = cpu->steps;
    rec->pc = pc;
    rec->ir = cpu->ir.value;
    memcpy(rec->regs, cpu->regs.values, sizeof rec->regs);
    rec->flags = cpu->flags;
    rec->next_pc = (int)cpu->pc.value;
    rec->mar = cpu->mar.value;
    rec->mdr = cpu->mdr.value;
    memcpy(rec->mem, cpu->memory.words, sizeof rec->mem);
    rec->halted = cpu->halted;
    memcpy(rec->bus_log, cpu->bus.log, sizeof rec->bus_log);
    rec->bus_count = cpu->bus.log_count;
    rec->alu = cpu->alu.last;
    return 0;
}

int cpu_run(CPU *cpu, const SymbolTable *symbols, char *err, size_t err_size)
{
    StepRecord rec;

    while (!cpu->halted)
        if (cpu_step(cpu, symbols, &rec, err, err_size) != 0)
            return -1;
    return 0;
}
