/*
 * main.c
 * The SPARTAN-16 command line tool.
 *
 * Build it with "make", then:
 * "run" shows the whole simulation in three phases:
 *     1. ASSEMBLE  translate the source into machine code with the ISA lookup table
 *     2. LOAD      write the machine code into RAM over the system bus
 *     3. EXECUTE   fetch, decode, execute every instruction until HALT
 *
 *     ./spartan run programs/factorial_of_3.asm   small sample (3! = 6)
 *     ./spartan run programs/factorial.asm        program 1 (8! = 40320)
 *     ./spartan run programs/power.c              program 2 (8^2 = 64), C code
 *     ./spartan run programs/power.c --quiet      just print the answer
 *     ./spartan asm programs/factorial.asm        show the machine code
 *     ./spartan compile programs/power.c          show the C -> assembly output
 *     ./spartan isa                               list all instructions
 */
#define _POSIX_C_SOURCE 200809L   /* for isatty() and fileno() */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <unistd.h>
#endif

#include "assembler.h"
#include "compiler.h"
#include "cpu.h"
#include "display.h"
#include "loader.h"

static void usage(FILE *out)
{
    fprintf(out,
        "usage: spartan <command> [options]\n"
        "\n"
        "commands:\n"
        "  run FILE [--quiet] [--no-color]   run a .asm or .c program and show every step\n"
        "  asm FILE                          assemble a program and show the machine code\n"
        "  compile FILE [-o OUTPUT]          turn a .c file into SPARTAN-16 assembly\n"
        "  isa                               list the instruction set\n");
}

/* Read a whole file into memory. Returns NULL and prints why if it can't. */
static char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    char *text;
    long size;

    if (!f) {
        fprintf(stderr, "error: can't open %s: %s\n", path, strerror(errno));
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    text = malloc((size_t)size + 1);
    if (!text || fread(text, 1, (size_t)size, f) != (size_t)size) {
        fprintf(stderr, "error: can't read %s\n", path);
        fclose(f);
        free(text);
        return NULL;
    }
    text[size] = '\0';
    fclose(f);
    return text;
}

static const char *base_name(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static int ends_with(const char *s, const char *end)
{
    size_t a = strlen(s), b = strlen(end);
    return a >= b && strcmp(s + a - b, end) == 0;
}

/* Read a .asm or .c file and assemble it. C files go through our compiler first.
 * Returns the assembly text (free it), or NULL on a problem. */
static char *read_program(const char *path, Program *prog)
{
    char err[512];
    char *source = read_file(path);
    char *asm_text;

    if (!source)
        return NULL;
    if (ends_with(path, ".c")) {
        asm_text = compile_c(source, base_name(path), err, sizeof err);
        free(source);
        if (!asm_text) {
            fprintf(stderr, "error: %s\n", err);
            return NULL;
        }
    } else {
        asm_text = source;
    }
    if (assemble(asm_text, base_name(path), prog, err, sizeof err) != 0) {
        fprintf(stderr, "error: %s\n", err);
        free(asm_text);
        return NULL;
    }
    return asm_text;
}

static int cmd_run(const char *path, int quiet, int no_color)
{
    static Program prog;
    static CPU cpu;
    static LoadStep load_steps[MEM_SIZE];
    StepRecord rec;
    char err[256];
    char *asm_text = read_program(path, &prog);
    int color = 0, i;

    if (!asm_text)
        return 1;
#ifndef _WIN32
    color = isatty(fileno(stdout)) && !no_color;
#endif

    /* power on, then the loader puts the program into RAM */
    cpu_init(&cpu);
    load_program(&cpu, prog.words, prog.word_count, load_steps);
    if (!quiet) {
        print_assemble(stdout, &prog);
        print_load(stdout, &prog, &cpu, load_steps, prog.word_count, color);
        print_initial(stdout, &prog, &cpu, color);
    }
    while (!cpu.halted) {
        if (cpu_step(&cpu, &prog.symbols, &rec, err, sizeof err) != 0) {
            fprintf(stderr, "error: %s\n", err);
            free(asm_text);
            program_free(&prog);
            return 1;
        }
        if (!quiet)
            print_step(stdout, &prog, &rec, color);
    }

    if (!quiet) {
        printf("==============================================================================\n");
        printf("Halted after %d instructions (%d clock cycles).\n", cpu.steps, cpu.cycles);
    }
    printf("Program output: ");
    if (cpu.out_port.count == 0)
        printf("(none)");
    for (i = 0; i < cpu.out_port.count; i++)
        printf("%s%u", i ? " " : "", cpu.out_port.values[i]);
    printf("\n");

    free(asm_text);
    program_free(&prog);
    return 0;
}

static int cmd_asm(const char *path)
{
    static Program prog;
    char *asm_text = read_program(path, &prog);
    int i;

    if (!asm_text)
        return 1;
    print_listing(stdout, &prog);
    printf("\nLabels: ");
    for (i = 0; i < prog.symbols.count; i++)
        printf("%s%s=0x%02X", i ? ", " : "", prog.symbols.items[i].name, prog.symbols.items[i].value);
    printf("\n");
    free(asm_text);
    program_free(&prog);
    return 0;
}

static int cmd_compile(const char *path, const char *output)
{
    static Program prog;
    char *asm_text = read_program(path, &prog);

    if (!asm_text)
        return 1;
    if (output) {
        FILE *f = fopen(output, "w");
        if (!f) {
            fprintf(stderr, "error: can't write %s: %s\n", output, strerror(errno));
            free(asm_text);
            program_free(&prog);
            return 1;
        }
        fputs(asm_text, f);
        fclose(f);
        printf("wrote %s\n", output);
    } else {
        fputs(asm_text, stdout);
    }
    free(asm_text);
    program_free(&prog);
    return 0;
}

static int cmd_isa(void)
{
    int i;

    printf("%-9s %-4s %-5s %-34s %-40s %s\n", "NAME", "OP", "FMT", "SYNTAX", "WHAT IT DOES", "FLAGS");
    for (i = 0; i < ISA_TABLE_SIZE; i++) {
        const IsaRow *row = &ISA_TABLE[i];
        printf("%-9s 0x%X  %-5s %-34s %-40s %s\n", row->name, row->opcode, row->format,
               row->syntax, row->what, row->flags);
    }
    printf("\nInstruction formats (bit 15 on the left):\n");
    for (i = 0; i < FMT_COUNT; i++) {
        const Field *field;
        printf("  %-3s ", FORMAT_NAMES[i]);
        for (field = FORMATS[i]; field->name; field++)
            printf("%s%s(%d)", field == FORMATS[i] ? "" : " | ", field->name, field->width);
        printf("\n");
    }
    return 0;
}

int main(int argc, char **argv)
{
    const char *command, *file = NULL, *output = NULL;
    int quiet = 0, no_color = 0, i;

    if (argc < 2) {
        usage(stdout);
        return 1;
    }
    command = argv[1];

    for (i = 2; i < argc; i++) {
        if (strcmp(argv[i], "-q") == 0 || strcmp(argv[i], "--quiet") == 0) {
            quiet = 1;
        } else if (strcmp(argv[i], "--no-color") == 0) {
            no_color = 1;
        } else if ((strcmp(argv[i], "-o") == 0 || strcmp(argv[i], "--output") == 0) && i + 1 < argc) {
            output = argv[++i];
        } else if (argv[i][0] == '-' || file) {
            fprintf(stderr, "error: unexpected argument '%s'\n\n", argv[i]);
            usage(stderr);
            return 2;
        } else {
            file = argv[i];
        }
    }

    if (strcmp(command, "isa") == 0)
        return cmd_isa();
    if (strcmp(command, "run") != 0 && strcmp(command, "asm") != 0 && strcmp(command, "compile") != 0) {
        usage(strcmp(command, "-h") == 0 || strcmp(command, "--help") == 0 ? stdout : stderr);
        return 1;
    }
    if (!file) {
        fprintf(stderr, "error: %s needs a file name\n\n", command);
        usage(stderr);
        return 2;
    }
    if (strcmp(command, "run") == 0)
        return cmd_run(file, quiet, no_color);
    if (strcmp(command, "asm") == 0)
        return cmd_asm(file);
    return cmd_compile(file, output);
}
