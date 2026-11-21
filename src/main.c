/*
 * main.c
 * The SPARTAN-16 command line tool.
 *
 * Build it with "make", then:
 *     ./spartan run add            program 1: add two numbers (5 + 7)
 *     ./spartan run multiply       program 2: multiply using only ADD (5 x 3)
 *     ./spartan run add --quiet    just print the answer
 *     ./spartan list               list the programs
 *     ./spartan isa                print the instruction table
 *
 * "run" shows the whole thing:
 *     1. the C program
 *     2. our hand translation into assembly and binary
 *     3. RAM after we put the binary in by hand
 *     4. every instruction: the clock ticks for fetch, decode and execute,
 *        then the CPU registers, the ALU and the RAM
 */
#define _POSIX_C_SOURCE 200809L   /* for isatty() and fileno() */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <unistd.h>
#endif

#include "cpu.h"
#include "display.h"
#include "programs.h"

static void usage(FILE *out)
{
    int i;

    fprintf(out,
        "usage: spartan <command> [options]\n"
        "\n"
        "commands:\n"
        "  run PROGRAM [--quiet] [--no-color]   run a program and show every step\n"
        "  list                                 list the programs\n"
        "  isa                                  print the instruction table\n"
        "\n"
        "programs:\n");
    for (i = 0; i < PROGRAM_COUNT; i++)
        fprintf(out, "  %-10s %s\n", PROGRAMS[i].name, PROGRAMS[i].title);
}

/* Read a whole file into memory. Returns NULL if it can't. */
static char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    char *text;
    long size;

    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    text = malloc((size_t)size + 1);
    if (!text || fread(text, 1, (size_t)size, f) != (size_t)size) {
        fclose(f);
        free(text);
        return NULL;
    }
    text[size] = '\0';
    fclose(f);
    return text;
}

/* Accept "add", "programs/add.c" or "add.c". */
static const HandProgram *program_from_arg(const char *arg)
{
    char name[64];
    const char *slash = strrchr(arg, '/');
    char *dot;

    snprintf(name, sizeof name, "%s", slash ? slash + 1 : arg);
    dot = strrchr(name, '.');
    if (dot)
        *dot = '\0';
    return find_program(name);
}

static int cmd_run(const HandProgram *prog, int quiet, int no_color)
{
    static CPU cpu;
    StepRecord rec;
    char err[256];
    char *c_source;
    int color = 0, i;

#ifndef _WIN32
    color = isatty(fileno(stdout)) && !no_color;
#endif
    (void)no_color;

    /* power on, then put our hand-made binary into RAM */
    cpu_init(&cpu);
    put_in_ram(&cpu.memory, prog);

    if (!quiet) {
        c_source = read_file(prog->c_file);
        print_program(stdout, prog, c_source, &cpu, color);
        free(c_source);
    }

    /* fetch, decode, execute until HALT */
    while (!cpu.halted) {
        if (cpu_step(&cpu, &prog->names, &rec, err, sizeof err) != 0) {
            fprintf(stderr, "error: %s\n", err);
            return 1;
        }
        if (!quiet)
            print_step(stdout, prog, &rec, color);
    }

    if (!quiet) {
        printf("==============================================================================\n");
        printf("Halted after %d instructions (%lu clock ticks).\n", cpu.steps, cpu.clock.cycle);
    }
    printf("Program output: ");
    if (cpu.out_port.count == 0)
        printf("(none)");
    for (i = 0; i < cpu.out_port.count; i++)
        printf("%s%u", i ? " " : "", cpu.out_port.values[i]);
    printf("\n");
    return 0;
}

static int cmd_isa(void)
{
    int i;
    char bits[8];

    printf("%-9s %-6s %-5s %-34s %-40s %s\n", "NAME", "OPCODE", "FMT", "SYNTAX", "WHAT IT DOES", "FLAGS");
    for (i = 0; i < ISA_TABLE_SIZE; i++) {
        const IsaRow *row = &ISA_TABLE[i];
        printf("%-9s %-6s %-5s %-34s %-40s %s\n", row->name, opcode_bits(row->opcode, bits, sizeof bits),
               row->format, row->syntax, row->what, row->flags);
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
    const char *target = NULL;
    const HandProgram *prog;
    int quiet = 0, no_color = 0, i;

    if (argc < 2) {
        usage(stdout);
        return 1;
    }
    if (strcmp(argv[1], "isa") == 0)
        return cmd_isa();
    if (strcmp(argv[1], "list") == 0) {
        for (i = 0; i < PROGRAM_COUNT; i++)
            printf("%-10s %s  (%s)\n", PROGRAMS[i].name, PROGRAMS[i].title, PROGRAMS[i].c_file);
        return 0;
    }
    if (strcmp(argv[1], "run") != 0) {
        usage(strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0 ? stdout : stderr);
        return 1;
    }

    for (i = 2; i < argc; i++) {
        if (strcmp(argv[i], "-q") == 0 || strcmp(argv[i], "--quiet") == 0)
            quiet = 1;
        else if (strcmp(argv[i], "--no-color") == 0)
            no_color = 1;
        else if (!target)
            target = argv[i];
        else {
            fprintf(stderr, "error: unexpected argument '%s'\n\n", argv[i]);
            usage(stderr);
            return 2;
        }
    }
    if (!target) {
        fprintf(stderr, "error: run needs a program name\n\n");
        usage(stderr);
        return 2;
    }
    prog = program_from_arg(target);
    if (!prog) {
        fprintf(stderr, "error: no program called '%s'\n\n", target);
        usage(stderr);
        return 2;
    }
    return cmd_run(prog, quiet, no_color);
}
