/*
 * loader.c
 * The loader. After the assembler has made the machine code, the loader puts
 * it into RAM so the CPU can run it.
 *
 * It does not just copy an array. Like every other memory write in our
 * simulation, each word goes over the system bus:
 *     address bus  <- where the word goes (0x00, 0x01, ...)
 *     data bus     <- the 16-bit word
 *     control bus  <- MEM WRITE
 * and the memory stores it. The program always starts at address 0x00, which
 * is also where PC points after power on, so 0x00 is the entry point.
 */
#include "loader.h"

int load_program(CPU *cpu, const uint16_t *words, int count, LoadStep *steps)
{
    int addr;

    if (count > MEM_SIZE)
        count = MEM_SIZE;
    for (addr = 0; addr < count; addr++) {
        cpu->bus.log_count = 0;
        sysbus_write(&cpu->bus, addr, words[addr], PHASE_LOAD);
        if (steps) {
            steps[addr].addr = (int)cpu->bus.address.value;
            steps[addr].word = (uint16_t)cpu->bus.data.value;
            steps[addr].control = cpu->bus.control;
        }
    }

    /* loading is done: the bus goes quiet and the CPU starts from PC = 0x00 */
    cpu->bus.log_count = 0;
    cpu->bus.control = "idle";
    return count;
}
