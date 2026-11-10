/*
 * assembler.c
 * Assembler for SPARTAN-16 assembly language.
 *
 * One statement per line. Anything after ';' is a comment.
 *
 *     label:  INSTRUCTION operand, operand, ...
 *             .word  value, value, ...    ; data words
 *             .space count                ; count words of zeros (we use it for the stack)
 *
 * Operands can be:
 *     a register    R0..R7 or SP (same as R7)
 *     a number      42, -1, 0x2A, or a label name (means its address)
 *     memory        a label or number for a fixed address,
 *                   or [Rb], [Rb+n], [Rb-n] for register + offset
 *
 * Two extra helper instructions:
 *     MOV rd, rs   is really  ADDI rd, rs, 0
 *     NOP          is really  ADDI R0, R0, 0
 *
 * It works in two passes. Pass 1 finds the address of every label.
 * Pass 2 turns every line into machine code: it looks the instruction name up
 * in the ISA lookup table (OPCODE_TABLE in isa.c) to get the opcode and the
 * format, then puts the operands into the right bit fields.
 */
#include <ctype.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "assembler.h"

/* One source line split into its pieces. */
typedef struct {
    int lineno;
    char *raw;          /* the line as written, without trailing spaces */
    char *buffer;       /* working copy that label, mnemonic and ops point into */
    char *label;
    char *mnemonic;     /* in upper case, NULL if the line has no statement */
    char **ops;
    int op_count;
} Line;

typedef struct {
    const char *name;
    Program *prog;
    Line *lines;
    int line_count;
    char *err;
    size_t err_size;
    jmp_buf on_error;
} Assembler;

/* static because it is used across setjmp/longjmp */
static Assembler as;

static void fail(int lineno, const char *format, ...)
{
    char message[256];
    va_list args;

    va_start(args, format);
    vsnprintf(message, sizeof message, format, args);
    va_end(args);
    snprintf(as.err, as.err_size, "%s:%d: %s", as.name, lineno, message);
    longjmp(as.on_error, 1);
}

static void *must_alloc(size_t size)
{
    void *p = calloc(size ? size : 1, 1);
    if (!p) {
        fprintf(stderr, "out of memory\n");
        exit(1);
    }
    return p;
}

static char *copy_range(const char *start, size_t length)
{
    char *s = must_alloc(length + 1);
    memcpy(s, start, length);
    return s;
}

/* Remove spaces from both ends. Returns a pointer inside s. */
static char *trim(char *s)
{
    char *end;

    while (isspace((unsigned char)*s))
        s++;
    end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1]))
        *--end = '\0';
    return s;
}

static int is_name_start(char c)
{
    return isalpha((unsigned char)c) || c == '_' || c == '.';
}

static int is_name_char(char c)
{
    return isalnum((unsigned char)c) || c == '_' || c == '.';
}

static int is_word_char(char c)
{
    return isalnum((unsigned char)c) || c == '_';
}

static int all_word_chars(const char *s)
{
    if (!*s)
        return 0;
    for (; *s; s++)
        if (!is_word_char(*s))
            return 0;
    return 1;
}

/* Read a whole string as a number: 42, -1, 0x2A, 0b101 or 0o17. */
static int parse_number(const char *s, long *result)
{
    const char *p = s;
    char *end;
    int base = 10;
    long value;

    if (*p == '+' || *p == '-')
        p++;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        base = 16;
        p += 2;
    } else if (p[0] == '0' && (p[1] == 'b' || p[1] == 'B')) {
        base = 2;
        p += 2;
    } else if (p[0] == '0' && (p[1] == 'o' || p[1] == 'O')) {
        base = 8;
        p += 2;
    }
    if (!isalnum((unsigned char)*p))
        return 0;
    value = strtol(p, &end, base);
    if (*end != '\0')
        return 0;
    *result = (s[0] == '-') ? -value : value;
    return 1;
}

/* ----- reading operands ----- */

static int reg(const char *text, int lineno)
{
    if (strlen(text) == 2 && toupper((unsigned char)text[0]) == 'S' && toupper((unsigned char)text[1]) == 'P')
        return SP;
    if (strlen(text) == 2 && toupper((unsigned char)text[0]) == 'R' && text[1] >= '0' && text[1] <= '7')
        return text[1] - '0';
    fail(lineno, "expected a register (R0-R7 or SP), got '%s'", text);
    return 0;
}

/* A number, a label, or label+n / label-n. */
static long value(const char *input, int lineno)
{
    char text[MAX_NAME * 2];
    char *t, *p;
    long number;
    int i;

    snprintf(text, sizeof text, "%s", input);
    t = trim(text);

    if (is_name_start(t[0])) {
        p = t;
        while (is_name_char(*p))
            p++;
        if (*p) {
            char *after = p;
            while (isspace((unsigned char)*after))
                after++;
            if (*after == '+' || *after == '-') {
                char sign = *after;
                char *rest = after + 1;
                long base, offset;
                while (isspace((unsigned char)*rest))
                    rest++;
                if (all_word_chars(rest)) {
                    *p = '\0';
                    base = value(t, lineno);
                    offset = value(rest, lineno);
                    return sign == '+' ? base + offset : base - offset;
                }
            }
        }
    }

    if (parse_number(t, &number))
        return number;
    for (i = 0; i < as.prog->symbols.count; i++)
        if (strcmp(as.prog->symbols.items[i].name, t) == 0)
            return as.prog->symbols.items[i].value;
    fail(lineno, "unknown label or bad number '%s'", t);
    return 0;
}

static void expect_ops(const Line *line, int count)
{
    if (line->op_count != count)
        fail(line->lineno, "%s takes %d operand(s), got %d", line->mnemonic, count, line->op_count);
}

/* Match "[Rb]", "[Rb+n]" or "[Rb-n]". Returns 1 and the pieces if it matches. */
static int match_memory(char *text, char **base, char *sign, char **offset)
{
    char *p, *end;
    size_t length = strlen(text);

    if (length < 2 || text[0] != '[' || text[length - 1] != ']')
        return 0;
    text[length - 1] = '\0';
    p = text + 1;
    while (isspace((unsigned char)*p))
        p++;
    *base = p;
    while (is_word_char(*p))
        p++;
    if (p == *base)
        return 0;
    end = p;
    while (isspace((unsigned char)*p))
        p++;
    *sign = '\0';
    *offset = NULL;
    if (*p == '+' || *p == '-') {
        *sign = *p;
        *p++ = '\0';
        while (isspace((unsigned char)*p))
            p++;
        *offset = trim(p);
        if (!all_word_chars(*offset))
            return 0;
    } else if (*p != '\0') {
        return 0;
    }
    *end = '\0';
    return 1;
}

/* ----- splitting the source into lines ----- */

static int count_lines(const char *s)
{
    int n = 0;
    for (; *s; s++)
        if (*s == '\n')
            n++;
    return n + 1;
}

static void split_operands(Line *line, char *text)
{
    char *p, *start;
    int count = 1, i = 0;

    text = trim(text);
    if (!*text)
        return;
    for (p = text; *p; p++)
        if (*p == ',')
            count++;
    line->ops = must_alloc(sizeof(char *) * (size_t)count);
    start = text;
    for (p = text; ; p++) {
        if (*p == ',' || *p == '\0') {
            int done = *p == '\0';
            *p = '\0';
            line->ops[i++] = trim(start);
            start = p + 1;
            if (done)
                break;
        }
    }
    line->op_count = count;
}

/* Split one line into its label, instruction and operands. */
static void parse_line(Line *line)
{
    char *text, *p, *name_end;

    /* remove the comment */
    text = line->buffer;
    p = strchr(text, ';');
    if (p)
        *p = '\0';
    text = trim(text);

    /* label? */
    if (is_name_start(text[0])) {
        p = text;
        while (is_name_char(*p))
            p++;
        name_end = p;
        while (isspace((unsigned char)*p))
            p++;
        if (*p == ':') {
            *name_end = '\0';
            line->label = text;
            text = trim(p + 1);
        }
    }

    /* instruction and operands */
    if (*text) {
        p = text;
        while (*p && !isspace((unsigned char)*p))
            p++;
        if (*p) {
            *p++ = '\0';
            split_operands(line, p);
        }
        line->mnemonic = text;
        for (p = text; *p; p++)
            *p = (char)toupper((unsigned char)*p);
    }
}

static void read_lines(const char *source)
{
    const char *p = source;
    int capacity = count_lines(source);

    as.lines = must_alloc(sizeof(Line) * (size_t)capacity);
    while (*p) {
        const char *end = strchr(p, '\n');
        size_t length = end ? (size_t)(end - p) : strlen(p);
        size_t kept = length;
        Line *line = &as.lines[as.line_count++];

        while (kept > 0 && isspace((unsigned char)p[kept - 1]))
            kept--;
        line->lineno = as.line_count;
        line->raw = copy_range(p, kept);
        line->buffer = copy_range(p, length);
        parse_line(line);
        p += length;
        if (*p == '\n')
            p++;
    }
}

/* ----- pass 1: find where every label is ----- */

static int space_count(const Line *line)
{
    long count = 0;

    if (line->op_count != 1 || !parse_number(line->ops[0], &count) || count < 0)
        fail(line->lineno, ".space needs a word count");
    return (int)count;
}

static void add_symbol(SymbolTable *table, const char *name, int address, int lineno)
{
    Symbol *symbol;

    if (table->count >= MAX_SYMBOLS)
        fail(lineno, "too many labels");
    if (strlen(name) >= MAX_NAME)
        fail(lineno, "label '%s' is too long", name);
    symbol = &table->items[table->count++];
    strcpy(symbol->name, name);
    symbol->value = address;
}

static void pass1(void)
{
    int i, j, addr = 0;

    for (i = 0; i < as.line_count; i++) {
        const Line *line = &as.lines[i];

        if (line->label) {
            for (j = 0; j < as.prog->symbols.count; j++)
                if (strcmp(as.prog->symbols.items[j].name, line->label) == 0)
                    fail(line->lineno, "label '%s' defined twice", line->label);
            add_symbol(&as.prog->symbols, line->label, addr, line->lineno);
        }
        if (line->mnemonic) {
            if (strcmp(line->mnemonic, ".WORD") == 0)
                addr += line->op_count;
            else if (strcmp(line->mnemonic, ".SPACE") == 0)
                addr += space_count(line);
            else
                addr += 1;
        }
    }
    if (addr > MEM_SIZE) {
        snprintf(as.err, as.err_size, "%s: program needs %d words, memory has %d",
                 as.name, addr, MEM_SIZE);
        longjmp(as.on_error, 1);
    }
}

/* ----- pass 2: turn each line into machine code ----- */

static uint16_t encode_memory(const Line *line, int op)
{
    char *base, *offset_text, sign;
    int r;

    expect_ops(line, 2);
    r = reg(line->ops[0], line->lineno);
    if (match_memory(line->ops[1], &base, &sign, &offset_text)) {
        int rb = reg(base, line->lineno);
        long offset = offset_text ? value(offset_text, line->lineno) : 0;
        if (sign == '-')
            offset = -offset;
        if (!fits_signed((int)offset, 5))
            fail(line->lineno, "offset %ld out of range -16..15", offset);
        return enc_m_ind(op, r, rb, (int)offset);
    } else {
        long addr = value(line->ops[1], line->lineno);
        if (addr < 0 || addr > ADDR_MASK)
            fail(line->lineno, "address %ld out of range 0..255", addr);
        return enc_m_abs(op, r, (int)addr);
    }
}

/* Translate one instruction into a 16-bit word.
 * Step 1: look the mnemonic up in OPCODE_TABLE (isa.c) to get the opcode and format.
 * Step 2: read the operands and put them into the bit fields of that format. */
static uint16_t encode(const Line *line)
{
    const char *m = line->mnemonic;
    int lineno = line->lineno;
    const OpcodeEntry *entry;
    int op;

    /* two shortcuts that are really ADDI */
    if (strcmp(m, "MOV") == 0) {
        expect_ops(line, 2);
        return enc_i(OP_ADDI, reg(line->ops[0], lineno), reg(line->ops[1], lineno), 0);
    }
    if (strcmp(m, "NOP") == 0)
        return enc_i(OP_ADDI, 0, 0, 0);

    /* step 1: the lookup */
    entry = lookup_mnemonic(m);
    if (!entry)
        fail(lineno, "unknown instruction '%s'", m);
    op = entry->opcode;

    /* step 2: fill in the fields for this format */
    switch (entry->format) {
    case FMT_N:         /* HALT, RET: no operands */
        expect_ops(line, 0);
        return (uint16_t)(op << 12);

    case FMT_R:         /* ADD, SUB, MUL, AND, OR, CMP */
        if (op == OP_CMP) {
            expect_ops(line, 2);
            return enc_r(op, 0, reg(line->ops[0], lineno), reg(line->ops[1], lineno));
        }
        expect_ops(line, 3);
        return enc_r(op, reg(line->ops[0], lineno), reg(line->ops[1], lineno), reg(line->ops[2], lineno));

    case FMT_I: {       /* ADDI */
        long imm;
        expect_ops(line, 3);
        imm = value(line->ops[2], lineno);
        if (!fits_signed((int)imm, 6))
            fail(lineno, "ADDI immediate %ld out of range -32..31", imm);
        return enc_i(op, reg(line->ops[0], lineno), reg(line->ops[1], lineno), (int)imm);
    }

    case FMT_L: {       /* LI */
        long imm;
        expect_ops(line, 2);
        imm = value(line->ops[1], lineno);
        if (!fits_signed((int)imm, 9))
            fail(lineno, "LI immediate %ld out of range -256..255", imm);
        return enc_l(op, reg(line->ops[0], lineno), (int)imm);
    }

    case FMT_M:         /* LD, ST */
        return encode_memory(line, op);

    case FMT_B: {       /* JMP, BEQ ... BLE, CALL */
        long addr;
        expect_ops(line, 1);
        addr = value(line->ops[0], lineno);
        if (addr < 0 || addr > ADDR_MASK)
            fail(lineno, "target %ld out of range 0..255", addr);
        return enc_b(op, entry->extra, (int)addr);
    }

    default:            /* FMT_S: PUSH, POP, OUT */
        expect_ops(line, 1);
        return enc_s(op, reg(line->ops[0], lineno), entry->extra);
    }
}

static void pass2(void)
{
    Program *prog = as.prog;
    int i, j;

    prog->listing = must_alloc(sizeof(ListingLine) * (size_t)(as.line_count + 1));
    for (i = 0; i < as.line_count; i++) {
        const Line *line = &as.lines[i];
        ListingLine *row = &prog->listing[prog->listing_count++];
        int addr = prog->word_count;

        row->text = copy_range(line->raw, strlen(line->raw));
        row->addr = -1;
        if (!line->mnemonic)
            continue;
        row->addr = addr;

        if (strcmp(line->mnemonic, ".WORD") == 0) {
            for (j = 0; j < line->op_count; j++) {
                prog->words[prog->word_count] = (uint16_t)(value(line->ops[j], line->lineno) & WORD_MASK);
                prog->kinds[prog->word_count++] = KIND_DATA;
            }
            if (line->label)
                add_symbol(&prog->variables, line->label, addr, line->lineno);
        } else if (strcmp(line->mnemonic, ".SPACE") == 0) {
            int count = space_count(line);
            for (j = 0; j < count; j++) {
                prog->words[prog->word_count] = 0;
                prog->kinds[prog->word_count++] = KIND_STACK;
            }
        } else if (line->mnemonic[0] == '.') {
            fail(line->lineno, "unknown directive '%s'", line->mnemonic);
        } else {
            prog->words[prog->word_count] = encode(line);
            prog->kinds[prog->word_count++] = KIND_CODE;
        }
        row->word_count = prog->word_count - addr;
    }
}

static void free_lines(void)
{
    int i;

    for (i = 0; i < as.line_count; i++) {
        free(as.lines[i].raw);
        free(as.lines[i].buffer);
        free(as.lines[i].ops);
    }
    free(as.lines);
    as.lines = NULL;
    as.line_count = 0;
}

int assemble(const char *source, const char *name, Program *prog, char *err, size_t err_size)
{
    memset(prog, 0, sizeof *prog);
    snprintf(prog->source_name, sizeof prog->source_name, "%s", name);

    memset(&as, 0, sizeof as);
    as.name = name;
    as.prog = prog;
    as.err = err;
    as.err_size = err_size;

    if (setjmp(as.on_error)) {
        free_lines();
        program_free(prog);
        return -1;
    }
    read_lines(source);
    pass1();
    pass2();
    free_lines();
    return 0;
}

void program_free(Program *prog)
{
    int i;

    for (i = 0; i < prog->listing_count; i++)
        free(prog->listing[i].text);
    free(prog->listing);
    prog->listing = NULL;
    prog->listing_count = 0;
}

const char *kind_name(WordKind kind)
{
    switch (kind) {
    case KIND_CODE: return "code";
    case KIND_DATA: return "data";
    default:        return "stack";
    }
}

void print_listing(FILE *out, const Program *prog)
{
    int i, j;
    char bits[32], dashes[41];

    memset(dashes, '-', 40);
    dashes[40] = '\0';
    fprintf(out, "ADDR  HEX   BINARY (fields)        SOURCE\n");
    fprintf(out, "----  ----  ---------------------  %s\n", dashes);

    for (i = 0; i < prog->listing_count; i++) {
        const ListingLine *row = &prog->listing[i];

        if (row->addr < 0) {
            fprintf(out, "%-34s%s\n", "", row->text);
            continue;
        }
        for (j = 0; j < row->word_count; j++) {
            int addr = row->addr + j;
            uint16_t word = prog->words[addr];

            if (prog->kinds[addr] == KIND_CODE)
                binary_fields(word, bits, sizeof bits);
            else
                binary_plain(word, bits, sizeof bits);
            fprintf(out, "0x%02X  %04X  %-21s  %s\n", addr, word, bits, j == 0 ? row->text : "");
        }
    }
}
