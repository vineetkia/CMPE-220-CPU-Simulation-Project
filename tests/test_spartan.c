/*
 * test_spartan.c
 * Tests for the CPU simulator and our hand-translated programs.
 * Run them with:  make test
 */
#define _POSIX_C_SOURCE 200809L   /* for popen() */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpu.h"
#include "display.h"
#include "programs.h"

static int checks = 0, failures = 0;

#define CHECK(cond, ...)                                              \
    do {                                                              \
        checks++;                                                     \
        if (!(cond)) {                                                \
            failures++;                                               \
            printf("  FAIL %s:%d: ", __FILE__, __LINE__);             \
            printf(__VA_ARGS__);                                      \
            printf("\n");                                             \
        }                                                             \
    } while (0)

/* Put some words into RAM by hand and run them until HALT. */
static int run_words(CPU *cpu, const char *const *bits, int count)
{
    char err[256];
    int i;

    cpu_init(cpu);
    for (i = 0; i < count; i++)
        cpu->memory.words[i] = bin(bits[i]);
    if (cpu_run(cpu, NULL, err, sizeof err) != 0) {
        printf("  run error: %s\n", err);
        return -1;
    }
    return 0;
}

static int run_program(CPU *cpu, const char *name)
{
    const HandProgram *prog = find_program(name);
    char err[256];

    cpu_init(cpu);
    put_in_ram(&cpu->memory, prog);
    if (cpu_run(cpu, &prog->names, err, sizeof err) != 0) {
        printf("  run error: %s\n", err);
        return -1;
    }
    return 0;
}

/* ----- our two programs ----- */

static void test_add_program(void)
{
    static CPU cpu;
    CHECK(run_program(&cpu, "add") == 0, "add did not run");
    CHECK(cpu.out_port.count == 1 && cpu.out_port.values[0] == 12, "add printed %u", cpu.out_port.values[0]);
    CHECK(cpu.memory.words[0x08] == 12, "c in RAM is %u", cpu.memory.words[0x08]);
    CHECK(cpu.steps == 6, "add took %d instructions", cpu.steps);
}

static void test_multiply_program(void)
{
    static CPU cpu;
    const HandProgram *prog = find_program("multiply");
    int i, uses_mul = 0;

    CHECK(run_program(&cpu, "multiply") == 0, "multiply did not run");
    CHECK(cpu.out_port.count == 1 && cpu.out_port.values[0] == 15, "multiply printed %u", cpu.out_port.values[0]);
    CHECK(cpu.memory.words[0x0E] == 15 && cpu.memory.words[0x0D] == 0, "result/b in RAM are wrong");
    CHECK(cpu.steps == 28, "multiply took %d instructions", cpu.steps);

    /* the point of this program: it must not use the MUL instruction */
    for (i = 0; i < prog->code_count; i++)
        if ((bin(prog->words[i].bits) >> 12) == OP_MUL)
            uses_mul = 1;
    CHECK(!uses_mul, "multiply uses the MUL instruction");
}

/* Our hand-written binary must decode back to the assembly we wrote next to it. */
static void test_hand_encoding(void)
{
    int p, i;

    for (p = 0; p < PROGRAM_COUNT; p++) {
        const HandProgram *prog = &PROGRAMS[p];
        for (i = 0; i < prog->code_count; i++) {
            char text[64];
            const char *bits = prog->words[i].bits;
            int digits = 0;
            const char *c;

            for (c = bits; *c; c++)
                if (*c == '0' || *c == '1')
                    digits++;
            CHECK(digits == 16, "%s word %d has %d bits", prog->name, i, digits);
            disassemble(bin(bits), &prog->names, text, sizeof text);
            CHECK(strcmp(text, prog->words[i].assembly) == 0, "%s 0x%02X: binary %s decodes to '%s', table says '%s'",
                  prog->name, i, bits, text, prog->words[i].assembly);
        }
    }
}

/* Build each C program with the real C compiler and check it prints the same answer. */
static void test_same_answer_as_gcc(void)
{
    static CPU cpu;
    int p;

    if (system("cc -o build/host_program programs/add.c > /dev/null 2>&1") != 0) {
        printf("  (skipped the gcc check: no C compiler found)\n");
        return;
    }
    for (p = 0; p < PROGRAM_COUNT; p++) {
        char command[256], line[64] = "";
        FILE *pipe;

        snprintf(command, sizeof command, "cc -o build/host_program %s && ./build/host_program", PROGRAMS[p].c_file);
        pipe = popen(command, "r");
        if (!pipe)
            continue;
        if (!fgets(line, sizeof line, pipe))
            line[0] = '\0';
        pclose(pipe);
        run_program(&cpu, PROGRAMS[p].name);
        CHECK((unsigned)atoi(line) == cpu.out_port.values[0], "%s: ours %u, gcc %s",
              PROGRAMS[p].name, cpu.out_port.values[0], line);
    }
    remove("build/host_program");
}

/* ----- the CPU parts ----- */

static void test_clock(void)
{
    static CPU cpu;
    StepRecord rec;
    char err[256];

    run_program(&cpu, "add");
    CHECK(cpu.clock.cycle == 18, "6 instructions took %lu ticks, should be 18", cpu.clock.cycle);

    cpu_init(&cpu);
    put_in_ram(&cpu.memory, find_program("add"));
    cpu_step(&cpu, NULL, &rec, err, sizeof err);
    cpu_step(&cpu, NULL, &rec, err, sizeof err);
    CHECK(rec.clock[0] == 4 && rec.clock[1] == 5 && rec.clock[2] == 6,
          "instruction 2 ticks are %lu %lu %lu", rec.clock[0], rec.clock[1], rec.clock[2]);
}

static void test_alu_and_flags(void)
{
    Flags flags = {0, 0, 0, 0};
    ALU alu;

    alu.flags = &flags;
    CHECK(alu_compute(&alu, OP_ADD, 0xFFFF, 1) == 0 && flags.z == 1 && flags.c == 1, "0xFFFF + 1");
    CHECK(alu_compute(&alu, OP_SUB, 3, 5) == 0xFFFE && flags.n == 1 && flags.c == 1, "3 - 5");
    CHECK(alu_compute(&alu, OP_ADD, 0x7FFF, 1) == 0x8000 && flags.v == 1, "0x7FFF + 1");
    CHECK(alu_compute(&alu, OP_MUL, 300, 300) == ((300 * 300) & 0xFFFF) && flags.c == 1, "300 x 300");
}

static void test_fetch_uses_mar_mdr_and_bus(void)
{
    static const char *const words[] = {
        "0111 001 000000101",       /* LI R1, 5 */
        "1001 001 0 00100000",      /* ST R1, 0x20 */
        "0000 000000000000",        /* HALT */
    };
    static CPU cpu;
    StepRecord rec;
    char err[256];
    int i;

    cpu_init(&cpu);
    for (i = 0; i < 3; i++)
        cpu.memory.words[i] = bin(words[i]);
    cpu_step(&cpu, NULL, &rec, err, sizeof err);
    cpu_step(&cpu, NULL, &rec, err, sizeof err);   /* ST R1, 0x20 */

    CHECK(rec.bus_count == 2, "ST made %d bus transfers", rec.bus_count);
    CHECK(rec.bus_log[0].phase == PHASE_FETCH && rec.bus_log[0].kind == XFER_READ &&
          rec.bus_log[0].addr == 1 && rec.bus_log[0].data == bin(words[1]), "fetch transfer is wrong");
    CHECK(rec.bus_log[1].phase == PHASE_EXECUTE && rec.bus_log[1].kind == XFER_WRITE &&
          rec.bus_log[1].addr == 0x20 && rec.bus_log[1].data == 5, "store transfer is wrong");
    CHECK(rec.mar == 0x20 && rec.mdr == 5, "MAR=%X MDR=%X", rec.mar, rec.mdr);
    CHECK(strcmp(cpu.bus.control, "MEM WRITE") == 0, "control bus says %s", cpu.bus.control);
    CHECK(cpu.memory.words[0x20] == 5, "memory not written");
}

static void test_r0_stays_zero(void)
{
    static const char *const words[] = {
        "0111 000 000000101",       /* LI R0, 5 */
        "0111 001 000000011",       /* LI R1, 3 */
        "0111 010 000000101",       /* LI R2, 5 */
        "1010 000 001 010 000",     /* CMP R1, R2 */
        "0000 000000000000",        /* HALT */
    };
    static CPU cpu;
    run_words(&cpu, words, 5);
    CHECK(regfile_read(&cpu.regs, 0) == 0, "R0 changed");
    CHECK(cpu.flags.n == 1 && cpu.flags.z == 0 && cpu.flags.c == 1, "flags after 3 - 5");
}

static void test_branches(void)
{
    static const struct { int a, b; const char *taken; } cases[] = {
        {3, 5, "BLT BLE BNE"}, {5, 5, "BEQ BGE BLE"}, {-2, 1, "BLT BLE BNE"}, {7, -4, "BGT BGE BNE"},
    };
    static const char *const names[] = { "BEQ", "BNE", "BLT", "BGE", "BGT", "BLE" };
    static CPU cpu;
    char li1[24], li2[24], branch[24];
    int i, cond;

    for (i = 0; i < 4; i++) {
        for (cond = 1; cond <= 6; cond++) {
            const char *words[7];
            int k;
            /* LI R1, a / LI R2, b / CMP R1, R2 / Bcc 0x05 / HALT / LI R3, 1 / HALT */
            strcpy(li1, "0111 001 ");
            strcpy(li2, "0111 010 ");
            for (k = 8; k >= 0; k--) {
                strncat(li1, ((cases[i].a >> k) & 1) ? "1" : "0", 1);
                strncat(li2, ((cases[i].b >> k) & 1) ? "1" : "0", 1);
            }
            strcpy(branch, "1011 ");
            for (k = 3; k >= 0; k--)
                strncat(branch, ((cond >> k) & 1) ? "1" : "0", 1);
            strcat(branch, " 00000101");
            words[0] = li1;
            words[1] = li2;
            words[2] = "1010 000 001 010 000";
            words[3] = branch;
            words[4] = "0000 000000000000";
            words[5] = "0111 011 000000001";
            words[6] = "0000 000000000000";
            run_words(&cpu, words, 7);
            CHECK((regfile_read(&cpu.regs, 3) == 1) == (strstr(cases[i].taken, names[cond - 1]) != NULL),
                  "%d %s %d", cases[i].a, names[cond - 1], cases[i].b);
        }
    }
}

static void test_stack(void)
{
    static const char *const words[] = {
        "0111 111 001000000",       /* 0x00 LI SP, 0x40 */
        "0111 001 000001011",       /* 0x01 LI R1, 11 */
        "1110 001 0 00000000",      /* 0x02 PUSH R1 */
        "1100 0000 00000110",       /* 0x03 CALL 0x06 */
        "1110 010 1 00000000",      /* 0x04 POP R2 */
        "0000 000000000000",        /* 0x05 HALT */
        "0111 001 000000000",       /* 0x06 LI R1, 0 */
        "1101 000000000000",        /* 0x07 RET */
    };
    static CPU cpu;
    run_words(&cpu, words, 8);
    CHECK(regfile_read(&cpu.regs, 2) == 11, "POP gave %d", regfile_read(&cpu.regs, 2));
    CHECK(regfile_read(&cpu.regs, SP) == 0x40, "SP ended at %X", regfile_read(&cpu.regs, SP));
}

static void test_ascii_and_binary(void)
{
    char ascii[3], bits[32];

    word_ascii(0x4869, ascii);          /* "Hi" */
    CHECK(strcmp(ascii, "Hi") == 0, "0x4869 shows as %s", ascii);
    word_ascii(0x8206, ascii);
    CHECK(strcmp(ascii, "..") == 0, "0x8206 shows as %s", ascii);
    CHECK(bin("0111 001 000000101") == 0x7205, "bin() is wrong");
    value_ascii(0x41, 8, ascii);          /* 8-bit register: one character */
    CHECK(strcmp(ascii, "A") == 0, "8-bit 0x41 shows as %s", ascii);
    value_ascii(0x4142, 16, ascii);       /* 16-bit register: two characters */
    CHECK(strcmp(ascii, "AB") == 0, "16-bit 0x4142 shows as %s", ascii);
    CHECK(strcmp(binary_plain(0x8206, bits, sizeof bits), "1000001000000110") == 0, "binary_plain is wrong");
}

int main(void)
{
    static const struct { const char *name; void (*run)(void); } tests[] = {
        {"program 1: add two numbers", test_add_program},
        {"program 2: multiply using only ADD", test_multiply_program},
        {"hand-written binary matches assembly", test_hand_encoding},
        {"same answers as gcc", test_same_answer_as_gcc},
        {"clock: 3 ticks per instruction", test_clock},
        {"ALU and flags", test_alu_and_flags},
        {"fetch uses MAR, MDR and the bus", test_fetch_uses_mar_mdr_and_bus},
        {"R0 stays zero", test_r0_stays_zero},
        {"branches", test_branches},
        {"stack", test_stack},
        {"binary and ASCII display", test_ascii_and_binary},
    };
    int count = (int)(sizeof tests / sizeof tests[0]);
    int i;

    for (i = 0; i < count; i++) {
        int before = failures;
        tests[i].run();
        printf("%-40s %s\n", tests[i].name, failures == before ? "ok" : "FAILED");
    }
    printf("\n%d tests, %d checks, %d failed\n", count, checks, failures);
    return failures ? 1 : 0;
}
