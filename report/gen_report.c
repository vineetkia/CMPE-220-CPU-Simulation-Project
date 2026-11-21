/*
 * gen_report.c
 * Makes the tables and traces for our report. Run it with "make report".
 *
 * It runs our two programs on the simulator and writes LaTeX files into
 * report/generated/. The Makefile then runs pdflatex on report/report.tex.
 * Because the tables come straight from the simulator, the PDF always shows
 * what the code really does.
 *
 * Run it from the project folder (the Makefile does that).
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpu.h"
#include "display.h"
#include "programs.h"

/* ===================================================================== */
/* Small helpers                                                          */
/* ===================================================================== */

static void die(const char *message, const char *detail)
{
    fprintf(stderr, "gen_report: %s %s\n", message, detail ? detail : "");
    exit(1);
}

static char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    char *text;
    long size;

    if (!f)
        die("can't open", path);
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    text = calloc((size_t)size + 1, 1);
    if (!text || fread(text, 1, (size_t)size, f) != (size_t)size)
        die("can't read", path);
    fclose(f);
    return text;
}

static FILE *open_generated(const char *name)
{
    char path[256];
    FILE *f;

    snprintf(path, sizeof path, "report/generated/%s", name);
    f = fopen(path, "w");
    if (!f)
        die("can't write", path);
    return f;
}

/* A few rotating buffers, so we can use several results in one printf. */
static char *scratch(void)
{
    static char buffers[8][1024];
    static int next;
    next = (next + 1) % 8;
    buffers[next][0] = '\0';
    return buffers[next];
}

/* Escape special LaTeX characters. */
static const char *tex(const char *text)
{
    char *out = scratch();
    size_t n = 0;

    for (; *text && n < 1000; text++) {
        const char *rep = NULL;
        switch (*text) {
        case '\\': rep = "\\textbackslash{}"; break;
        case '&':  rep = "\\&"; break;
        case '%':  rep = "\\%"; break;
        case '$':  rep = "\\$"; break;
        case '#':  rep = "\\#"; break;
        case '_':  rep = "\\_"; break;
        case '{':  rep = "\\{"; break;
        case '}':  rep = "\\}"; break;
        case '~':  rep = "\\textasciitilde{}"; break;
        case '^':  rep = "\\textasciicircum{}"; break;
        }
        if (rep) {
            strcpy(out + n, rep);
            n += strlen(rep);
        } else {
            out[n++] = *text;
        }
    }
    out[n] = '\0';
    return out;
}

/* Like tex(), but "<-" and "->" become real arrows and "3 x 2" gets a times sign. */
static const char *arrows(const char *text)
{
    const char *in = tex(text);
    char *out = scratch();
    size_t n = 0;

    for (; *in && n < 1000; in++) {
        if (in[0] == '<' && in[1] == '-') {
            strcpy(out + n, "$\\leftarrow$");
            n += strlen(out + n);
            in++;
        } else if (in[0] == '-' && in[1] == '>') {
            strcpy(out + n, "$\\rightarrow$");
            n += strlen(out + n);
            in++;
        } else if (strncmp(in, " x ", 3) == 0 && n > 0 && isdigit((unsigned char)out[n - 1]) &&
                   isdigit((unsigned char)in[3])) {
            strcpy(out + n, " $\\times$ ");
            n += strlen(out + n);
            in += 2;
        } else {
            out[n++] = *in;
        }
    }
    out[n] = '\0';
    return out;
}

static const char *bits_of(unsigned value, int n)
{
    char *out = scratch();
    int i;
    for (i = 0; i < n; i++)
        out[i] = ((value >> (n - 1 - i)) & 1) ? '1' : '0';
    out[n] = '\0';
    return out;
}

/* ===================================================================== */
/* Instruction set tables                                                 */
/* ===================================================================== */

#define L "$\\leftarrow$"

static const struct {
    const char *name, *pattern, *format, *what, *flags;
} ENCODING[] = {
    {"HALT", "0000 0000 0000 0000", "N", "stop the CPU", "none"},
    {"ADD", "0001 ddd sss ttt 000", "R", "rd " L " rs + rt", "Z N C V"},
    {"SUB", "0010 ddd sss ttt 000", "R", "rd " L " rs $-$ rt", "Z N C V"},
    {"MUL", "0011 ddd sss ttt 000", "R", "rd " L " rs $\\times$ rt", "Z N C V"},
    {"AND", "0100 ddd sss ttt 000", "R", "rd " L " rs AND rt", "Z N"},
    {"OR", "0101 ddd sss ttt 000", "R", "rd " L " rs OR rt", "Z N"},
    {"ADDI", "0110 ddd sss iiiiii", "I", "rd " L " rs + imm6", "Z N C V"},
    {"LI", "0111 ddd iiiiiiiii", "L", "rd " L " imm9", "none"},
    {"LD", "1000 rrr 0 aaaaaaaa", "M", "r " L " RAM[addr8]", "none"},
    {"LD", "1000 rrr 1 bbb iiiii", "MX", "r " L " RAM[rb+imm5]", "none"},
    {"ST", "1001 rrr 0 aaaaaaaa", "M", "RAM[addr8] " L " r", "none"},
    {"ST", "1001 rrr 1 bbb iiiii", "MX", "RAM[rb+imm5] " L " r", "none"},
    {"CMP", "1010 000 sss ttt 000", "R", "flags from rs $-$ rt", "Z N C V"},
    {"Bcc", "1011 cccc aaaaaaaa", "B", "if cond is true: PC " L " addr8", "none"},
    {"CALL", "1100 0000 aaaaaaaa", "B", "push PC, then PC " L " addr8", "none"},
    {"RET", "1101 0000 0000 0000", "N", "pop PC from the stack", "none"},
    {"PUSH", "1110 rrr 0 0000 0000", "S", "SP " L " SP$-$1, RAM[SP] " L " r", "none"},
    {"POP", "1110 rrr 1 0000 0000", "S", "r " L " RAM[SP], SP " L " SP+1", "none"},
    {"OUT", "1111 rrr 0 0000 0000", "S", "print r", "none"},
};

static void isa_tables(void)
{
    static const struct { const char *when, *c_op; } conds[] = {
        {"always", "none"}, {"Z = 1", "=="}, {"Z = 0", "!="}, {"N $\\ne$ V", "<"},
        {"N = V", ">="}, {"Z = 0 and N = V", ">"}, {"Z = 1 or N $\\ne$ V", "<="},
    };
    FILE *f = open_generated("isa_table.tex");
    size_t i;

    for (i = 0; i < sizeof ENCODING / sizeof ENCODING[0]; i++) {
        char opcode[8];
        memcpy(opcode, ENCODING[i].pattern, 4);
        opcode[4] = '\0';
        fprintf(f, "\\texttt{%s} & \\texttt{%s} & %s & \\texttt{\\textbf{%s} %s} & %s & %s\\\\\n",
                opcode, ENCODING[i].name, ENCODING[i].format, opcode, ENCODING[i].pattern + 5,
                ENCODING[i].what, ENCODING[i].flags);
    }
    fclose(f);

    f = open_generated("cond_table.tex");
    for (i = 0; i < 7; i++)
        fprintf(f, "\\texttt{%s} & \\texttt{%s} & %s & \\texttt{%s}\\\\\n", bits_of((unsigned)i, 4),
                cond_name((int)i), conds[i].when, tex(conds[i].c_op));
    fclose(f);
}

/* Draw every instruction format with TikZ. Each bit is 0.62 cm wide. */
static void format_figure(void)
{
    static const char *const names[FMT_COUNT] = {
        "R (ALU, CMP)", "I (ADDI)", "L (LI)", "M (LD/ST address)", "MX (LD/ST [rb+imm5])",
        "B (Bcc, CALL)", "S (PUSH/POP/OUT)", "N (HALT, RET)"
    };
    const double scale = 0.62;
    FILE *f = open_generated("formats.tex");
    double y = 0;
    int b, i;

    for (b = 0; b < 16; b++)
        fprintf(f, "\\node[font=\\tiny\\ttfamily, text=dim] at (%.3f, -0.3) {%d};\n", (15 - b + 0.5) * scale, b);
    for (i = 0; i < FMT_COUNT; i++) {
        const Field *field;
        double x = 0.0;

        y -= 0.85;
        fprintf(f, "\\node[anchor=east, font=\\footnotesize\\sffamily] at (-0.2, %.2f) {%s};\n", y, tex(names[i]));
        for (field = FORMATS[i]; field->name; field++) {
            double w = field->width * scale;
            const char *fill = "fld";
            int all_zero = strspn(field->name, "0") == strlen(field->name);

            if (strcmp(field->name, "op") == 0)
                fill = "opc";
            else if (all_zero || strncmp(field->name, "m=", 2) == 0)
                fill = "fixed";
            fprintf(f, "\\draw[fill=%s] (%.3f,%.2f) rectangle (%.3f,%.2f);\n", fill, x, y - 0.32, x + w, y + 0.32);
            fprintf(f, "\\node[font=\\scriptsize\\ttfamily] at (%.3f,%.2f) {%s};\n", x + w / 2, y,
                    (all_zero && field->width > 4) ? "unused (0)" : tex(field->name));
            x += w;
        }
    }
    fclose(f);
}

/* The CPU struct from src/cpu.h, so the report shows how the CPU looks in our program. */
static void cpu_struct(void)
{
    char *header = read_file("src/cpu.h");
    const char *start = strstr(header, "/* The whole computer. */");
    const char *end = start ? strstr(start, "} CPU;") : NULL;
    FILE *f;

    if (!start || !end)
        die("can't find the CPU struct in", "src/cpu.h");
    f = open_generated("cpu_struct.tex");
    fprintf(f, "\\begin{lstlisting}[style=csrc]\n%.*s} CPU;\n\\end{lstlisting}\n",
            (int)(end - start), start);
    fclose(f);
    free(header);
}

/* ===================================================================== */
/* One program: C source, hand translation, RAM, trace                    */
/* ===================================================================== */

/* The C file without the comment block at the top. */
static void c_listing(const HandProgram *prog, const char *key)
{
    char *source = read_file(prog->c_file);
    const char *p = source, *end = strstr(source, "*/");
    char name[64];
    FILE *f;

    if (strncmp(source, "/*", 2) == 0 && end) {
        p = end + 2;
        while (*p == '\n')
            p++;
    }
    snprintf(name, sizeof name, "%s_c.tex", key);
    f = open_generated(name);
    fprintf(f, "\\begin{lstlisting}[style=csrc]\n%s\\end{lstlisting}\n", p);
    fclose(f);
    free(source);
}

/* Address, C line, our assembly, our binary, hex. */
static void translation_table(const HandProgram *prog, const char *key)
{
    char name[64];
    FILE *f;
    int addr;

    snprintf(name, sizeof name, "%s_translate.tex", key);
    f = open_generated(name);
    for (addr = 0; addr < prog->word_count; addr++) {
        const ProgramWord *w = &prog->words[addr];
        uint16_t word = bin(w->bits);
        char fields[32];

        if (addr < prog->code_count)
            binary_fields(word, fields, sizeof fields);
        else
            binary_plain(word, fields, sizeof fields);
        fprintf(f, "%s\\texttt{%02X} & \\texttt{%s} & \\texttt{%s} & \\texttt{%s} & \\texttt{%04X}\\\\\n",
                addr < prog->code_count ? "" : "\\rowcolor{datac}", addr, tex(w->c_line),
                tex(w->assembly), fields, word);
    }
    fclose(f);
}

/* RAM rows: address, binary, hex, ASCII, what it is. pc and written add the colors. */
static void ram_rows(FILE *f, const HandProgram *prog, const uint16_t *mem, int pc, const char *written)
{
    int addr;

    for (addr = 0; addr < prog->word_count; addr++) {
        char ascii[3], content[64];
        const char *start = "", *close = "";

        word_ascii(mem[addr], ascii);
        word_content(prog, addr, mem[addr], content, sizeof content);
        if (written && written[addr]) {
            fprintf(f, "\\rowcolor{wrt}");
            start = "\\textcolor{white}{\\textbf{";
            close = "}}";
        } else if (addr == pc) {
            fprintf(f, "\\rowcolor{pcc}");
        } else if (addr >= prog->code_count) {
            fprintf(f, "\\rowcolor{datac}");
        }
        fprintf(f, "%s%02X%s & %s%s%s & %s%04X%s & %s%s%s & %s%s%s\\\\\n",
                start, addr, close, start, bits_of(mem[addr], 16), close, start, mem[addr], close,
                start, tex(ascii), close, start, tex(content), close);
    }
}

static void ram_table(const HandProgram *prog, const char *key, const uint16_t *mem)
{
    char name[64];
    FILE *f;

    snprintf(name, sizeof name, "%s_ram.tex", key);
    f = open_generated(name);
    ram_rows(f, prog, mem, 0, NULL);
    fclose(f);
}

/* A value's ASCII in square brackets, escaped for LaTeX, like "[.P]". */
static const char *ascii_of(unsigned value, int bits)
{
    char raw[3], *out = scratch();
    value_ascii(value, bits, raw);
    snprintf(out, 1024, "[%s]", tex(raw));
    return out;
}

/* One box of the trace: the clock ticks, the CPU and the RAM after one instruction. */
static void step_box(FILE *f, const HandProgram *prog, const StepRecord *rec)
{
    char bits[32];
    int i;

    fprintf(f, "\\begin{tcolorbox}[stepbox, title={Instruction %d \\hfill ticks %lu to %lu \\quad %s}]\n",
            rec->step, rec->clock[0], rec->clock[2], tex(rec->text));

    /* the three clock ticks */
    fprintf(f, "\\textcolor{dim}{tick %lu} \\textbf{F} MAR%s%02X, MDR%sRAM[%02X]=%04X, IR%sMDR, PC%s%02X\\\\\n",
            rec->clock[0], L, rec->pc, L, rec->pc, rec->ir, L, L, (rec->pc + 1) & 0xFF);
    fprintf(f, "\\textcolor{dim}{tick %lu} \\textbf{D} %s = %s\\\\\n", rec->clock[1],
            binary_fields((uint16_t)rec->ir, bits, sizeof bits), tex(rec->text));
    fprintf(f, "\\textcolor{dim}{tick %lu} \\textbf{E} %s", rec->clock[2], arrows(rec->effect));
    if (rec->has_output)
        fprintf(f, " \\hfill \\colorbox{outc}{\\textbf{OUTPUT: %u}}", rec->output);
    fprintf(f, "\\\\[2pt]\n");

    /* special purpose registers */
    fprintf(f, "\\textcolor{dim}{special purpose registers (binary [ASCII])}\\\\\n");
    fprintf(f, "PC~~%s %s \\hfill IR~~%s %s\\\\\n", bits_of((unsigned)rec->next_pc, 8),
            ascii_of((unsigned)rec->next_pc, 8), bits_of(rec->ir, 16), ascii_of(rec->ir, 16));
    fprintf(f, "MAR %s %s \\hfill MDR %s %s\\\\\n", bits_of(rec->mar, 8), ascii_of(rec->mar, 8),
            bits_of(rec->mdr, 16), ascii_of(rec->mdr, 16));
    fprintf(f, "FLAGS Z=%d N=%d C=%d V=%d\\\\[2pt]\n", rec->flags.z, rec->flags.n, rec->flags.c, rec->flags.v);

    /* general purpose registers, changed ones in red */
    fprintf(f, "\\textcolor{dim}{general purpose registers (binary, decimal [ASCII])}\\\\\n");
    for (i = 0; i < 4; i++) {
        int j;
        for (j = 0; j < 2; j++) {
            int r = i + 4 * j;
            const char *text = scratch();
            snprintf((char *)text, 1024, "%s %s %u %s", REG_NAMES[r], bits_of(rec->regs[r], 16), rec->regs[r],
                     ascii_of(rec->regs[r], 16));
            if (j)
                fprintf(f, " \\hfill ");
            if (rec->reg_written[r])
                fprintf(f, "\\textcolor{wrt}{\\textbf{%s}}", text);
            else
                fprintf(f, "%s", text);
        }
        fprintf(f, "\\\\\n");
    }

    /* ALU */
    if (rec->alu.used) {
        const char *sign = strcmp(rec->alu.op, "ADD") == 0 ? "+" :
                           strcmp(rec->alu.op, "SUB") == 0 ? "$-$" :
                           strcmp(rec->alu.op, "MUL") == 0 ? "$\\times$" : rec->alu.op;
        fprintf(f, "\\textcolor{dim}{ALU} %s: %d %s %d = %u %s \\hfill %s\\\\[2pt]\n", rec->alu.op,
                to_signed16((int)rec->alu.a), sign, to_signed16((int)rec->alu.b), rec->alu.result,
                ascii_of(rec->alu.result, 16), bits_of(rec->alu.result, 16));
    } else {
        fprintf(f, "\\textcolor{dim}{ALU not used}\\\\[2pt]\n");
    }

    /* RAM */
    fprintf(f, "{\\setlength{\\tabcolsep}{2pt}\\begin{tabular}{@{}lllll@{}}\n");
    fprintf(f, "\\textcolor{dim}{addr} & \\textcolor{dim}{binary} & \\textcolor{dim}{hex} & "
               "\\textcolor{dim}{ascii} & \\textcolor{dim}{content}\\\\\n");
    ram_rows(f, prog, rec->mem, rec->halted ? -1 : rec->next_pc, rec->mem_written);
    fprintf(f, "\\end{tabular}}\n\\end{tcolorbox}");
}

static void run_and_trace(const HandProgram *prog, const char *key, FILE *results)
{
    static CPU cpu;
    StepRecord rec;
    char name[64], err[256];
    FILE *f;
    int i = 0;

    cpu_init(&cpu);
    put_in_ram(&cpu.memory, prog);
    ram_table(prog, key, cpu.memory.words);

    snprintf(name, sizeof name, "%s_trace.tex", key);
    f = open_generated(name);
    while (!cpu.halted) {
        if (cpu_step(&cpu, &prog->names, &rec, err, sizeof err) != 0)
            die("program failed:", err);
        fprintf(f, i % 2 == 0 ? "\\noindent" : "\\hfill");
        step_box(f, prog, &rec);
        if (i % 2 == 1)
            fprintf(f, "\\par\\vspace{3pt}\n");
        i++;
    }
    if (i % 2 == 1)
        fprintf(f, "\\par\\vspace{3pt}\n");
    fclose(f);

    fprintf(results, "\\newcommand{\\%sSteps}{%d}\\newcommand{\\%sTicks}{%lu}\\newcommand{\\%sOut}{%u}"
            "\\newcommand{\\%sWords}{%d}\n", key, cpu.steps, key, cpu.clock.cycle, key,
            cpu.out_port.count ? cpu.out_port.values[0] : 0, key, prog->word_count);
}

int main(void)
{
    FILE *results;

    isa_tables();
    format_figure();
    cpu_struct();

    results = open_generated("results.tex");

    c_listing(find_program("add"), "add");
    translation_table(find_program("add"), "add");
    run_and_trace(find_program("add"), "add", results);

    c_listing(find_program("multiply"), "mul");
    translation_table(find_program("multiply"), "mul");
    run_and_trace(find_program("multiply"), "mul", results);

    fclose(results);
    printf("wrote the report tables into report/generated/\n");
    return 0;
}
