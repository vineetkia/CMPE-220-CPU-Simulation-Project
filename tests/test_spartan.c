/*
 * test_spartan.c
 * Tests for the simulator, assembler and compiler.
 * Run them with:  make test
 */
#define _POSIX_C_SOURCE 200809L   /* for popen() */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "assembler.h"
#include "compiler.h"
#include "cpu.h"
#include "loader.h"

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

static char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    char *text;
    long size;

    if (!f) {
        printf("  can't open %s\n", path);
        exit(1);
    }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    text = calloc((size_t)size + 1, 1);
    if (fread(text, 1, (size_t)size, f) != (size_t)size)
        exit(1);
    fclose(f);
    return text;
}

/* Assemble and run. Returns 0 if everything worked. */
static int run_asm(const char *text, CPU *cpu)
{
    static Program prog;
    char err[256];

    if (assemble(text, "test", &prog, err, sizeof err) != 0) {
        printf("  assemble error: %s\n", err);
        return -1;
    }
    cpu_init(cpu);
    load_program(cpu, prog.words, prog.word_count, NULL);
    if (cpu_run(cpu, &prog.symbols, err, sizeof err) != 0) {
        printf("  run error: %s\n", err);
        program_free(&prog);
        return -1;
    }
    program_free(&prog);
    return 0;
}

static int run_c(const char *text, CPU *cpu)
{
    char err[256];
    char *asm_text = compile_c(text, "test.c", err, sizeof err);
    int result;

    if (!asm_text) {
        printf("  compile error: %s\n", err);
        return -1;
    }
    result = run_asm(asm_text, cpu);
    free(asm_text);
    return result;
}

static int run_file(const char *path, CPU *cpu)
{
    char *text = read_file(path);
    int is_c = strcmp(path + strlen(path) - 2, ".c") == 0;
    int result = is_c ? run_c(text, cpu) : run_asm(text, cpu);
    free(text);
    return result;
}

/* The program output as one string, like "0 1 1 2". */
static const char *output_text(const CPU *cpu)
{
    static char text[1024];
    size_t length = 0;
    int i;

    text[0] = '\0';
    for (i = 0; i < cpu->out_port.count && length < sizeof text - 16; i++)
        length += (size_t)sprintf(text + length, "%s%u", i ? " " : "", cpu->out_port.values[i]);
    return text;
}

/* ----- whole programs ----- */

static void test_factorial_of_3(void)
{
    static CPU cpu;
    CHECK(run_file("programs/factorial_of_3.asm", &cpu) == 0, "3! did not run");
    CHECK(strcmp(output_text(&cpu), "6") == 0, "3! printed %s", output_text(&cpu));
    CHECK(cpu.steps == 19, "3! took %d steps", cpu.steps);
}

static void test_factorial_asm(void)
{
    static CPU cpu;
    CHECK(run_file("programs/factorial.asm", &cpu) == 0, "8! did not run");
    CHECK(strcmp(output_text(&cpu), "40320") == 0, "8! printed %s", output_text(&cpu));
}

static void test_power_c(void)
{
    static CPU cpu;
    CHECK(run_file("programs/power.c", &cpu) == 0, "power.c did not run");
    CHECK(strcmp(output_text(&cpu), "64") == 0, "power.c printed %s", output_text(&cpu));
}

static void test_extra_c_programs(void)
{
    static CPU cpu;
    CHECK(run_file("programs/extras/factorial.c", &cpu) == 0 && strcmp(output_text(&cpu), "40320") == 0,
          "extras/factorial.c printed %s", output_text(&cpu));
    CHECK(run_file("programs/extras/fibonacci.c", &cpu) == 0 &&
          strcmp(output_text(&cpu), "0 1 1 2 3 5 8 13 21 34") == 0,
          "extras/fibonacci.c printed %s", output_text(&cpu));
    CHECK(run_file("programs/extras/gcd.c", &cpu) == 0 && strcmp(output_text(&cpu), "12 501") == 0,
          "extras/gcd.c printed %s", output_text(&cpu));
}

/* Build each C file with the real C compiler and check it prints the same thing. */
static void test_same_answer_as_gcc(void)
{
    static const char *const files[] = {
        "programs/power.c", "programs/extras/factorial.c",
        "programs/extras/fibonacci.c", "programs/extras/gcd.c", NULL
    };
    static CPU cpu;
    int i;

    if (system("cc -o build/host_program programs/power.c > /dev/null 2>&1") != 0) {
        printf("  (skipped the gcc check: no C compiler found)\n");
        return;
    }
    for (i = 0; files[i]; i++) {
        char command[256], expected[1024] = "", line[64];
        FILE *pipe;
        size_t length = 0;

        snprintf(command, sizeof command, "cc -o build/host_program %s && ./build/host_program", files[i]);
        pipe = popen(command, "r");
        if (!pipe)
            continue;
        while (fgets(line, sizeof line, pipe) && length < sizeof expected - 64) {
            line[strcspn(line, "\n")] = '\0';
            length += (size_t)sprintf(expected + length, "%s%s", length ? " " : "", line);
        }
        pclose(pipe);
        run_file(files[i], &cpu);
        CHECK(strcmp(output_text(&cpu), expected) == 0, "%s: ours %s, gcc %s",
              files[i], output_text(&cpu), expected);
    }
    remove("build/host_program");
}

/* ----- the CPU parts ----- */

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
    static Program prog;
    static CPU cpu;
    StepRecord rec;
    char err[256];

    assemble("LI R1, 5\nST R1, 0x20\nHALT", "test", &prog, err, sizeof err);
    cpu_init(&cpu);
    load_program(&cpu, prog.words, prog.word_count, NULL);
    cpu_step(&cpu, NULL, &rec, err, sizeof err);
    cpu_step(&cpu, NULL, &rec, err, sizeof err);   /* ST R1, 0x20 */

    CHECK(rec.bus_count == 2, "ST made %d bus transfers", rec.bus_count);
    CHECK(rec.bus_log[0].phase == PHASE_FETCH && rec.bus_log[0].kind == XFER_READ &&
          rec.bus_log[0].addr == 1 && rec.bus_log[0].data == prog.words[1], "fetch transfer is wrong");
    CHECK(rec.bus_log[1].phase == PHASE_EXECUTE && rec.bus_log[1].kind == XFER_WRITE &&
          rec.bus_log[1].addr == 0x20 && rec.bus_log[1].data == 5, "store transfer is wrong");
    CHECK(rec.mar == 0x20 && rec.mdr == 5, "MAR=%X MDR=%X", rec.mar, rec.mdr);
    CHECK(strcmp(cpu.bus.control, "MEM WRITE") == 0, "control bus says %s", cpu.bus.control);
    CHECK(cpu.memory.words[0x20] == 5, "memory not written");
    program_free(&prog);
}

/* The loader writes every word into RAM over the bus, starting at 0x00. */
static void test_loader(void)
{
    static Program prog;
    static CPU cpu;
    static LoadStep steps[MEM_SIZE];
    char err[256], *source = read_file("programs/factorial_of_3.asm");
    int count, i, rest_is_zero = 1;

    assemble(source, "factorial_of_3.asm", &prog, err, sizeof err);
    free(source);
    cpu_init(&cpu);
    for (i = 0; i < MEM_SIZE; i++)
        if (cpu.memory.words[i] != 0)
            rest_is_zero = 0;
    CHECK(rest_is_zero, "RAM is not empty before loading");

    count = load_program(&cpu, prog.words, prog.word_count, steps);
    CHECK(count == prog.word_count && count == 11, "loaded %d words", count);
    for (i = 0; i < count; i++) {
        CHECK(cpu.memory.words[i] == prog.words[i], "RAM[%d] is %04X", i, cpu.memory.words[i]);
        CHECK(steps[i].addr == i && steps[i].word == prog.words[i] &&
              strcmp(steps[i].control, "MEM WRITE") == 0, "load step %d is wrong", i);
    }
    for (i = count; i < MEM_SIZE; i++)
        if (cpu.memory.words[i] != 0)
            rest_is_zero = 0;
    CHECK(rest_is_zero, "loader wrote past the program");
    CHECK(cpu.pc.value == 0, "PC is %X after loading, should be the entry point 0", cpu.pc.value);
    CHECK(strcmp(cpu.bus.control, "idle") == 0, "bus is %s after loading", cpu.bus.control);
    program_free(&prog);
}

/* ----- the assembler's lookup table ----- */

static void test_lookup_table(void)
{
    const OpcodeEntry *entry = lookup_mnemonic("BNE");
    int i;

    CHECK(entry && entry->opcode == OP_BR && entry->format == FMT_B && entry->extra == 2, "BNE lookup");
    entry = lookup_mnemonic("MUL");
    CHECK(entry && entry->opcode == OP_MUL && entry->format == FMT_R, "MUL lookup");
    entry = lookup_mnemonic("POP");
    CHECK(entry && entry->opcode == OP_STK && entry->extra == 1, "POP lookup");
    CHECK(lookup_mnemonic("FOO") == NULL, "FOO should not be found");

    /* every name in the table must decode back to the same name */
    for (i = 0; i < OPCODE_TABLE_SIZE; i++) {
        const OpcodeEntry *e = &OPCODE_TABLE[i];
        Decoded d = decode((uint16_t)((e->opcode << 12) | (e->extra << 8)));
        char name[16];
        CHECK(strcmp(decoded_mnemonic(&d, name, sizeof name), e->mnemonic) == 0,
              "%s decodes as %s", e->mnemonic, name);
    }
}

/* ----- single instructions ----- */

/* Make text easy to compare: no commas, single spaces, upper case, R7 written as SP. */
static void clean(const char *in, char *out)
{
    int space = 0;
    for (; *in; in++) {
        char c = *in == ',' ? ' ' : *in;
        if (c == ' ' || c == '\t') {
            space = 1;
            continue;
        }
        if (space && out[-1] != '\0') {
            *out++ = ' ';
        }
        space = 0;
        if (c == 'R' && in[1] == '7') {
            *out++ = 'S';
            *out++ = 'P';
            in++;
            continue;
        }
        *out++ = (char)(c >= 'a' && c <= 'z' ? c - 32 : c);
    }
    *out = '\0';
}

static void test_encode_then_decode(void)
{
    static const char *const lines[] = {
        "ADD R1, R2, R3", "SUB R4, R5, R6", "MUL R1, R1, R2", "AND R3, R3, R1", "OR  R2, R1, R0",
        "ADDI R2, R2, -1", "LI  R7, 200", "LD  R1, 0x20", "ST  R2, [SP+3]", "LD  R3, [R4-2]",
        "CMP R1, R0", "BLE 0x10", "CALL 0x30", "RET", "PUSH R5", "POP R6", "OUT R1", "HALT", NULL
    };
    static Program prog;
    char source[1024] = "", err[256], text[96], a[96], b[96];
    int i;

    for (i = 0; lines[i]; i++) {
        strcat(source, lines[i]);
        strcat(source, "\n");
    }
    CHECK(assemble(source, "test", &prog, err, sizeof err) == 0, "assemble: %s", err);
    for (i = 0; lines[i]; i++) {
        a[0] = b[0] = '\0';
        disassemble(prog.words[i], NULL, text, sizeof text);
        clean(text, a + 1);
        clean(lines[i], b + 1);
        CHECK(strcmp(a + 1, b + 1) == 0, "'%s' came back as '%s'", lines[i], text);
    }
    program_free(&prog);
}

static void test_r0_stays_zero(void)
{
    static CPU cpu;
    run_asm("LI R0, 5\nLI R1, 3\nLI R2, 5\nCMP R1, R2\nHALT", &cpu);
    CHECK(regfile_read(&cpu.regs, 0) == 0, "R0 changed");
    CHECK(cpu.flags.n == 1 && cpu.flags.z == 0 && cpu.flags.c == 1, "flags after 3 - 5");
}

static void test_branches(void)
{
    static const struct { int a, b; const char *taken; } cases[] = {
        {3, 5, "BLT BLE BNE"}, {5, 5, "BEQ BGE BLE"}, {-2, 1, "BLT BLE BNE"}, {7, -4, "BGT BGE BNE"},
    };
    static const char *const branches[] = { "BEQ", "BNE", "BLT", "BGE", "BGT", "BLE" };
    static CPU cpu;
    char source[256];
    int i, j;

    for (i = 0; i < 4; i++) {
        for (j = 0; j < 6; j++) {
            snprintf(source, sizeof source,
                     "LI R1, %d\nLI R2, %d\nCMP R1, R2\n%s yes\nHALT\nyes: LI R3, 1\nHALT",
                     cases[i].a, cases[i].b, branches[j]);
            run_asm(source, &cpu);
            CHECK((regfile_read(&cpu.regs, 3) == 1) == (strstr(cases[i].taken, branches[j]) != NULL),
                  "%d %s %d", cases[i].a, branches[j], cases[i].b);
        }
    }
}

static void test_stack(void)
{
    static CPU cpu;
    run_asm("LI SP, 0x40\nLI R1, 11\nPUSH R1\nCALL f\nPOP R2\nHALT\nf: LI R1, 0\nRET", &cpu);
    CHECK(regfile_read(&cpu.regs, 2) == 11, "POP gave %d", regfile_read(&cpu.regs, 2));
    CHECK(regfile_read(&cpu.regs, SP) == 0x40, "SP ended at %X", regfile_read(&cpu.regs, SP));
}

/* ----- the compiler ----- */

static void test_expressions(void)
{
    static const int expected[] = { -7, 52, 2, 10, 6, 2 };
    static CPU cpu;
    int i;

    run_c("int g = 3;\n"
          "int sq(int v) { return v * v; }\n"
          "int main() {\n"
          "    int a = 6, b = -4;\n"
          "    printf(\"%d\", a + b * 2 - (a - 1));\n"
          "    printf(\"%d\", sq(a) + sq(sq(2)));\n"
          "    printf(\"%d\", (a > b && b < 0) + (a == 6 || 0) + !a);\n"
          "    printf(\"%d\", (a & 3) | 8);\n"
          "    g *= 2; g += 1; g--;\n"
          "    printf(\"%d\", g);\n"
          "    if (a < 0) printf(\"%d\", 1); else printf(\"%d\", 2);\n"
          "    return 0;\n"
          "}\n", &cpu);
    CHECK(cpu.out_port.count == 6, "printed %d numbers", cpu.out_port.count);
    /* OUT prints unsigned numbers, so turn them back into signed ones first */
    for (i = 0; i < 6 && i < cpu.out_port.count; i++)
        CHECK(to_signed16((int)cpu.out_port.values[i]) == expected[i], "value %d is %d, wanted %d",
              i, to_signed16((int)cpu.out_port.values[i]), expected[i]);
}

static void test_rejects_recursion_and_divide(void)
{
    char err[256];
    char *result;

    result = compile_c("int f(int n) { return f(n); } int main() { return f(1); }", "t.c", err, sizeof err);
    CHECK(result == NULL && strstr(err, "recursion"), "recursion was not rejected");
    free(result);
    result = compile_c("int main() { int a = 4; return a / 2; }", "t.c", err, sizeof err);
    CHECK(result == NULL && strstr(err, "divide"), "divide was not rejected");
    free(result);
}

int main(void)
{
    static const struct { const char *name; void (*run)(void); } tests[] = {
        {"3! sample", test_factorial_of_3},
        {"8! in assembly", test_factorial_asm},
        {"8^2 in C", test_power_c},
        {"extra C programs", test_extra_c_programs},
        {"same answers as gcc", test_same_answer_as_gcc},
        {"ALU and flags", test_alu_and_flags},
        {"fetch uses MAR, MDR and the bus", test_fetch_uses_mar_mdr_and_bus},
        {"loader writes RAM over the bus", test_loader},
        {"assembler lookup table", test_lookup_table},
        {"encode then decode", test_encode_then_decode},
        {"R0 stays zero", test_r0_stays_zero},
        {"branches", test_branches},
        {"stack", test_stack},
        {"compiler expressions", test_expressions},
        {"compiler rejects recursion and /", test_rejects_recursion_and_divide},
    };
    int count = (int)(sizeof tests / sizeof tests[0]);
    int i;

    for (i = 0; i < count; i++) {
        int before = failures;
        tests[i].run();
        printf("%-36s %s\n", tests[i].name, failures == before ? "ok" : "FAILED");
    }
    printf("\n%d tests, %d checks, %d failed\n", count, checks, failures);
    return failures ? 1 : 0;
}
