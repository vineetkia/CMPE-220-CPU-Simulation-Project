/*
 * gen_report.c
 * Makes the tables and traces for our report. Run it with "make report".
 *
 * What it does:
 *   1. assembles, loads and runs the sample (3!), program 1 (8!) and
 *      program 2 (8^2) on the simulator
 *   2. writes the tables and traces as LaTeX files into report/generated/
 * The Makefile then runs pdflatex on report/report.tex.
 *
 * Because the tables come straight from the simulator, the PDF always matches
 * what the code really does.
 *
 * Run it from the project folder (the Makefile does that).
 */
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "assembler.h"
#include "compiler.h"
#include "cpu.h"
#include "display.h"
#include "loader.h"

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

static FILE *open_out(const char *path)
{
    FILE *f = fopen(path, "w");
    if (!f)
        die("can't write", path);
    return f;
}

static FILE *open_generated(const char *name)
{
    char path[256];
    snprintf(path, sizeof path, "report/generated/%s", name);
    return open_out(path);
}

/* A few rotating buffers, so we can use several results in one printf. */
static char *scratch(void)
{
    static char buffers[8][4096];
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

    for (; *text && n < 4000; text++) {
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

/* Like tex(), but "<-" becomes a real arrow and "3 x 2" gets a times sign. */
static const char *arrows(const char *text)
{
    const char *in = tex(text);
    char *out = scratch();
    size_t n = 0;

    for (; *in && n < 4000; in++) {
        if (in[0] == '<' && in[1] == '-') {
            strcpy(out + n, "$\\leftarrow$");
            n += strlen("$\\leftarrow$");
            in++;
        } else if (strncmp(in, " x ", 3) == 0 && n > 0 && isdigit((unsigned char)out[n - 1]) &&
                   isdigit((unsigned char)in[3])) {
            strcpy(out + n, " $\\times$ ");
            n += strlen(" $\\times$ ");
            in += 2;
        } else {
            out[n++] = *in;
        }
    }
    out[n] = '\0';
    return out;
}

/* Replace every " x " with a times sign. */
static const char *times_signs(const char *text)
{
    char *out = scratch();
    size_t n = 0;

    for (; *text && n < 4000; text++) {
        if (strncmp(text, " x ", 3) == 0) {
            strcpy(out + n, " $\\times$ ");
            n += strlen(" $\\times$ ");
            text += 2;
        } else {
            out[n++] = *text;
        }
    }
    out[n] = '\0';
    return out;
}

static char *copy_string(const char *fmt, ...)
{
    va_list args;
    int length;
    char *s;

    va_start(args, fmt);
    length = vsnprintf(NULL, 0, fmt, args);
    va_end(args);
    s = malloc((size_t)length + 1);
    if (!s)
        die("out of memory", NULL);
    va_start(args, fmt);
    vsnprintf(s, (size_t)length + 1, fmt, args);
    va_end(args);
    return s;
}

/* Split "code ; comment" into its two halves (both trimmed). */
static void split_comment(const char *line, char *code, char *comment)
{
    const char *semi = strchr(line, ';');
    size_t n = semi ? (size_t)(semi - line) : strlen(line);
    const char *c;

    memcpy(code, line, n);
    code[n] = '\0';
    while (n > 0 && isspace((unsigned char)code[n - 1]))
        code[--n] = '\0';
    comment[0] = '\0';
    if (semi) {
        c = semi + 1;
        while (isspace((unsigned char)*c))
            c++;
        strcpy(comment, c);
        n = strlen(comment);
        while (n > 0 && isspace((unsigned char)comment[n - 1]))
            comment[--n] = '\0';
    }
}

/* If code starts with "label:", split it off. Returns 1 if there was a label. */
static int split_label(const char *code, char *label, char *rest)
{
    const char *p = code;
    const char *start;

    while (isspace((unsigned char)*p))
        p++;
    start = p;
    while (isalnum((unsigned char)*p) || *p == '_' || *p == '.')
        p++;
    if (p > start && *p == ':') {
        memcpy(label, start, (size_t)(p - start));
        label[p - start] = '\0';
        strcpy(rest, p + 1);
        return 1;
    }
    label[0] = '\0';
    strcpy(rest, code);
    return 0;
}

/* Squeeze all runs of spaces into one, and trim the ends. */
static void squeeze(const char *in, char *out)
{
    int space = 0;
    char *start = out;

    for (; *in; in++) {
        if (isspace((unsigned char)*in)) {
            space = 1;
            continue;
        }
        if (space && out > start)
            *out++ = ' ';
        space = 0;
        *out++ = *in;
    }
    *out = '\0';
}

/* ===================================================================== */
/* Instruction set tables                                                 */
/* ===================================================================== */

#define LARROW "$\\leftarrow$"

/* The hex column is made by really assembling the example. */
static const struct {
    const char *name, *pattern, *example, *format, *what, *flags;
} PATTERNS[] = {
    {"HALT", "0000 0000 0000 0000", "HALT", "N", "stop the CPU", "none"},
    {"ADD", "0001 ddd sss ttt 000", "ADD R1, R2, R3", "R", "rd " LARROW " rs + rt", "Z N C V"},
    {"SUB", "0010 ddd sss ttt 000", "SUB R4, R4, R1", "R", "rd " LARROW " rs $-$ rt", "Z N C V"},
    {"MUL", "0011 ddd sss ttt 000", "MUL R1, R1, R2", "R", "rd " LARROW " rs $\\times$ rt", "Z N C V"},
    {"AND", "0100 ddd sss ttt 000", "AND R3, R1, R2", "R", "rd " LARROW " rs AND rt", "Z N"},
    {"OR", "0101 ddd sss ttt 000", "OR R3, R1, R2", "R", "rd " LARROW " rs OR rt", "Z N"},
    {"ADDI", "0110 ddd sss iiiiii", "ADDI R2, R2, -1", "I", "rd " LARROW " rs + imm6", "Z N C V"},
    {"LI", "0111 ddd iiiiiiiii", "LI R1, 8", "L", "rd " LARROW " imm9", "none"},
    {"LD", "1000 rrr 0 aaaaaaaa", "LD R2, 0x09", "M", "r " LARROW " M[addr8]", "none"},
    {"LD", "1000 rrr 1 bbb iiiii", "LD R1, [SP+2]", "MX", "r " LARROW " M[rb+imm5]", "none"},
    {"ST", "1001 rrr 0 aaaaaaaa", "ST R1, 0x0A", "M", "M[addr8] " LARROW " r", "none"},
    {"ST", "1001 rrr 1 bbb iiiii", "ST R1, [R3-1]", "MX", "M[rb+imm5] " LARROW " r", "none"},
    {"CMP", "1010 000 sss ttt 000", "CMP R1, R0", "R", "flags from rs $-$ rt", "Z N C V"},
    {"Bcc", "1011 cccc aaaaaaaa", "BNE 0x02", "B", "if cond is true: PC " LARROW " addr8", "none"},
    {"CALL", "1100 0000 aaaaaaaa", "CALL 0x03", "B", "push PC, then PC " LARROW " addr8", "none"},
    {"RET", "1101 0000 0000 0000", "RET", "N", "pop PC from the stack", "none"},
    {"PUSH", "1110 rrr 0 0000 0000", "PUSH R1", "S", "SP " LARROW " SP$-$1, M[SP] " LARROW " r", "none"},
    {"POP", "1110 rrr 1 0000 0000", "POP R1", "S", "r " LARROW " M[SP], SP " LARROW " SP+1", "none"},
    {"OUT", "1111 rrr 0 0000 0000", "OUT R1", "S", "print r", "none"},
};

static void isa_tables(void)
{
    static const struct { const char *name, *when, *c_op; } conds[] = {
        {"JMP", "always", "none"}, {"BEQ", "Z = 1", "=="}, {"BNE", "Z = 0", "!="},
        {"BLT", "N $\\ne$ V", "<"}, {"BGE", "N = V", ">="},
        {"BGT", "Z = 0 and N = V", ">"}, {"BLE", "Z = 1 or N $\\ne$ V", "<="},
    };
    FILE *f = open_generated("isa_table.tex");
    size_t i;

    for (i = 0; i < sizeof PATTERNS / sizeof PATTERNS[0]; i++) {
        static Program prog;
        char err[256], opcode[8];
        const char *rest = strchr(PATTERNS[i].pattern, ' ') + 1;
        uint16_t word;

        if (assemble(PATTERNS[i].example, "example", &prog, err, sizeof err) != 0)
            die("bad example:", err);
        word = prog.words[0];
        program_free(&prog);
        memcpy(opcode, PATTERNS[i].pattern, 4);
        opcode[4] = '\0';
        fprintf(f, "0x%X & \\texttt{%s} & %s & \\texttt{\\textbf{%s} %s} & %s & %s & \\texttt{%s} & \\texttt{%04X}\\\\\n",
                word >> 12, PATTERNS[i].name, PATTERNS[i].format, opcode, rest, PATTERNS[i].what,
                PATTERNS[i].flags, tex(PATTERNS[i].example), word);
    }
    fclose(f);

    f = open_generated("cond_table.tex");
    for (i = 0; i < sizeof conds / sizeof conds[0]; i++)
        fprintf(f, "%d & \\texttt{%s} & %s & \\texttt{%s}\\\\\n", branch_cond(conds[i].name),
                conds[i].name, conds[i].when, tex(conds[i].c_op));
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
            char label[32];
            int all_zero = strspn(field->name, "0") == strlen(field->name);

            if (strcmp(field->name, "op") == 0)
                fill = "opc";
            else if (all_zero || strncmp(field->name, "m=", 2) == 0)
                fill = "fixed";
            if (all_zero && field->width > 4)
                strcpy(label, "unused (0)");
            else
                strcpy(label, field->name);
            fprintf(f, "\\draw[fill=%s] (%.3f,%.2f) rectangle (%.3f,%.2f);\n", fill, x, y - 0.32, x + w, y + 0.32);
            fprintf(f, "\\node[font=\\scriptsize\\ttfamily] at (%.3f,%.2f) {%s};\n", x + w / 2, y, tex(label));
            x += w;
        }
    }
    fclose(f);
}

/* ===================================================================== */
/* Program listings                                                       */
/* ===================================================================== */

/* Table rows: address, hex, binary, label, instruction, comment. */
static void asm_listing(const Program *prog, const char *file_name)
{
    FILE *f = open_generated(file_name);
    int i;

    for (i = 0; i < prog->listing_count; i++) {
        const ListingLine *row = &prog->listing[i];
        char code[512], comment[512], label[MAX_NAME], rest[512], text[512], bits[32];
        uint16_t word;

        if (row->addr < 0 || row->word_count == 0)
            continue;
        split_comment(row->text, code, comment);
        split_label(code, label, rest);
        squeeze(rest, text);
        word = prog->words[row->addr];
        if (prog->kinds[row->addr] == KIND_CODE)
            binary_fields(word, bits, sizeof bits);
        else
            binary_plain(word, bits, sizeof bits);
        fprintf(f, "%s0x%02X & %04X & %s & %s & %s & {\\rmfamily\\footnotesize %s}\\\\\n",
                prog->kinds[row->addr] != KIND_CODE ? "\\rowcolor{datac}" : "",
                row->addr, word, bits, tex(label), tex(text), arrows(comment));
    }
    fclose(f);
}

/* One block of the C translation table: a C line and the instructions made from it. */
typedef struct {
    char *c_text;           /* NULL for no text */
    char *items[256][3];    /* address, hex, assembly */
    int count;
    int active;
} Group;

typedef struct {
    char **rows;
    int count, capacity;
} Rows;

static void add_row(Rows *rows, char *row)
{
    if (rows->count == rows->capacity) {
        rows->capacity = rows->capacity ? rows->capacity * 2 : 64;
        rows->rows = realloc(rows->rows, sizeof(char *) * (size_t)rows->capacity);
        if (!rows->rows)
            die("out of memory", NULL);
    }
    rows->rows[rows->count++] = row;
}

static void flush_group(Group *g, Rows *rows)
{
    int i;

    if (!g->active || (!g->c_text && g->count == 0))
        return;
    if (g->count == 0)
        add_row(rows, copy_string("%s &  &  & \\\\", g->c_text ? g->c_text : ""));
    for (i = 0; i < g->count; i++) {
        add_row(rows, copy_string("%s & %s & %s & %s\\\\", (i == 0 && g->c_text) ? g->c_text : "",
                                  g->items[i][0], g->items[i][1], g->items[i][2]));
        free(g->items[i][0]);
        free(g->items[i][1]);
        free(g->items[i][2]);
    }
    add_row(rows, copy_string("\\midrule"));
}

static void start_group(Group *g, Rows *rows, char *c_text)
{
    flush_group(g, rows);
    free(g->c_text);
    g->c_text = c_text;
    g->count = 0;
    g->active = 1;
}

static void add_item(Group *g, char *addr, char *hex, char *text)
{
    if (g->count < 256) {
        g->items[g->count][0] = addr;
        g->items[g->count][1] = hex;
        g->items[g->count][2] = text;
        g->count++;
    }
}

/* Does comment look like "int name(" or "void name("? */
static int is_signature(const char *comment)
{
    const char *p;

    if (strncmp(comment, "int ", 4) == 0)
        p = comment + 4;
    else if (strncmp(comment, "void ", 5) == 0)
        p = comment + 5;
    else
        return 0;
    if (!(isalnum((unsigned char)*p) || *p == '_'))
        return 0;
    while (isalnum((unsigned char)*p) || *p == '_')
        p++;
    return *p == '(';
}

/* Is the code just "label:" (with nothing after it)? */
static int is_label_only(const char *code)
{
    char label[MAX_NAME + 8], rest[512], text[512];
    if (!split_label(code, label, rest))
        return 0;
    squeeze(rest, text);
    return text[0] == '\0';
}

static void c_translation(const Program *prog)
{
    static Group group;
    Rows rows = {NULL, 0, 0};
    FILE *f;
    int i;

    memset(&group, 0, sizeof group);
    for (i = 0; i < prog->listing_count; i++) {
        const ListingLine *row = &prog->listing[i];
        char code[512], comment[512], label[MAX_NAME + 8], rest[512], text[512], trimmed[512];

        split_comment(row->text, code, comment);
        if (row->addr < 0) {
            squeeze(code, trimmed);
            if (strncmp(row->text, "        ;", 9) == 0) {      /* the compiler copies each C line as a comment */
                start_group(&group, &rows, copy_string("\\texttt{%s}", tex(comment)));
            } else if (strncmp(row->text, "; ", 2) == 0 && is_signature(comment)) {
                start_group(&group, &rows, NULL);
                add_row(&rows, copy_string("\\rowcolor{funcc}\\multicolumn{4}{l}{\\textbf{\\texttt{%s}}}\\\\", tex(comment)));
            } else if (strncmp(row->text, "; data", 6) == 0) {
                start_group(&group, &rows, copy_string("{\\rmfamily\\itshape variables, one word each}"));
            } else if (strncmp(row->text, "; stack", 7) == 0) {
                start_group(&group, &rows, copy_string("{\\rmfamily\\itshape the stack}"));
            } else if (is_label_only(code)) {                       /* a label on its own line */
                if (strcmp(trimmed, "_start:") == 0)
                    start_group(&group, &rows, copy_string("{\\rmfamily\\itshape start-up code added by the compiler}"));
                group.active = 1;
                add_item(&group, copy_string(""), copy_string(""), copy_string("\\textit{%s}", tex(trimmed)));
            }
            continue;
        }

        split_label(code, label, rest);
        squeeze(rest, text);
        group.active = 1;
        if (strncmp(text, ".space", 6) == 0) {
            add_item(&group, copy_string("0x%02X to 0x%02X", row->addr, row->addr + row->word_count - 1),
                     copy_string("0000"), copy_string("%s", tex(copy_string("%s: %s", label, text))));
            continue;
        }
        if (label[0]) {
            char *joined = copy_string("%s: %s", label, text);
            strcpy(text, joined);
            free(joined);
        }
        add_item(&group, copy_string("0x%02X", row->addr), copy_string("%04X", prog->words[row->addr]),
                 copy_string("%s", tex(text)));
    }
    flush_group(&group, &rows);
    free(group.c_text);
    if (rows.count && strcmp(rows.rows[rows.count - 1], "\\midrule") == 0)
        free(rows.rows[--rows.count]);

    f = open_generated("power_translation.tex");
    for (i = 0; i < rows.count; i++) {
        fprintf(f, "%s\n", rows.rows[i]);
        free(rows.rows[i]);
    }
    free(rows.rows);
    fclose(f);
}

/* One row for each block of code, data or stack. */
static void memory_map(const Program *prog, const char *file_name)
{
    FILE *f = open_generated(file_name);
    int start = 0, a, i;

    for (a = 1; a <= prog->word_count; a++) {
        WordKind kind = prog->kinds[start];
        char names[1024] = "";

        if (a < prog->word_count && prog->kinds[a] == kind)
            continue;
        if (kind == KIND_DATA) {
            for (i = 0; i < prog->variables.count; i++) {
                const Symbol *s = &prog->variables.items[i];
                if (s->value >= start && s->value < a)
                    snprintf(names + strlen(names), sizeof names - strlen(names), "%s%s", names[0] ? ", " : "", s->name);
            }
        } else if (kind == KIND_CODE) {     /* just the function names, not loop labels */
            for (i = 0; i < prog->symbols.count; i++) {
                const Symbol *s = &prog->symbols.items[i];
                if (s->value >= start && s->value < a && !strchr(s->name, '.') && prog->kinds[s->value] == KIND_CODE)
                    snprintf(names + strlen(names), sizeof names - strlen(names), "%s%s", names[0] ? ", " : "", s->name);
            }
        } else {
            snprintf(names, sizeof names, "SP starts at 0x%02X and goes down", a);
        }
        fprintf(f, "0x%02X to 0x%02X & %d & %s & %s\\\\\n", start, a - 1, a - start, kind_name(kind), tex(names));
        start = a;
    }
    fclose(f);
}

/* ===================================================================== */
/* Execution traces                                                       */
/* ===================================================================== */

/* The machine state shown in one box. */
typedef struct {
    const uint16_t *regs;
    int pc;
    Flags flags;
    const uint16_t *mem;
} State;

typedef struct {
    CPU cpu;
    LoadStep load[MEM_SIZE];    /* what the loader put on the bus */
    int load_count;
    uint16_t first_regs[NUM_REGS];
    uint16_t first_mem[MEM_SIZE];
    StepRecord *records;
    int count;
} Run;

static void run_program(const Program *prog, Run *run)
{
    char err[256];
    int capacity = 0;

    cpu_init(&run->cpu);
    run->load_count = load_program(&run->cpu, prog->words, prog->word_count, run->load);
    memcpy(run->first_regs, run->cpu.regs.values, sizeof run->first_regs);
    memcpy(run->first_mem, run->cpu.memory.words, sizeof run->first_mem);
    run->records = NULL;
    run->count = 0;
    while (!run->cpu.halted) {
        if (run->count == capacity) {
            capacity = capacity ? capacity * 2 : 64;
            run->records = realloc(run->records, sizeof(StepRecord) * (size_t)capacity);
            if (!run->records)
                die("out of memory", NULL);
        }
        if (cpu_step(&run->cpu, &prog->symbols, &run->records[run->count], err, sizeof err) != 0)
            die("program failed:", err);
        run->count++;
    }
}

static State first_state(const Run *run)
{
    State s;
    s.regs = run->first_regs;
    s.pc = 0;
    memset(&s.flags, 0, sizeof s.flags);
    s.mem = run->first_mem;
    return s;
}

static State record_state(const StepRecord *rec)
{
    State s;
    s.regs = rec->regs;
    s.pc = rec->next_pc;
    s.flags = rec->flags;
    s.mem = rec->mem;
    return s;
}

static void print_bus(FILE *f, const BusTransfer *log, int count, int execute_only)
{
    int i, printed = 0;

    for (i = 0; i < count; i++) {
        if (execute_only && log[i].phase == PHASE_FETCH)
            continue;
        if (printed++)
            fprintf(f, ", ");
        if (log[i].kind == XFER_READ)
            fprintf(f, "read M[%02X]=%04X", log[i].addr, log[i].data);
        else if (log[i].kind == XFER_WRITE)
            fprintf(f, "\\textcolor{wrt}{write M[%02X]=%04X}", log[i].addr, log[i].data);
        else
            fprintf(f, "out %u", log[i].data);
    }
}

/* One box of the trace. rec is NULL for the starting state. */
static void step_box(FILE *f, const Program *prog, const char *title, const StepRecord *rec, State state)
{
    int i, start, a;
    char bits[32];

    fprintf(f, "\\begin{tcolorbox}[stepbox, title={%s}]\n", title);
    if (rec) {
        binary_fields((uint16_t)rec->ir, bits, sizeof bits);
        fprintf(f, "\\textcolor{dim}{IR} %04X = %s\\\\\n", rec->ir, bits);
        fprintf(f, "%s", arrows(rec->effect));
        if (rec->has_output)
            fprintf(f, " \\hfill \\colorbox{outc}{\\textbf{OUTPUT: %u}}", rec->output);
        fprintf(f, "\\\\\n\\textcolor{dim}{BUS} ");
        print_bus(f, rec->bus_log, rec->bus_count, 0);
        fprintf(f, "\\\\\n\\textcolor{dim}{ALU} ");
        if (rec->alu.used)
            fprintf(f, "%s A=%04X B=%04X $\\rightarrow$ %04X", rec->alu.op, rec->alu.a, rec->alu.b, rec->alu.result);
        else
            fprintf(f, "\\textcolor{dim}{not used}");
        fprintf(f, "\\\\\nPC=%02X IR=%04X MAR=%02X MDR=%04X\\\\\n", state.pc, rec->ir, rec->mar, rec->mdr);
        fprintf(f, "Z=%d N=%d C=%d V=%d \\hfill \\textcolor{dim}{%d clock cycles}\\\\\n",
                state.flags.z, state.flags.n, state.flags.c, state.flags.v, rec->cycles);
    } else {
        fprintf(f, "\\textcolor{dim}{program loaded into memory}\\\\\n");
        fprintf(f, "\\textcolor{dim}{all registers start at 0}\\\\\n");
        fprintf(f, "PC=00 IR=0000 MAR=00 MDR=0000\\\\\n");
        fprintf(f, "Z=0 N=0 C=0 V=0\\\\\n");
    }

    /* R0..R7, changed ones in red */
    for (i = 0; i < NUM_REGS; i++) {
        int changed = rec && rec->reg_written[i];
        if (i % 4)
            fprintf(f, " ");
        if (changed)
            fprintf(f, "\\textcolor{wrt}{\\textbf{%s=%04X}}", REG_NAMES[i], state.regs[i]);
        else
            fprintf(f, "%s=%04X", REG_NAMES[i], state.regs[i]);
        if (i % 4 == 3)
            fprintf(f, "\\\\\n");
    }

    /* the named variables in decimal */
    {
        char vars[1024] = "";
        for (i = 0; i < prog->variables.count; i++)
            snprintf(vars + strlen(vars), sizeof vars - strlen(vars), "%s%s=%d", i ? "  " : "",
                     prog->variables.items[i].name, state.mem[prog->variables.items[i].value]);
        fprintf(f, "\\textcolor{dim}{%s}\\\\[1pt]\n", tex(vars));
    }

    /* memory */
    fprintf(f, "{\\setlength{\\tabcolsep}{1.6pt}\\begin{tabular}{@{}r*{8}{c}@{}}\n");
    fprintf(f, "\\textcolor{dim}{addr}");
    for (i = 0; i < ROW; i++)
        fprintf(f, " & \\textcolor{dim}{+%d}", i);
    fprintf(f, "\\\\\n");
    for (start = 0; start < window_end(prog); start += ROW) {
        fprintf(f, "\\textcolor{dim}{%02X}", start);
        for (a = start; a < start + ROW; a++) {
            unsigned word = state.mem[a];
            fprintf(f, " & ");
            if (rec && rec->mem_written[a])
                fprintf(f, "\\cellcolor{wrt}\\textcolor{white}{\\textbf{%04X}}", word);
            else if (a == state.pc && !(rec && rec->halted))   /* after HALT there is no next instruction */
                fprintf(f, "\\cellcolor{pcc}\\textbf{%04X}", word);
            else if (a < prog->word_count && prog->kinds[a] == KIND_DATA)
                fprintf(f, "\\cellcolor{datac}%04X", word);
            else if (a < prog->word_count && prog->kinds[a] == KIND_STACK)
                fprintf(f, "\\cellcolor{stackc}%04X", word);
            else
                fprintf(f, "%04X", word);
        }
        fprintf(f, "\\\\\n");
    }
    fprintf(f, "\\end{tabular}}\n\\end{tcolorbox}");
}

/* Boxes go two side by side. */
static void box_separator(FILE *f, int index, int total)
{
    (void)total;
    fprintf(f, index % 2 == 0 ? "\\noindent" : "\\hfill");
}

static void box_end(FILE *f, int index, int total)
{
    if (index % 2 == 1 || index == total - 1)
        fprintf(f, "\\par\\vspace{3pt}\n");
}

static void trace_boxes(const Program *prog, const Run *run, const char *file_name)
{
    FILE *f = open_generated(file_name);
    int total = run->count + 1, i;

    box_separator(f, 0, total);
    step_box(f, prog, "Step 0 \\hfill starting state", NULL, first_state(run));
    box_end(f, 0, total);
    for (i = 0; i < run->count; i++) {
        const StepRecord *rec = &run->records[i];
        char title[256];
        snprintf(title, sizeof title, "Step %d \\hfill PC %02X \\quad %s", rec->step, rec->pc, tex(rec->text));
        box_separator(f, i + 1, total);
        step_box(f, prog, title, rec, record_state(rec));
        box_end(f, i + 1, total);
    }
    fclose(f);
}

/* ===================================================================== */
/* The 3! walkthrough table                                               */
/* ===================================================================== */

/* A short plain-English sentence for one step of the sample. */
static void explain(const StepRecord *rec, const uint16_t *prev, const Program *prog, char *out, size_t size)
{
    Decoded d = decode((uint16_t)rec->ir);
    char name[MAX_NAME];
    const char *const *r = REG_NAMES;

    addr_text(d.addr8, &prog->symbols, name, sizeof name);
    switch (d.op) {
    case OP_LD:
        snprintf(out, size, "Copy memory word %s (value %d) into %s", name, rec->regs[d.rd], r[d.rd]);
        break;
    case OP_LI:
        snprintf(out, size, "Put the number %d into %s", d.imm9, r[d.rd]);
        break;
    case OP_MUL:
        snprintf(out, size, "ALU multiplies %s by %s: %d x %d = %d, answer goes to %s",
                 r[d.rs], r[d.rt], prev[d.rs], prev[d.rt], rec->regs[d.rd], r[d.rd]);
        break;
    case OP_ST:
        snprintf(out, size, "Copy %s (value %d) into memory word %s", r[d.rd], rec->regs[d.rd], name);
        break;
    case OP_ADDI:
        snprintf(out, size, "ALU adds %d to %s: %d + (%d) = %d, %s", d.imm6, r[d.rd], prev[d.rs], d.imm6,
                 rec->regs[d.rd], rec->flags.z ? "answer is 0, so Z = 1" : "answer is not 0, so Z = 0");
        break;
    case OP_BR:
        if (rec->next_pc == d.addr8)
            snprintf(out, size, "Z is 0, so jump back to %s (PC = %02X)", name, d.addr8);
        else
            snprintf(out, size, "Z is 1, so no jump. The loop is finished");
        break;
    case OP_OUT:
        snprintf(out, size, "Send %s = %d to the output port", r[d.rd], rec->regs[d.rd]);
        break;
    case OP_HALT:
        snprintf(out, size, "Stop the CPU");
        break;
    default:
        snprintf(out, size, "%s", rec->effect);
        break;
    }
}

static int symbol_value(const SymbolTable *table, const char *name)
{
    int i;
    for (i = 0; i < table->count; i++)
        if (strcmp(table->items[i].name, name) == 0)
            return table->items[i].value;
    die("missing label", name);
    return 0;
}

static void walkthrough(const Program *prog, const Run *run)
{
    FILE *f = open_generated("sample_walk.tex");
    int n_addr = symbol_value(&prog->symbols, "n");
    int result_addr = symbol_value(&prog->symbols, "result");
    const uint16_t *prev = run->first_regs;
    const StepRecord *last;
    char title[256];
    int i, j;

    for (i = 0; i < run->count; i++) {
        const StepRecord *rec = &run->records[i];
        char sentence[256];
        int has_execute = 0;

        for (j = 0; j < rec->bus_count; j++)
            if (rec->bus_log[j].phase != PHASE_FETCH)
                has_execute = 1;
        explain(rec, prev, prog, sentence, sizeof sentence);
        fprintf(f, "%d & \\texttt{%s} & %s & \\texttt{", rec->step, tex(rec->text), times_signs(tex(sentence)));
        if (has_execute)
            print_bus(f, rec->bus_log, rec->bus_count, 1);
        else
            fprintf(f, "\\textcolor{dim}{none}");
        fprintf(f, "} & %d & %d & %d & %d & %d\\\\\n", rec->regs[1], rec->regs[2], rec->flags.z,
                rec->mem[n_addr], rec->mem[result_addr]);
        prev = rec->regs;
    }
    fclose(f);

    /* memory before and after, side by side */
    f = open_generated("sample_boxes.tex");
    last = &run->records[run->count - 1];
    box_separator(f, 0, 2);
    step_box(f, prog, "Before: step 0 \\hfill program loaded", NULL, first_state(run));
    box_end(f, 0, 2);
    snprintf(title, sizeof title, "After: step %d \\hfill %s", last->step, tex(last->text));
    box_separator(f, 1, 2);
    step_box(f, prog, title, last, record_state(last));
    box_end(f, 1, 2);
    fclose(f);
}

/* ===================================================================== */
/* The three phases of the sample: assemble, load, (execute is above)     */
/* ===================================================================== */

/* The assembler's lookup table, four entries per row. */
static void lookup_table(void)
{
    FILE *f = open_generated("lookup_table.tex");
    int rows = (OPCODE_TABLE_SIZE + 3) / 4, r, c;

    for (r = 0; r < rows; r++) {
        for (c = 0; c < 4; c++) {
            int i = c * rows + r;
            if (c)
                fprintf(f, " & ");
            if (i < OPCODE_TABLE_SIZE) {
                const OpcodeEntry *e = &OPCODE_TABLE[i];
                char bits[5];
                int b;
                for (b = 0; b < 4; b++)
                    bits[b] = ((e->opcode >> (3 - b)) & 1) ? '1' : '0';
                bits[4] = '\0';
                fprintf(f, "\\texttt{%s} & \\texttt{%s} & %s", e->mnemonic, bits, FORMAT_NAMES[e->format]);
            } else {
                fprintf(f, " & & ");
            }
        }
        fprintf(f, "\\\\\n");
    }
    fclose(f);
}

/* Phase 1: each line, its table lookup and the machine code it became. */
static void assemble_table(const Program *prog, const char *file_name)
{
    FILE *f = open_generated(file_name);
    int i, j;

    for (i = 0; i < prog->listing_count; i++) {
        const ListingLine *row = &prog->listing[i];
        char code[512], comment[512], label[MAX_NAME + 8], rest[512], text[512], bits[32], mnemonic[16];

        if (row->addr < 0 || row->word_count == 0)
            continue;
        split_comment(row->text, code, comment);
        split_label(code, label, rest);
        squeeze(rest, text);
        if (prog->kinds[row->addr] != KIND_CODE) {
            for (j = 0; j < row->word_count; j++) {
                uint16_t word = prog->words[row->addr + j];
                binary_plain(word, bits, sizeof bits);
                fprintf(f, "\\rowcolor{datac}0x%02X & %s & {\\rmfamily\\itshape data word} & %s & %04X\\\\\n",
                        row->addr + j, j == 0 ? tex(text) : "", bits, word);
            }
            continue;
        }
        for (j = 0; text[j] && text[j] != ' ' && j < 15; j++)
            mnemonic[j] = (char)toupper((unsigned char)text[j]);
        mnemonic[j] = '\0';
        {
            uint16_t word = prog->words[row->addr];
            Decoded d = decode(word);
            char opbits[5];
            int b;
            for (b = 0; b < 4; b++)
                opbits[b] = ((d.op >> (3 - b)) & 1) ? '1' : '0';
            opbits[4] = '\0';
            binary_fields(word, bits, sizeof bits);
            fprintf(f, "0x%02X & %s & %s $\\rightarrow$ %s {\\rmfamily (%s)} & %s & %04X\\\\\n",
                    row->addr, tex(text), mnemonic, opbits, FORMAT_NAMES[decoded_format(&d)], bits, word);
        }
    }
    fclose(f);
}

/* A box that shows only RAM, with the written words in red. */
static void ram_box(FILE *f, const Program *prog, const char *title, const char *note,
                    const uint16_t *mem, const char *written)
{
    int i, start, a;

    fprintf(f, "\\begin{tcolorbox}[stepbox, title={%s}]\n", title);
    fprintf(f, "\\textcolor{dim}{%s}\\\\[1pt]\n", note);
    fprintf(f, "{\\setlength{\\tabcolsep}{1.6pt}\\begin{tabular}{@{}r*{8}{c}@{}}\n");
    fprintf(f, "\\textcolor{dim}{addr}");
    for (i = 0; i < ROW; i++)
        fprintf(f, " & \\textcolor{dim}{+%d}", i);
    fprintf(f, "\\\\\n");
    for (start = 0; start < window_end(prog); start += ROW) {
        fprintf(f, "\\textcolor{dim}{%02X}", start);
        for (a = start; a < start + ROW; a++) {
            if (written && written[a])
                fprintf(f, " & \\cellcolor{wrt}\\textcolor{white}{\\textbf{%04X}}", mem[a]);
            else
                fprintf(f, " & %04X", mem[a]);
        }
        fprintf(f, "\\\\\n");
    }
    fprintf(f, "\\end{tabular}}\n\\end{tcolorbox}");
}

/* Phase 2: every word the loader put on the bus, then RAM before and after. */
static void load_tables(const Program *prog, const Run *run)
{
    static const uint16_t empty[MEM_SIZE];
    char written[MEM_SIZE];
    FILE *f = open_generated("sample_load.tex");
    int i, j;

    for (i = 0; i < run->load_count; i++) {
        const LoadStep *step = &run->load[i];
        char what[MAX_NAME + 32];

        if (prog->kinds[i] == KIND_CODE) {
            char text[MAX_NAME + 32];
            disassemble(step->word, &prog->symbols, text, sizeof text);
            snprintf(what, sizeof what, "\\texttt{%s}", tex(text));
        } else {
            snprintf(what, sizeof what, "data");
            for (j = 0; j < prog->variables.count; j++)
                if (prog->variables.items[j].value == i)
                    snprintf(what, sizeof what, "data (\\texttt{%s})", tex(prog->variables.items[j].name));
        }
        fprintf(f, "%d & \\texttt{0x%02X} & \\texttt{%04X} & %s & \\texttt{RAM[0x%02X] = %04X} & %s\\\\\n",
                i + 1, step->addr, step->word, step->control, step->addr, step->word, what);
    }
    fclose(f);

    memset(written, 0, sizeof written);
    memset(written, 1, (size_t)run->load_count);
    f = open_generated("sample_load_boxes.tex");
    fprintf(f, "\\noindent");
    ram_box(f, prog, "RAM before loading", "all 256 words are 0000", empty, NULL);
    fprintf(f, "\\hfill");
    ram_box(f, prog, "RAM after loading", "red = written by the loader", run->first_mem, written);
    fprintf(f, "\\par\\vspace{3pt}\n");
    fclose(f);
}

/* ===================================================================== */
/* main                                                                   */
/* ===================================================================== */

static void load_asm(const char *path, const char *name, Program *prog)
{
    char err[256];
    char *source = read_file(path);
    if (assemble(source, name, prog, err, sizeof err) != 0)
        die("assembler error:", err);
    free(source);
}

static void write_numbers(FILE *f, const char *key, const Run *run, const Program *prog)
{
    int i;

    fprintf(f, "\\newcommand{\\%sSteps}{%d}\\newcommand{\\%sCycles}{%d}\\newcommand{\\%sOut}{",
            key, run->cpu.steps, key, run->cpu.cycles, key);
    for (i = 0; i < run->cpu.out_port.count; i++)
        fprintf(f, "%s%u", i ? " " : "", run->cpu.out_port.values[i]);
    fprintf(f, "}\\newcommand{\\%sWords}{%d}\n", key, prog->word_count);
}

int main(void)
{
    static Program sample, fact, power;
    static Run sample_run, fact_run, power_run;
    char err[256];
    char *power_c, *power_asm;
    FILE *f;

    isa_tables();
    format_figure();
    lookup_table();

    /* sample: 3! */
    load_asm("programs/factorial_of_3.asm", "factorial_of_3.asm", &sample);
    assemble_table(&sample, "sample_assemble.tex");
    run_program(&sample, &sample_run);
    load_tables(&sample, &sample_run);
    walkthrough(&sample, &sample_run);

    /* program 1: 8! in assembly */
    load_asm("programs/factorial.asm", "factorial.asm", &fact);
    asm_listing(&fact, "fact_listing.tex");
    memory_map(&fact, "fact_memmap.tex");
    run_program(&fact, &fact_run);
    trace_boxes(&fact, &fact_run, "fact_trace.tex");

    /* program 2: 8^2 in C */
    power_c = read_file("programs/power.c");
    power_asm = compile_c(power_c, "power.c", err, sizeof err);
    if (!power_asm)
        die("compiler error:", err);
    f = open_out("output/power.asm");
    fputs(power_asm, f);
    fclose(f);
    if (assemble(power_asm, "power.c", &power, err, sizeof err) != 0)
        die("assembler error:", err);
    f = open_generated("power_c.tex");
    fprintf(f, "\\begin{lstlisting}[style=csrc]\n%s\\end{lstlisting}\n", power_c);
    fclose(f);
    c_translation(&power);
    memory_map(&power, "power_memmap.tex");
    run_program(&power, &power_run);
    trace_boxes(&power, &power_run, "power_trace.tex");

    f = open_generated("results.tex");
    write_numbers(f, "sample", &sample_run, &sample);
    write_numbers(f, "fact", &fact_run, &fact);
    write_numbers(f, "power", &power_run, &power);
    fclose(f);

    free(power_c);
    free(power_asm);
    free(sample_run.records);
    free(fact_run.records);
    free(power_run.records);
    program_free(&sample);
    program_free(&fact);
    program_free(&power);
    printf("wrote the report tables into report/generated/\n");
    return 0;
}
