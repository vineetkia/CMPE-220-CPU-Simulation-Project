/*
 * components.c
 * The code for each hardware part. See components.h for the list of parts.
 */
#include <stdio.h>

#include "components.h"

/* ----- registers ----- */

void register_init(Register *r, const char *name, int bits)
{
    r->name = name;
    r->bits = bits;
    r->value = 0;
}

void register_load(Register *r, int value)
{
    /* keep only as many bits as the register has */
    r->value = (unsigned)value & ((1u << r->bits) - 1);
}

int regfile_read(const RegisterFile *rf, int n)
{
    if (n == 0)
        return 0;
    return rf->values[n];
}

/* Write a register. Returns 0 for R0 because R0 can't change. */
int regfile_write(RegisterFile *rf, int n, int value)
{
    if (n == 0)
        return 0;
    rf->values[n] = (uint16_t)(value & WORD_MASK);
    return 1;
}


/* ----- ALU ----- */

unsigned alu_compute(ALU *alu, int op, int a_in, int b_in)
{
    uint32_t a = (uint32_t)a_in & WORD_MASK;
    uint32_t b = (uint32_t)b_in & WORD_MASK;
    uint32_t full, result;
    int carry = 0, overflow = 0;
    const char *name;

    switch (op) {
    case OP_ADD:
    case OP_ADDI:
        name = "ADD";
        full = a + b;
        result = full & WORD_MASK;
        carry = full > WORD_MASK;
        /* overflow: both inputs have the same sign but the result doesn't */
        overflow = ((a ^ result) & (b ^ result) & 0x8000) != 0;
        break;
    case OP_SUB:
    case OP_CMP:
        name = "SUB";
        result = (a - b) & WORD_MASK;
        carry = a < b;      /* we use C as "borrow" for subtraction */
        overflow = ((a ^ b) & (a ^ result) & 0x8000) != 0;
        break;
    case OP_MUL:
        name = "MUL";
        full = a * b;
        result = full & WORD_MASK;
        carry = overflow = full > WORD_MASK;
        break;
    case OP_AND:
        name = "AND";
        result = a & b;
        break;
    default:
        name = "OR";
        result = a | b;
        break;
    }

    alu->flags->z = result == 0;
    alu->flags->n = (result & 0x8000) != 0;
    alu->flags->c = carry;
    alu->flags->v = overflow;

    alu->last.used = 1;
    alu->last.op = name;
    alu->last.a = a;
    alu->last.b = b;
    alu->last.result = result;
    return result;
}


/* ----- buses ----- */

void bus_put(Bus *bus, int value)
{
    bus->value = (unsigned)value & ((1u << bus->bits) - 1);
}

static void log_transfer(SystemBus *bus, BusPhase phase, XferKind kind, unsigned addr, unsigned data)
{
    if (bus->log_count < MAX_TRANSFERS) {
        BusTransfer *t = &bus->log[bus->log_count++];
        t->phase = phase;
        t->kind = kind;
        t->addr = addr;
        t->data = data;
    }
}

void sysbus_init(SystemBus *bus, Memory *memory, OutputPort *out_port)
{
    bus->address.name = "address";
    bus->address.bits = 8;
    bus->address.value = 0;
    bus->data.name = "data";
    bus->data.bits = 16;
    bus->data.value = 0;
    bus->control = "idle";
    bus->memory = memory;
    bus->out_port = out_port;
    bus->log_count = 0;
}

unsigned sysbus_read(SystemBus *bus, int addr, BusPhase phase)
{
    bus_put(&bus->address, addr);
    bus->control = "MEM READ";
    bus_put(&bus->data, bus->memory->words[bus->address.value]);
    log_transfer(bus, phase, XFER_READ, bus->address.value, bus->data.value);
    return bus->data.value;
}

void sysbus_write(SystemBus *bus, int addr, int value, BusPhase phase)
{
    bus_put(&bus->address, addr);
    bus_put(&bus->data, value);
    bus->control = "MEM WRITE";
    bus->memory->words[bus->address.value] = (uint16_t)bus->data.value;
    log_transfer(bus, phase, XFER_WRITE, bus->address.value, bus->data.value);
}

void sysbus_output(SystemBus *bus, int value, BusPhase phase)
{
    bus_put(&bus->data, value);
    bus->control = "IO WRITE";
    if (bus->out_port->count < MAX_STEPS)
        bus->out_port->values[bus->out_port->count++] = bus->data.value;
    log_transfer(bus, phase, XFER_OUT, 0, bus->data.value);
}


/* ----- control unit ----- */

Decoded control_decode(unsigned ir_value)
{
    return decode((uint16_t)ir_value);
}

int control_branch_taken(int cond, const Flags *flags)
{
    int less = flags->n != flags->v;    /* signed "less than" */

    switch (cond) {
    case 0: return 1;                           /* JMP */
    case 1: return flags->z == 1;               /* BEQ */
    case 2: return flags->z == 0;               /* BNE */
    case 3: return less;                        /* BLT */
    case 4: return !less;                       /* BGE */
    case 5: return flags->z == 0 && !less;      /* BGT */
    case 6: return flags->z == 1 || less;       /* BLE */
    default: return 0;
    }
}
