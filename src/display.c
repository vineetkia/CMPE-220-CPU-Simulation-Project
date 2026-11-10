/*
 * display.c
 * Prints the machine state in the terminal.
 *
 * How to read the memory dump:
 *     >7208     the next instruction (the word PC points to)
 *     [0038]    a word that the last instruction wrote
 *     R1=0038*  a register that the last instruction wrote
 */
#include <ctype.h>
#include <string.h>

#include "display.h"

#define COLOR_PC    "\033[1;32m"
#define COLOR_WRITE "\033[1;31m"
#define COLOR_HEAD  "\033[1;36m"
#define COLOR_END   "\033[0m"

static void print_bar(FILE *out)
{
    char bar[79];

    memset(bar, '=', 78);
    bar[78] = '\0';
    fprintf(out, "%s\n", bar);
}

int window_end(const Program *prog)
{
    int n = prog->word_count > ROW ? prog->word_count : ROW;
    return (n + ROW - 1) / ROW * ROW;
}

/* What is stored in this memory row: code, data, stack (or a mix). */
static void print_row_kind(FILE *out, const Program *prog, int start)
{
    int seen[3] = {0, 0, 0};
    int a, printed = 0;

    for (a = start; a < start + ROW && a < prog->word_count; a++) {
        if (!seen[prog->kinds[a]]) {
            seen[prog->kinds[a]] = 1;
            fprintf(out, "%s%s", printed ? "/" : "", kind_name(prog->kinds[a]));
            printed = 1;
        }
    }
}

/* pc is -1 after HALT, because then there is no next instruction to mark. */
static void print_memory(FILE *out, const Program *prog, const uint16_t *mem, int pc,
                         const char *written, int color)
{
    int start, a, i;

    fprintf(out, "  ADDR  ");
    for (i = 0; i < ROW; i++)
        fprintf(out, "  +%d  ", i);
    fprintf(out, "\n");

    for (start = 0; start < window_end(prog); start += ROW) {
        fprintf(out, "  0x%02X  ", start);
        for (a = start; a < start + ROW; a++) {
            if (written && written[a])
                fprintf(out, "%s[%04X]%s", color ? COLOR_WRITE : "", mem[a], color ? COLOR_END : "");
            else if (a == pc)
                fprintf(out, "%s>%04X %s", color ? COLOR_PC : "", mem[a], color ? COLOR_END : "");
            else
                fprintf(out, " %04X ", mem[a]);
        }
        fprintf(out, "  ");
        print_row_kind(out, prog, start);
        fprintf(out, "\n");
    }
}

static void print_regs(FILE *out, const uint16_t *regs, const char *written)
{
    int i;

    for (i = 0; i < NUM_REGS; i++)
        fprintf(out, "%s%s=%04X%c", i ? " " : "", REG_NAMES[i], regs[i],
                (written && written[i]) ? '*' : ' ');
}

static void print_vars(FILE *out, const Program *prog, const uint16_t *mem)
{
    int i;

    if (prog->variables.count == 0)
        fprintf(out, "(none)");
    for (i = 0; i < prog->variables.count; i++)
        fprintf(out, "%s%s=%d", i ? "  " : "", prog->variables.items[i].name,
                mem[prog->variables.items[i].value]);
}

static void print_flags(FILE *out, const Flags *f)
{
    fprintf(out, "Z=%d N=%d C=%d V=%d", f->z, f->n, f->c, f->v);
}

/* One short entry for every transfer on the system bus. */
static void print_bus(FILE *out, const BusTransfer *log, int count)
{
    int i;

    for (i = 0; i < count; i++) {
        const char *phase = log[i].phase == PHASE_FETCH ? "fetch" :
                            log[i].phase == PHASE_LOAD ? "load" : "execute";

        if (i)
            fprintf(out, " | ");
        if (log[i].kind == XFER_READ)
            fprintf(out, "%s: read M[0x%02X] = %04X", phase, log[i].addr, log[i].data);
        else if (log[i].kind == XFER_WRITE)
            fprintf(out, "%s: write M[0x%02X] = %04X", phase, log[i].addr, log[i].data);
        else
            fprintf(out, "%s: output port = %u", phase, log[i].data);
    }
}

/* Get the instruction part of a source line: no label, no comment, single spaces. */
static void source_code(const char *line, char *code, size_t size)
{
    const char *p = line, *start;
    size_t n = 0;
    int space = 0;

    while (isspace((unsigned char)*p))
        p++;
    start = p;
    while (isalnum((unsigned char)*p) || *p == '_' || *p == '.')
        p++;
    if (p > start && *p == ':')     /* skip "label:" */
        start = p + 1;
    for (p = start; *p && *p != ';' && n + 1 < size; p++) {
        if (isspace((unsigned char)*p)) {
            space = 1;
            continue;
        }
        if (space && n > 0)
            code[n++] = ' ';
        space = 0;
        code[n++] = *p;
    }
    code[n] = '\0';
}

/* The opcode as 4 binary digits, like "1000". */
static const char *opcode_bits(int op, char *buf)
{
    int i;
    for (i = 0; i < 4; i++)
        buf[i] = ((op >> (3 - i)) & 1) ? '1' : '0';
    buf[4] = '\0';
    return buf;
}

void print_assemble(FILE *out, const Program *prog)
{
    int i, j;

    print_bar(out);
    fprintf(out, "PHASE 1: ASSEMBLE   %s -> machine code\n", prog->source_name);
    fprintf(out, "  Each instruction name is looked up in the ISA table (OPCODE_TABLE in isa.c).\n");
    fprintf(out, "  The table gives the opcode and the format. The operands fill the other bits.\n\n");
    fprintf(out, "  ADDR  %-22s %-20s %-24s HEX\n", "SOURCE", "LOOKUP", "MACHINE CODE (binary)");

    for (i = 0; i < prog->listing_count; i++) {
        const ListingLine *row = &prog->listing[i];
        char code[128], mnemonic[16], lookup[40], bits[32], opbits[8];
        uint16_t word;

        if (row->addr < 0 || row->word_count == 0)
            continue;
        source_code(row->text, code, sizeof code);
        word = prog->words[row->addr];

        if (prog->kinds[row->addr] == KIND_STACK) {
            fprintf(out, "  0x%02X  %-22s %-20s %d words of 0000\n", row->addr, code, "empty words", row->word_count);
            continue;
        }
        if (prog->kinds[row->addr] == KIND_DATA) {
            for (j = 0; j < row->word_count; j++) {
                word = prog->words[row->addr + j];
                binary_plain(word, bits, sizeof bits);
                fprintf(out, "  0x%02X  %-22s %-20s %-24s %04X\n", row->addr + j, j == 0 ? code : "",
                        "data word", bits, word);
            }
            continue;
        }

        /* an instruction: show the table lookup that the assembler did */
        for (j = 0; code[j] && code[j] != ' ' && j < 15; j++)
            mnemonic[j] = (char)toupper((unsigned char)code[j]);
        mnemonic[j] = '\0';
        {
            Decoded d = decode(word);
            snprintf(lookup, sizeof lookup, "%-4s -> %s (%s)", mnemonic, opcode_bits(d.op, opbits),
                     FORMAT_NAMES[decoded_format(&d)]);
        }
        binary_fields(word, bits, sizeof bits);
        fprintf(out, "  0x%02X  %-22s %-20s %-24s %04X\n", row->addr, code, lookup, bits, word);
    }

    fprintf(out, "\n  Labels: ");
    for (i = 0; i < prog->symbols.count; i++)
        fprintf(out, "%s%s=0x%02X", i ? ", " : "", prog->symbols.items[i].name, prog->symbols.items[i].value);
    fprintf(out, "\n");
}

void print_load(FILE *out, const Program *prog, const CPU *cpu, const LoadStep *steps, int count, int color)
{
    char written[MEM_SIZE];
    int i, j;

    print_bar(out);
    fprintf(out, "PHASE 2: LOAD   the loader writes each word into RAM over the system bus\n");
    fprintf(out, "  RAM before loading: all %d words are 0000\n\n", MEM_SIZE);
    fprintf(out, "  LOAD  ADDRESS BUS  DATA BUS  CONTROL BUS  RAM WORD           WHAT IT IS\n");

    for (i = 0; i < count; i++) {
        char what[MAX_NAME + 32];

        if (prog->kinds[i] == KIND_CODE) {
            disassemble(steps[i].word, &prog->symbols, what, sizeof what);
        } else if (prog->kinds[i] == KIND_STACK) {
            snprintf(what, sizeof what, "stack space");
        } else {
            snprintf(what, sizeof what, "data");
            for (j = 0; j < prog->variables.count; j++)
                if (prog->variables.items[j].value == i)
                    snprintf(what, sizeof what, "data (%s)", prog->variables.items[j].name);
        }
        fprintf(out, "  %-4d  0x%02X         %04X      %-11s  RAM[0x%02X] = %04X  %s\n",
                i + 1, steps[i].addr, steps[i].word, steps[i].control, steps[i].addr, steps[i].word, what);
    }

    memset(written, 0, sizeof written);
    memset(written, 1, (size_t)count);
    fprintf(out, "\n  RAM after loading (each [....] word was just written by the loader):\n");
    print_memory(out, prog, cpu->memory.words, -1, written, color);
    fprintf(out, "\n  CPU reset: PC = 0x00 (the entry point), all registers and flags are 0\n");
}

void print_initial(FILE *out, const Program *prog, const CPU *cpu, int color)
{
    print_bar(out);
    fprintf(out, "PHASE 3: EXECUTE   fetch, decode, execute until HALT\n");
    print_bar(out);
    fprintf(out, "%sSTEP 0  | program loaded from %s%s\n",
            color ? COLOR_HEAD : "", prog->source_name, color ? COLOR_END : "");
    fprintf(out, "  cpu    : PC=%02X IR=%04X MAR=%02X MDR=%04X   flags ",
            cpu->pc.value, cpu->ir.value, cpu->mar.value, cpu->mdr.value);
    print_flags(out, &cpu->flags);
    fprintf(out, "\n  regs   : ");
    print_regs(out, cpu->regs.values, NULL);
    fprintf(out, "\n  vars   : ");
    print_vars(out, prog, cpu->memory.words);
    fprintf(out, "\n");
    print_memory(out, prog, cpu->memory.words, (int)cpu->pc.value, NULL, color);
}

void print_step(FILE *out, const Program *prog, const StepRecord *rec, int color)
{
    char bits[32];

    binary_fields((uint16_t)rec->ir, bits, sizeof bits);
    print_bar(out);
    fprintf(out, "%sSTEP %-3d| PC 0x%02X | IR %04X = %-19s | %s%s\n", color ? COLOR_HEAD : "",
            rec->step, rec->pc, rec->ir, bits, rec->text, color ? COLOR_END : "");
    fprintf(out, "  result : %s\n", rec->effect);
    if (rec->has_output)
        fprintf(out, "  OUTPUT : %u\n", rec->output);

    fprintf(out, "  bus    : ");
    print_bus(out, rec->bus_log, rec->bus_count);

    fprintf(out, "\n  alu    : ");
    if (rec->alu.used)
        fprintf(out, "%s  A=%04X  B=%04X  ->  %04X", rec->alu.op, rec->alu.a, rec->alu.b, rec->alu.result);
    else
        fprintf(out, "not used");

    fprintf(out, "\n  cpu    : PC=%02X IR=%04X MAR=%02X MDR=%04X   flags ",
            rec->next_pc, rec->ir, rec->mar, rec->mdr);
    print_flags(out, &rec->flags);
    fprintf(out, "   cycles %d\n  regs   : ", rec->cycles);
    print_regs(out, rec->regs, rec->reg_written);
    fprintf(out, "\n  vars   : ");
    print_vars(out, prog, rec->mem);
    fprintf(out, "\n");
    print_memory(out, prog, rec->mem, rec->halted ? -1 : rec->next_pc, rec->mem_written, color);
}
