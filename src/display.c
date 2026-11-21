/*
 * display.c
 * Prints the program and the machine state in the terminal.
 *
 * How to read the RAM dump:
 *     >   the next instruction (the word PC points to)
 *     *   a word the last instruction wrote
 *   Every word is shown in binary, in hex, and as two ASCII characters
 *   (one per byte, '.' when the byte is not a printable character).
 *   The CPU registers and the ALU are shown the same way: binary, number and ASCII.
 *
 * In the register list, * after a register means the last instruction changed it.
 */
#include <string.h>

#include "display.h"

#define COLOR_MARK  "\033[1;32m"
#define COLOR_WRITE "\033[1;31m"
#define COLOR_HEAD  "\033[1;36m"
#define COLOR_END   "\033[0m"

static void print_bar(FILE *out, char c)
{
    char bar[79];

    memset(bar, c, 78);
    bar[78] = '\0';
    fprintf(out, "%s\n", bar);
}

static void heading(FILE *out, int color, const char *text)
{
    print_bar(out, '=');
    fprintf(out, "%s%s%s\n", color ? COLOR_HEAD : "", text, color ? COLOR_END : "");
}

/* n bits of value in binary, like "00000101". */
static const char *bits_of(unsigned value, int n, char *buf)
{
    int i;
    for (i = 0; i < n; i++)
        buf[i] = ((value >> (n - 1 - i)) & 1) ? '1' : '0';
    buf[n] = '\0';
    return buf;
}

void word_ascii(uint16_t word, char out[3])
{
    unsigned high = (word >> 8) & 0xFF, low = word & 0xFF;
    out[0] = (high >= 0x20 && high < 0x7F) ? (char)high : '.';
    out[1] = (low >= 0x20 && low < 0x7F) ? (char)low : '.';
    out[2] = '\0';
}

void value_ascii(unsigned value, int bits, char out[3])
{
    if (bits <= 8) {
        unsigned byte = value & 0xFF;
        out[0] = (byte >= 0x20 && byte < 0x7F) ? (char)byte : '.';
        out[1] = '\0';
    } else {
        word_ascii((uint16_t)value, out);
    }
}

const char *word_content(const HandProgram *prog, int addr, uint16_t word, char *buf, size_t size)
{
    int i;

    if (addr < prog->code_count)
        return disassemble(word, &prog->names, buf, size);
    for (i = 0; i < prog->names.count; i++) {
        if (prog->names.items[i].value == addr) {
            snprintf(buf, size, "%s = %u", prog->names.items[i].name, word);
            return buf;
        }
    }
    snprintf(buf, size, "data = %u", word);
    return buf;
}

void print_ram(FILE *out, const HandProgram *prog, const uint16_t *mem, int pc, const char *written, int color)
{
    int addr;

    fprintf(out, "     ADDR  BINARY            HEX   ASCII  CONTENT\n");
    for (addr = 0; addr < prog->word_count; addr++) {
        char bits[17], ascii[3], content[64];
        int is_written = written && written[addr];
        const char *mark = is_written ? "*" : (addr == pc ? ">" : " ");
        const char *start = "", *end = "";

        if (color && is_written) {
            start = COLOR_WRITE;
            end = COLOR_END;
        } else if (color && addr == pc) {
            start = COLOR_MARK;
            end = COLOR_END;
        }
        word_ascii(mem[addr], ascii);
        fprintf(out, "  %s%s  0x%02X  %s  %04X  %s     %s%s\n", start, mark, addr,
                bits_of(mem[addr], 16, bits), mem[addr], ascii,
                word_content(prog, addr, mem[addr], content, sizeof content), end);
    }
}

/* Print the C file, without the comment block at the top. */
static void print_c_source(FILE *out, const char *source)
{
    const char *p = source;
    const char *end = strstr(source, "*/");

    if (strncmp(source, "/*", 2) == 0 && end) {
        p = end + 2;
        while (*p == '\n')
            p++;
    }
    while (*p) {
        const char *nl = strchr(p, '\n');
        int n = nl ? (int)(nl - p) : (int)strlen(p);
        fprintf(out, "    %.*s\n", n, p);
        p += n;
        if (*p == '\n')
            p++;
    }
}

void print_program(FILE *out, const HandProgram *prog, const char *c_source, const CPU *cpu, int color)
{
    char title[128];
    int addr;

    snprintf(title, sizeof title, "PROGRAM: %s  (%s)", prog->name, prog->title);
    heading(out, color, title);

    heading(out, color, "STEP 1: THE C PROGRAM");
    if (c_source)
        print_c_source(out, c_source);
    else
        fprintf(out, "    (could not open %s)\n", prog->c_file);

    heading(out, color, "STEP 2: TRANSLATED BY HAND INTO OUR ASSEMBLY, THEN INTO BINARY");
    fprintf(out, "  ADDR  C LINE                      ASSEMBLY          BINARY (fields)        HEX\n");
    for (addr = 0; addr < prog->word_count; addr++) {
        const ProgramWord *w = &prog->words[addr];
        uint16_t word = bin(w->bits);
        char fields[32];

        if (addr < prog->code_count)
            binary_fields(word, fields, sizeof fields);
        else
            binary_plain(word, fields, sizeof fields);
        fprintf(out, "  0x%02X  %-27s %-17s %-22s %04X\n", addr, w->c_line, w->assembly, fields, word);
    }

    heading(out, color, "STEP 3: THE BINARY PUT INTO RAM BY HAND (the program starts at 0x00, PC = 00)");
    print_ram(out, prog, cpu->memory.words, (int)cpu->pc.value, NULL, color);

    heading(out, color, "STEP 4: RUN IT (each instruction = fetch, decode, execute = 3 clock ticks)");
}

static void print_register(FILE *out, const char *name, unsigned value, int bits, int changed)
{
    char b[17], hex[8], ascii[3];
    snprintf(hex, sizeof hex, "0x%0*X", bits / 4, value);
    value_ascii(value, bits, ascii);
    fprintf(out, "    %-5s %-16s  %-6s  %-7u  %s%s\n", name, bits_of(value, bits, b), hex, value, ascii,
            changed ? "  *" : "");
}

void print_step(FILE *out, const HandProgram *prog, const StepRecord *rec, int color)
{
    char title[160], b1[17], b2[17], b3[17], a1[3], a2[3], a3[3];
    int i;

    snprintf(title, sizeof title, "INSTRUCTION %d   (clock ticks %lu to %lu)   %s",
             rec->step, rec->clock[0], rec->clock[2], rec->text);
    heading(out, color, title);
    fprintf(out, "  tick %-3lu FETCH    %s\n", rec->clock[0], rec->fetch);
    fprintf(out, "  tick %-3lu DECODE   %s\n", rec->clock[1], rec->decode);
    fprintf(out, "  tick %-3lu EXECUTE  %s\n", rec->clock[2], rec->effect);
    if (rec->has_output)
        fprintf(out, "  OUTPUT   %u\n", rec->output);

    /* the CPU */
    fprintf(out, "\n  CPU special purpose registers\n");
    fprintf(out, "    %-5s %-16s  %-6s  %-7s  %s\n", "", "binary", "hex", "decimal", "ASCII");
    print_register(out, "PC", (unsigned)rec->next_pc, 8, 0);
    print_register(out, "IR", rec->ir, 16, 0);
    print_register(out, "MAR", rec->mar, 8, 0);
    print_register(out, "MDR", rec->mdr, 16, 0);
    fprintf(out, "    FLAGS  Z=%d N=%d C=%d V=%d\n", rec->flags.z, rec->flags.n, rec->flags.c, rec->flags.v);

    fprintf(out, "  CPU general purpose registers (binary, decimal, ASCII)\n");
    for (i = 0; i < 4; i++) {
        value_ascii(rec->regs[i], 16, a1);
        value_ascii(rec->regs[i + 4], 16, a2);
        fprintf(out, "    %-2s = %s (%5u) %s%s    %-2s = %s (%5u) %s%s\n",
                REG_NAMES[i], bits_of(rec->regs[i], 16, b1), rec->regs[i], a1, rec->reg_written[i] ? " *" : "  ",
                REG_NAMES[i + 4], bits_of(rec->regs[i + 4], 16, b2), rec->regs[i + 4], a2,
                rec->reg_written[i + 4] ? " *" : "");
    }

    fprintf(out, "  CPU ALU (binary, decimal, ASCII)\n");
    if (rec->alu.used) {
        value_ascii(rec->alu.a, 16, a1);
        value_ascii(rec->alu.b, 16, a2);
        value_ascii(rec->alu.result, 16, a3);
        fprintf(out, "    %s  A = %s (%u) %s   B = %s (%u) %s\n         result = %s (%u) %s\n", rec->alu.op,
                bits_of(rec->alu.a, 16, b1), rec->alu.a, a1, bits_of(rec->alu.b, 16, b2), rec->alu.b, a2,
                bits_of(rec->alu.result, 16, b3), rec->alu.result, a3);
    }
    else
        fprintf(out, "    not used by this instruction\n");

    /* the RAM */
    fprintf(out, "\n  RAM (> next instruction, * just written)\n");
    print_ram(out, prog, rec->mem, rec->halted ? -1 : rec->next_pc, rec->mem_written, color);
}
