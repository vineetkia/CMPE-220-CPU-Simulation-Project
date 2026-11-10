/*
 * compiler.c
 * s16cc: our small C compiler. It turns a subset of C into SPARTAN-16
 * assembly. The output is plain assembly text that then goes through the
 * normal assembler, the same way a hand written program does.
 *
 * What it understands:
 *     types        int (16 bits), and void for functions that return nothing
 *     globals      int x;   int x = 5;
 *     functions    int f(int a, int b) { ... }      (no recursion, see below)
 *     statements   int x = e;  x = e;  x += e;  x -= e;  x *= e;  x++;  x--;
 *                  if / else, while, for, return, { ... }, f(...);
 *                  printf("%d\n", e);   which becomes an OUT instruction
 *     expressions  + - * & |   == != < <= > >=   && || !   unary minus, calls
 *     skipped      #include lines and comments
 *
 * How C is mapped onto the CPU:
 *     - every variable gets its own word in memory, named function.name
 *       (for example power.result). Globals just use their name.
 *     - expressions are worked out in registers R1 to R6
 *     - to call a function we store the arguments in its parameter words,
 *       then CALL. The answer comes back in R1. The return address goes on
 *       the stack.
 *     - since each variable has one fixed spot, a function can't call itself,
 *       so recursion gives an error
 *     - there is no divide instruction, so / and % give an error
 *
 * It works in three steps:
 *     1. split the source into tokens
 *     2. parse the tokens into a tree
 *     3. walk the tree and write assembly
 */
#include <ctype.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "compiler.h"

#define MAX_REG 6   /* we can use R1..R6 (R0 is always 0 and R7 is SP) */

/* ===================================================================== */
/* Memory and errors                                                      */
/* ===================================================================== */

/* Everything the compiler allocates goes on one list and is freed at the end. */
typedef struct Chunk {
    struct Chunk *next;
    long double align;      /* keeps the memory after the header aligned */
} Chunk;

static Chunk *chunks;
static jmp_buf on_error;
static char *error_text;
static size_t error_size;

static void *alloc(size_t size)
{
    Chunk *c = calloc(1, sizeof(Chunk) + size);
    if (!c) {
        fprintf(stderr, "out of memory\n");
        exit(1);
    }
    c->next = chunks;
    chunks = c;
    return c + 1;
}

static void free_all(void)
{
    while (chunks) {
        Chunk *next = chunks->next;
        free(chunks);
        chunks = next;
    }
}

static char *format(const char *fmt, ...)
{
    va_list args;
    int length;
    char *s;

    va_start(args, fmt);
    length = vsnprintf(NULL, 0, fmt, args);
    va_end(args);
    s = alloc((size_t)length + 1);
    va_start(args, fmt);
    vsnprintf(s, (size_t)length + 1, fmt, args);
    va_end(args);
    return s;
}

/* Stop compiling. line is 0 when we don't know the line. */
static void error(int line, const char *fmt, ...)
{
    char message[256];
    va_list args;

    va_start(args, fmt);
    vsnprintf(message, sizeof message, fmt, args);
    va_end(args);
    if (line)
        snprintf(error_text, error_size, "line %d: %s", line, message);
    else
        snprintf(error_text, error_size, "%s", message);
    longjmp(on_error, 1);
}

/* A growing list of strings. */
typedef struct {
    char **items;
    int count, capacity;
} StrList;

static void list_add(StrList *list, char *s)
{
    if (list->count == list->capacity) {
        int capacity = list->capacity ? list->capacity * 2 : 16;
        char **items = alloc(sizeof(char *) * (size_t)capacity);
        if (list->count)
            memcpy(items, list->items, sizeof(char *) * (size_t)list->count);
        list->items = items;
        list->capacity = capacity;
    }
    list->items[list->count++] = s;
}

static int list_find(const StrList *list, const char *s)
{
    int i;
    for (i = 0; i < list->count; i++)
        if (strcmp(list->items[i], s) == 0)
            return i;
    return -1;
}

/* ===================================================================== */
/* Step 1: split the source into tokens                                   */
/* ===================================================================== */

typedef enum { T_NUM, T_STR, T_ID, T_KW, T_OP, T_EOF } TokenKind;

typedef struct {
    TokenKind kind;
    char *text;
    int line;
} Token;

static Token *tokens;
static int token_count;

static const char *const KEYWORDS[] = { "int", "void", "if", "else", "while", "for", "return", NULL };
static const char *const TWO_CHAR_OPS[] = { "++", "--", "+=", "-=", "*=", "==", "!=", "<=", ">=", "&&", "||", NULL };
static const char ONE_CHAR_OPS[] = "-+*/%<>=!&|(){};,";

static void add_token(TokenKind kind, const char *start, size_t length, int line, int *capacity)
{
    Token *t;

    if (token_count == *capacity) {
        Token *bigger;
        *capacity = *capacity ? *capacity * 2 : 64;
        bigger = alloc(sizeof(Token) * (size_t)*capacity);
        if (token_count)
            memcpy(bigger, tokens, sizeof(Token) * (size_t)token_count);
        tokens = bigger;
    }
    t = &tokens[token_count++];
    t->kind = kind;
    t->text = alloc(length + 1);
    memcpy(t->text, start, length);
    t->line = line;
    if (kind == T_ID) {
        int i;
        for (i = 0; KEYWORDS[i]; i++)
            if (strcmp(KEYWORDS[i], t->text) == 0)
                t->kind = T_KW;
    }
}

static int count_newlines(const char *start, const char *end)
{
    int n = 0;
    for (; start < end; start++)
        if (*start == '\n')
            n++;
    return n;
}

static void tokenize(const char *src)
{
    const char *p = src;
    int line = 1, capacity = 0;

    tokens = NULL;
    token_count = 0;
    while (*p) {
        const char *start = p;
        TokenKind kind;
        int i;

        if (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
            while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
                p++;
            line += count_newlines(start, p);
            continue;
        }
        if (p[0] == '/' && p[1] == '/') {
            while (*p && *p != '\n')
                p++;
            continue;
        }
        if (p[0] == '/' && p[1] == '*' && strstr(p + 2, "*/")) {
            p = strstr(p + 2, "*/") + 2;
            line += count_newlines(start, p);
            continue;
        }
        if (*p == '#') {                    /* #include and friends are skipped */
            while (*p && *p != '\n')
                p++;
            continue;
        }

        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X') && isxdigit((unsigned char)p[2])) {
            p += 2;
            while (isxdigit((unsigned char)*p))
                p++;
            kind = T_NUM;
        } else if (isdigit((unsigned char)*p)) {
            while (isdigit((unsigned char)*p))
                p++;
            kind = T_NUM;
        } else if (*p == '"') {
            const char *q = p + 1;
            while (*q && *q != '"') {
                if (*q == '\\' && q[1])
                    q++;
                q++;
            }
            if (*q != '"')
                error(line, "unexpected character '\"'");
            p = q + 1;
            kind = T_STR;
        } else if (isalpha((unsigned char)*p) || *p == '_') {
            while (isalnum((unsigned char)*p) || *p == '_')
                p++;
            kind = T_ID;
        } else {
            kind = T_OP;
            for (i = 0; TWO_CHAR_OPS[i]; i++)
                if (strncmp(p, TWO_CHAR_OPS[i], 2) == 0)
                    break;
            if (TWO_CHAR_OPS[i])
                p += 2;
            else if (strchr(ONE_CHAR_OPS, *p))
                p += 1;
            else
                error(line, "unexpected character '%c'", *p);
        }
        add_token(kind, start, (size_t)(p - start), line, &capacity);
        line += count_newlines(start, p);
    }
    add_token(T_EOF, "", 0, line, &capacity);
}

/* ===================================================================== */
/* Step 2: parse the tokens into a tree                                   */
/* ===================================================================== */

typedef enum { E_NUM, E_VAR, E_BIN, E_UN, E_CALL } ExprKind;

typedef struct Expr {
    ExprKind kind;
    long value;             /* E_NUM */
    const char *name;       /* E_VAR, E_CALL */
    const char *op;         /* E_BIN, E_UN */
    struct Expr *a, *b;     /* operands */
    struct Expr **args;     /* E_CALL */
    int arg_count;
    int line;
} Expr;

typedef enum { S_BLOCK, S_DECL, S_ASSIGN, S_PRINT, S_EXPR, S_RETURN, S_IF, S_WHILE, S_FOR } StmtKind;

typedef struct Stmt {
    StmtKind kind;
    int line;
    const char *name;           /* S_DECL, S_ASSIGN */
    const char *op;             /* S_ASSIGN: "=", "+=", "-=" or "*=" */
    Expr *value;                /* S_DECL (can be NULL), S_ASSIGN, S_PRINT, S_EXPR, S_RETURN */
    Expr *cond;                 /* S_IF, S_WHILE, S_FOR */
    struct Stmt *then, *other;  /* S_IF */
    struct Stmt *body;          /* S_WHILE, S_FOR */
    struct Stmt *init, *step;   /* S_FOR (both can be NULL) */
    struct Stmt **items;        /* S_BLOCK */
    int count;
} Stmt;

typedef struct {
    const char *name;
    StrList params;
    Stmt *body;
    int line;
    int is_void;
} Function;

typedef struct {
    const char *name;
    long value;
} Global;

static Function *functions;
static int function_count;
static Global *globals;
static int global_count;
static int pos;     /* next token to read */

static Token *peek(int k) { return &tokens[pos + k < token_count ? pos + k : token_count - 1]; }
static Token *next(void)  { Token *t = peek(0); if (pos < token_count - 1) pos++; return t; }

/* Take the next token if it is this operator or keyword. */
static Token *accept(const char *text)
{
    Token *t = peek(0);
    if ((t->kind == T_OP || t->kind == T_KW) && strcmp(t->text, text) == 0)
        return next();
    return NULL;
}

static Token *expect(const char *text)
{
    Token *t = next();
    if (strcmp(t->text, text) != 0)
        error(t->line, "expected '%s' but found '%s'", text, t->kind == T_EOF ? "end of file" : t->text);
    return t;
}

static const char *ident(void)
{
    Token *t = next();
    if (t->kind != T_ID)
        error(t->line, "expected a name but found '%s'", t->text);
    return t->text;
}

static int peek_is(int k, const char *text)
{
    return strcmp(peek(k)->text, text) == 0;
}

static Expr *new_expr(ExprKind kind)
{
    Expr *e = alloc(sizeof *e);
    e->kind = kind;
    return e;
}

static Expr *num_expr(long value)
{
    Expr *e = new_expr(E_NUM);
    e->value = value;
    return e;
}

static Expr *bin_expr(const char *op, Expr *a, Expr *b)
{
    Expr *e = new_expr(E_BIN);
    e->op = op;
    e->a = a;
    e->b = b;
    return e;
}

static Stmt *new_stmt(StmtKind kind, int line)
{
    Stmt *s = alloc(sizeof *s);
    s->kind = kind;
    s->line = line;
    return s;
}

static Stmt *assign_stmt(int line, const char *name, const char *op, Expr *value)
{
    Stmt *s = new_stmt(S_ASSIGN, line);
    s->name = name;
    s->op = op;
    s->value = value;
    return s;
}

static void block_add(Stmt *block, Stmt *s, int *capacity)
{
    if (block->count == *capacity) {
        Stmt **items;
        *capacity = *capacity ? *capacity * 2 : 8;
        items = alloc(sizeof(Stmt *) * (size_t)*capacity);
        if (block->count)
            memcpy(items, block->items, sizeof(Stmt *) * (size_t)block->count);
        block->items = items;
    }
    block->items[block->count++] = s;
}

static Expr *parse_expr(int level);
static Stmt *parse_statement(void);

/* Operator levels, lowest priority first. Each list ends with NULL. */
static const char *const LEVELS[][5] = {
    {"||", NULL}, {"&&", NULL}, {"|", NULL}, {"&", NULL},
    {"==", "!=", NULL}, {"<", "<=", ">", ">=", NULL},
    {"+", "-", NULL}, {"*", "/", "%", NULL},
};
#define NUM_LEVELS 8

static int in_level(int level, const char *text)
{
    int i;
    for (i = 0; LEVELS[level][i]; i++)
        if (strcmp(LEVELS[level][i], text) == 0)
            return 1;
    return 0;
}

static Expr *parse_primary(void)
{
    Token *t = next();

    if (t->kind == T_NUM)
        return num_expr(strtol(t->text, NULL, 0));
    if (t->kind == T_ID) {
        if (accept("(")) {
            Expr *call = new_expr(E_CALL);
            Expr *args[32];
            call->name = t->text;
            call->line = t->line;
            if (!accept(")")) {
                for (;;) {
                    if (call->arg_count == 32)
                        error(t->line, "too many arguments");
                    args[call->arg_count++] = parse_expr(0);
                    if (accept(")"))
                        break;
                    expect(",");
                }
            }
            call->args = alloc(sizeof(Expr *) * (size_t)(call->arg_count + 1));
            memcpy(call->args, args, sizeof(Expr *) * (size_t)call->arg_count);
            return call;
        } else {
            Expr *var = new_expr(E_VAR);
            var->name = t->text;
            return var;
        }
    }
    if (strcmp(t->text, "(") == 0) {
        Expr *e = parse_expr(0);
        expect(")");
        return e;
    }
    error(t->line, "unexpected '%s' in expression", t->text);
    return NULL;
}

static Expr *parse_unary(void)
{
    if (accept("-")) {
        Expr *inner = parse_unary();
        Expr *e;
        if (inner->kind == E_NUM)
            return num_expr(-inner->value);
        e = new_expr(E_UN);
        e->op = "-";
        e->a = inner;
        return e;
    }
    if (accept("+"))
        return parse_unary();
    if (accept("!")) {
        Expr *e = new_expr(E_UN);
        e->op = "!";
        e->a = parse_unary();
        return e;
    }
    return parse_primary();
}

static Expr *parse_expr(int level)
{
    Expr *left;

    if (level == NUM_LEVELS)
        return parse_unary();
    left = parse_expr(level + 1);
    while (peek(0)->kind == T_OP && in_level(level, peek(0)->text)) {
        const char *op = next()->text;
        left = bin_expr(op, left, parse_expr(level + 1));
    }
    return left;
}

/* int a = 1, b; */
static Stmt *parse_declaration(void)
{
    int line = expect("int")->line;
    Stmt *block = new_stmt(S_BLOCK, line);
    int capacity = 0;

    for (;;) {
        Stmt *decl = new_stmt(S_DECL, line);
        decl->name = ident();
        decl->value = accept("=") ? parse_expr(0) : NULL;
        block_add(block, decl, &capacity);
        if (!accept(","))
            break;
    }
    return block->count == 1 ? block->items[0] : block;
}

/* x = ..., x++, printf(...) or a plain function call. */
static Stmt *parse_simple(void)
{
    Token *t = peek(0);
    int line = t->line;

    if (t->kind == T_OP && (strcmp(t->text, "++") == 0 || strcmp(t->text, "--") == 0)) {
        next();
        return assign_stmt(line, ident(), t->text[0] == '+' ? "+=" : "-=", num_expr(1));
    }
    if (t->kind == T_ID && strcmp(t->text, "printf") == 0) {
        Token *fmt;
        Stmt *s;
        next();
        expect("(");
        fmt = next();
        if (fmt->kind != T_STR || strchr(fmt->text, '%') != strrchr(fmt->text, '%') || !strchr(fmt->text, '%'))
            error(line, "only printf(\"...%%d...\", value) is supported");
        expect(",");
        s = new_stmt(S_PRINT, line);
        s->value = parse_expr(0);
        expect(")");
        return s;
    }
    if (t->kind == T_ID && (peek_is(1, "=") || peek_is(1, "+=") || peek_is(1, "-=") ||
                            peek_is(1, "*=") || peek_is(1, "++") || peek_is(1, "--"))) {
        const char *name = ident();
        const char *op = next()->text;
        if (strcmp(op, "++") == 0)
            return assign_stmt(line, name, "+=", num_expr(1));
        if (strcmp(op, "--") == 0)
            return assign_stmt(line, name, "-=", num_expr(1));
        return assign_stmt(line, name, op, parse_expr(0));
    } else {
        Stmt *s = new_stmt(S_EXPR, line);
        s->value = parse_expr(0);
        return s;
    }
}

static Stmt *parse_block(void)
{
    int line = expect("{")->line;
    Stmt *block = new_stmt(S_BLOCK, line);
    int capacity = 0;

    while (!accept("}"))
        block_add(block, parse_statement(), &capacity);
    return block;
}

static Stmt *parse_statement(void)
{
    Token *t = peek(0);
    int line = t->line;
    Stmt *s;

    if (strcmp(t->text, "{") == 0)
        return parse_block();
    if (strcmp(t->text, ";") == 0) {
        next();
        return new_stmt(S_BLOCK, line);
    }
    if (strcmp(t->text, "int") == 0) {
        s = parse_declaration();
        expect(";");
        return s;
    }
    if (strcmp(t->text, "if") == 0) {
        next();
        s = new_stmt(S_IF, line);
        expect("(");
        s->cond = parse_expr(0);
        expect(")");
        s->then = parse_statement();
        s->other = accept("else") ? parse_statement() : NULL;
        return s;
    }
    if (strcmp(t->text, "while") == 0) {
        next();
        s = new_stmt(S_WHILE, line);
        expect("(");
        s->cond = parse_expr(0);
        expect(")");
        s->body = parse_statement();
        return s;
    }
    if (strcmp(t->text, "for") == 0) {
        next();
        s = new_stmt(S_FOR, line);
        expect("(");
        if (!peek_is(0, ";"))
            s->init = peek_is(0, "int") ? parse_declaration() : parse_simple();
        expect(";");
        s->cond = peek_is(0, ";") ? num_expr(1) : parse_expr(0);
        expect(";");
        if (!peek_is(0, ")"))
            s->step = parse_simple();
        expect(")");
        s->body = parse_statement();
        return s;
    }
    if (strcmp(t->text, "return") == 0) {
        next();
        s = new_stmt(S_RETURN, line);
        s->value = peek_is(0, ";") ? NULL : parse_expr(0);
        expect(";");
        return s;
    }
    s = parse_simple();
    expect(";");
    return s;
}

static void add_global(const char *name, long value)
{
    int i;
    for (i = 0; i < global_count; i++) {
        if (strcmp(globals[i].name, name) == 0) {
            globals[i].value = value;
            return;
        }
    }
    globals[global_count].name = name;
    globals[global_count].value = value;
    global_count++;
}

/* The whole file: global variables and functions. */
static void parse_program(void)
{
    functions = alloc(sizeof(Function) * (size_t)(token_count + 1));
    globals = alloc(sizeof(Global) * (size_t)(token_count + 1));
    function_count = global_count = 0;
    pos = 0;

    while (peek(0)->kind != T_EOF) {
        int line = peek(0)->line;
        const char *type = next()->text;
        const char *name;

        if (strcmp(type, "int") != 0 && strcmp(type, "void") != 0)
            error(line, "expected 'int' or 'void'");
        name = ident();

        if (accept("(")) {
            Function *f = &functions[function_count++];
            f->name = name;
            f->line = line;
            f->is_void = strcmp(type, "void") == 0;
            if (!accept(")")) {
                if (peek_is(0, "void") && peek_is(1, ")")) {
                    next();
                    next();
                } else {
                    for (;;) {
                        expect("int");
                        list_add(&f->params, (char *)ident());
                        if (accept(")"))
                            break;
                        expect(",");
                    }
                }
            }
            f->body = parse_block();
        } else {
            for (;;) {
                long value = 0;
                if (accept("=")) {
                    int negative = accept("-") != NULL;
                    Token *t = next();
                    if (t->kind != T_NUM)
                        error(t->line, "global initialiser must be a constant");
                    value = strtol(t->text, NULL, 0);
                    if (negative)
                        value = -value;
                }
                add_global(name, value);
                if (!accept(","))
                    break;
                name = ident();
            }
            expect(";");
        }
    }
}

/* ===================================================================== */
/* Step 3: walk the tree and write assembly                               */
/* ===================================================================== */

/* What we know about each function after writing its code. */
typedef struct {
    const char *name;
    StrList locals;     /* parameters first, then local variables */
    StrList calls;      /* the functions it calls */
    int pushes;         /* most registers it saves before one call */
} FuncInfo;

static StrList out;             /* the assembly lines */
static char **src_lines;
static int src_line_count;
static char *commented;         /* which source lines are already copied as comments */
static StrList consts;          /* "const_N" labels for big numbers */
static long *const_values;
static int label_no;
static FuncInfo *infos;
static int info_count;
static FuncInfo *current;       /* the function we are writing now */

static void emit(const char *mnemonic, const char *operands, const char *comment)
{
    char *text = format("        %-5s %s", mnemonic, operands);
    size_t length;

    if (comment && *comment)
        text = format("%-34s; %s", text, comment);
    length = strlen(text);
    while (length > 0 && text[length - 1] == ' ')
        text[--length] = '\0';
    list_add(&out, text);
}

static void emit_label(const char *name)
{
    list_add(&out, format("%s:", name));
}

/* New labels with the same number, like power.while1 and power.endwhile1. */
static int new_label_number(void)
{
    return ++label_no;
}

static char *label_name(const char *kind, int number)
{
    return format("%s.%s%d", current->name, kind, number);
}

static char *strip(const char *s)
{
    const char *end;
    while (isspace((unsigned char)*s))
        s++;
    end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1]))
        end--;
    return format("%.*s", (int)(end - s), s);
}

/* Copy a C source line into the output as a comment, once. */
static void source_comment(int line)
{
    if (line > 0 && line <= src_line_count && !commented[line]) {
        commented[line] = 1;
        list_add(&out, format("        ; %s", strip(src_lines[line - 1])));
    }
}

static const char *reg(int r)
{
    if (r > MAX_REG)
        error(0, "expression too complex (needs more than %d registers)", MAX_REG);
    return format("R%d", r);
}

static int find_global(const char *name)
{
    int i;
    for (i = 0; i < global_count; i++)
        if (strcmp(globals[i].name, name) == 0)
            return i;
    return -1;
}

/* The memory label of a variable: function.name for locals, name for globals. */
static const char *var_label(const char *name, int line)
{
    if (list_find(&current->locals, name) >= 0)
        return format("%s.%s", current->name, name);
    if (find_global(name) >= 0)
        return name;
    error(line, "'%s' is not declared", name);
    return NULL;
}

static Function *find_function(const char *name)
{
    int i;
    for (i = function_count - 1; i >= 0; i--)    /* the last one wins */
        if (strcmp(functions[i].name, name) == 0)
            return &functions[i];
    return NULL;
}

static FuncInfo *find_info(const char *name)
{
    int i;
    for (i = info_count - 1; i >= 0; i--)
        if (strcmp(infos[i].name, name) == 0)
            return &infos[i];
    return NULL;
}

/* Find every local variable of a function, so we know its memory words. */
static void collect_locals(const Stmt *s, StrList *found)
{
    int i;

    if (!s)
        return;
    switch (s->kind) {
    case S_DECL:
        if (list_find(found, s->name) >= 0)
            error(s->line, "'%s' declared twice in %s()", s->name, current->name);
        list_add(found, (char *)s->name);
        break;
    case S_BLOCK:
        for (i = 0; i < s->count; i++)
            collect_locals(s->items[i], found);
        break;
    case S_IF:
        collect_locals(s->then, found);
        collect_locals(s->other, found);
        break;
    case S_WHILE:
        collect_locals(s->body, found);
        break;
    case S_FOR:
        collect_locals(s->init, found);
        collect_locals(s->body, found);
        break;
    default:
        break;
    }
}

/* ----- expressions ----- */

static void gen_expr(const Expr *e, int r);
static void gen_branch(const Expr *e, const char *label, int jump_if, int r);

static const char *arith_mnemonic(const char *op)
{
    if (strcmp(op, "+") == 0) return "ADD";
    if (strcmp(op, "-") == 0) return "SUB";
    if (strcmp(op, "*") == 0) return "MUL";
    if (strcmp(op, "&") == 0) return "AND";
    if (strcmp(op, "|") == 0) return "OR";
    return NULL;
}

/* The branch for a compare, or its opposite when we jump on "false". */
static const char *compare_branch(const char *op, int jump_if)
{
    static const char *const OPS[]      = { "==", "!=", "<", ">=", ">", "<=" };
    static const char *const BRANCH[]   = { "BEQ", "BNE", "BLT", "BGE", "BGT", "BLE" };
    static const char *const OPPOSITE[] = { "BNE", "BEQ", "BGE", "BLT", "BLE", "BGT" };
    int i;
    for (i = 0; i < 6; i++)
        if (strcmp(op, OPS[i]) == 0)
            return jump_if ? BRANCH[i] : OPPOSITE[i];
    return NULL;
}

static void load_const(long value, int r)
{
    value = ((value + 0x8000) & 0xFFFF) - 0x8000;   /* wrap around to 16 bits */
    if (value >= -256 && value <= 255) {
        emit("LI", format("%s, %ld", reg(r), value), NULL);
    } else {
        char *label = format("const_%ld", value & 0xFFFF);
        if (list_find(&consts, label) < 0) {
            const_values[consts.count] = value & 0xFFFF;
            list_add(&consts, label);
        }
        emit("LD", format("%s, %s", reg(r), label), NULL);
    }
}

static void gen_call(const Expr *e, int r)
{
    Function *callee = find_function(e->name);
    int live = r - 1;   /* registers R1..R(r-1) hold values we still need after the call */
    int i;

    if (!callee)
        error(e->line, "call to undefined function '%s'", e->name);
    if (e->arg_count != callee->params.count)
        error(e->line, "%s() expects %d argument(s)", e->name, callee->params.count);
    if (list_find(&current->calls, e->name) < 0)
        list_add(&current->calls, (char *)e->name);

    for (i = 1; i <= live; i++)
        emit("PUSH", format("R%d", i), format("save R%d during the call", i));
    /* work out all the arguments first, then store them */
    for (i = 0; i < e->arg_count; i++)
        gen_expr(e->args[i], r + i);
    for (i = 0; i < callee->params.count; i++)
        emit("ST", format("%s, %s.%s", reg(r + i), e->name, callee->params.items[i]),
             format("argument %s", callee->params.items[i]));
    emit("CALL", e->name, NULL);
    if (live > current->pushes)
        current->pushes = live;
    if (r != 1)
        emit("MOV", format("%s, R1", reg(r)), "return value");
    for (i = live; i >= 1; i--)
        emit("POP", format("R%d", i), NULL);
}

/* Write code that puts the value of e into register R<r>. */
static void gen_expr(const Expr *e, int r)
{
    if (e->kind == E_NUM) {
        load_const(e->value, r);
    } else if (e->kind == E_VAR) {
        emit("LD", format("%s, %s", reg(r), var_label(e->name, 0)), NULL);
    } else if (e->kind == E_CALL) {
        gen_call(e, r);
    } else if (e->kind == E_UN && strcmp(e->op, "-") == 0) {
        gen_expr(e->a, r);
        emit("SUB", format("%s, R0, %s", reg(r), reg(r)), NULL);
    } else if (e->kind == E_BIN && arith_mnemonic(e->op)) {
        int is_add = strcmp(e->op, "+") == 0;
        int is_sub = strcmp(e->op, "-") == 0;
        gen_expr(e->a, r);
        if ((is_add || is_sub) && e->b->kind == E_NUM &&
            (is_add ? e->b->value : -e->b->value) >= -32 && (is_add ? e->b->value : -e->b->value) <= 31) {
            /* small number: one ADDI instead of LI + ADD */
            emit("ADDI", format("%s, %s, %ld", reg(r), reg(r), is_add ? e->b->value : -e->b->value), NULL);
        } else {
            gen_expr(e->b, r + 1);
            emit(arith_mnemonic(e->op), format("%s, %s, %s", reg(r), reg(r), reg(r + 1)), NULL);
        }
    } else if (e->kind == E_BIN && (strcmp(e->op, "/") == 0 || strcmp(e->op, "%") == 0)) {
        error(0, "'%s' is not supported: SPARTAN-16 has no divide instruction", e->op);
    } else {
        /* a compare used as a value, so the answer is 0 or 1 */
        int n = new_label_number();
        char *false_label = label_name("false", n);
        char *done_label = label_name("done", n);
        gen_branch(e, false_label, 0, r);
        emit("LI", format("%s, 1", reg(r)), NULL);
        emit("JMP", done_label, NULL);
        emit_label(false_label);
        emit("LI", format("%s, 0", reg(r)), NULL);
        emit_label(done_label);
    }
}

/* Jump to label if e is true (jump_if = 1) or false (jump_if = 0). */
static void gen_branch(const Expr *e, const char *label, int jump_if, int r)
{
    if (e->kind == E_BIN && compare_branch(e->op, 1)) {
        const char *rhs;
        gen_expr(e->a, r);
        if (e->b->kind == E_NUM && e->b->value == 0) {
            rhs = "R0";
        } else {
            gen_expr(e->b, r + 1);
            rhs = reg(r + 1);
        }
        emit("CMP", format("%s, %s", reg(r), rhs), NULL);
        emit(compare_branch(e->op, jump_if), label, NULL);
    } else if (e->kind == E_BIN && (strcmp(e->op, "&&") == 0 || strcmp(e->op, "||") == 0)) {
        int is_and = strcmp(e->op, "&&") == 0;
        if (is_and != jump_if) {    /* "&&" jumping when false, or "||" jumping when true */
            gen_branch(e->a, label, jump_if, r);
            gen_branch(e->b, label, jump_if, r);
        } else {
            char *skip = label_name("skip", new_label_number());
            gen_branch(e->a, skip, !jump_if, r);
            gen_branch(e->b, label, jump_if, r);
            emit_label(skip);
        }
    } else if (e->kind == E_UN && strcmp(e->op, "!") == 0) {
        gen_branch(e->a, label, !jump_if, r);
    } else if (e->kind == E_NUM) {
        if ((e->value != 0) == jump_if)
            emit("JMP", label, NULL);
    } else {
        gen_expr(e, r);
        emit("CMP", format("%s, R0", reg(r)), NULL);
        emit(jump_if ? "BNE" : "BEQ", label, NULL);
    }
}

/* ----- statements ----- */

static void gen_stmt(const Stmt *s)
{
    int i;

    if (s->kind == S_BLOCK) {
        for (i = 0; i < s->count; i++)
            gen_stmt(s->items[i]);
        return;
    }
    source_comment(s->line);

    switch (s->kind) {
    case S_DECL:
        if (s->value) {
            gen_expr(s->value, 1);
            emit("ST", format("R1, %s", var_label(s->name, s->line)), NULL);
        }
        break;

    case S_ASSIGN: {
        Expr *value = s->value;
        if (strcmp(s->op, "=") != 0) {      /* x += e  is  x = x + e */
            Expr *var = new_expr(E_VAR);
            var->name = s->name;
            value = bin_expr(format("%c", s->op[0]), var, value);
        }
        gen_expr(value, 1);
        emit("ST", format("R1, %s", var_label(s->name, s->line)), NULL);
        break;
    }

    case S_PRINT:
        gen_expr(s->value, 1);
        emit("OUT", "R1", NULL);
        break;

    case S_EXPR:
        gen_expr(s->value, 1);
        break;

    case S_RETURN:
        if (s->value)
            gen_expr(s->value, 1);
        emit("RET", "", NULL);
        break;

    case S_IF: {
        int n = new_label_number();
        char *else_label = label_name("else", n);
        char *end_label = s->other ? label_name("endif", n) : else_label;
        gen_branch(s->cond, else_label, 0, 1);
        gen_stmt(s->then);
        if (s->other) {
            emit("JMP", end_label, NULL);
            emit_label(else_label);
            gen_stmt(s->other);
        }
        emit_label(end_label);
        break;
    }

    case S_WHILE: {
        int n = new_label_number();
        char *top = label_name("while", n);
        char *end = label_name("endwhile", n);
        emit_label(top);
        gen_branch(s->cond, end, 0, 1);
        gen_stmt(s->body);
        emit("JMP", top, NULL);
        emit_label(end);
        break;
    }

    case S_FOR: {
        int n;
        char *top, *end;
        if (s->init)
            gen_stmt(s->init);
        n = new_label_number();
        top = label_name("for", n);
        end = label_name("endfor", n);
        emit_label(top);
        gen_branch(s->cond, end, 0, 1);
        gen_stmt(s->body);
        if (s->step)
            gen_stmt(s->step);
        emit("JMP", top, NULL);
        emit_label(end);
        break;
    }

    default:
        break;
    }
}

/* ----- functions and the whole program ----- */

static char *dashes(int n, char c)
{
    char *s = alloc((size_t)n + 1);
    memset(s, c, (size_t)n);
    return s;
}

static void gen_function(const Function *f)
{
    char *signature;
    const char *last;
    int i;

    current = &infos[info_count++];
    current->name = f->name;
    memset(commented, 0, (size_t)src_line_count + 1);
    for (i = 0; i < f->params.count; i++)
        list_add(&current->locals, f->params.items[i]);
    collect_locals(f->body, &current->locals);

    signature = format("%s %s(", f->is_void ? "void" : "int", f->name);
    for (i = 0; i < f->params.count; i++)
        signature = format("%s%sint %s", signature, i ? ", " : "", f->params.items[i]);
    signature = format("%s)", signature);

    list_add(&out, "");
    list_add(&out, format("; %s", dashes(60, '-')));
    list_add(&out, format("; %s", signature));
    list_add(&out, format("; %s", dashes(60, '-')));
    emit_label(f->name);
    gen_stmt(f->body);

    /* add a RET if the function doesn't end with one */
    last = out.items[out.count - 1];
    while (isspace((unsigned char)*last))
        last++;
    if (strncmp(last, "RET", 3) != 0)
        emit("RET", "", format("end of %s", f->name));
}

/* How many stack words a function needs, counting the functions it calls. */
static int stack_need(const char *name, const char **path, int depth)
{
    FuncInfo *info = find_info(name);
    int i, deepest = -1;

    for (i = 0; i < depth; i++) {
        if (strcmp(path[i], name) == 0) {
            char *chain = format("%s", path[0]);
            int j;
            for (j = 1; j < depth; j++)
                chain = format("%s -> %s", chain, path[j]);
            chain = format("%s -> %s", chain, name);
            error(0, "recursion (%s) is not supported: SPARTAN-16 C uses static frames", chain);
        }
    }
    path[depth] = name;
    for (i = 0; i < info->calls.count; i++) {
        int need = stack_need(info->calls.items[i], path, depth + 1);
        if (need > deepest)
            deepest = need;
    }
    return deepest < 0 ? 0 : info->pushes + 1 + deepest;
}

/* True if this line takes up one word of memory (an instruction or a .word). */
static int takes_a_word(const char *line)
{
    char *text = format("%s", line);
    char *p = strchr(text, ';');
    if (p)
        *p = '\0';
    text = strip(text);
    if (!*text)
        return 0;
    p = strchr(text, ':');
    if (p)
        text = strip(p + 1);
    return *text != '\0';
}

static char *generate(const char *name)
{
    StrList body;
    const char **path;
    int i, j, stack_words, total;
    size_t length = 0;
    char *result;

    if (!find_function("main"))
        error(0, "no main() function");

    infos = alloc(sizeof(FuncInfo) * (size_t)(function_count + 1));
    const_values = alloc(sizeof(long) * (size_t)(token_count + 1));
    commented = alloc((size_t)src_line_count + 2);

    /* the code for every function */
    for (i = 0; i < function_count; i++)
        gen_function(find_function(functions[i].name));
    path = alloc(sizeof(char *) * (size_t)(function_count + 2));
    stack_words = 1 + stack_need("main", path, 0);  /* +1 for the return address that _start pushes */
    if (stack_words < 4)
        stack_words = 4;

    /* header and start-up code go first */
    body = out;
    memset(&out, 0, sizeof out);
    list_add(&out, format("; %s", dashes(60, '=')));
    list_add(&out, format("; %s compiled to SPARTAN-16 assembly by s16cc", name));
    list_add(&out, "; each C variable has its own memory word, named function.name");
    list_add(&out, format("; %s", dashes(60, '=')));
    list_add(&out, "_start:");
    emit("LI", "SP, stack_top", "SP points just past the stack");
    emit("CALL", "main", "run the C program");
    emit("HALT", "", "main is done, stop");
    for (i = 0; i < body.count; i++)
        list_add(&out, body.items[i]);

    /* one memory word for each variable */
    list_add(&out, "");
    list_add(&out, format("; %s", dashes(60, '-')));
    list_add(&out, "; data: one memory word for each C variable");
    list_add(&out, format("; %s", dashes(60, '-')));
    for (i = 0; i < global_count; i++)
        list_add(&out, format("%-16s .word %ld", format("%s:", globals[i].name), globals[i].value));
    for (i = 0; i < function_count; i++) {
        FuncInfo *info = find_info(functions[i].name);
        for (j = 0; j < info->locals.count; j++)
            list_add(&out, format("%-16s .word 0", format("%s.%s:", info->name, info->locals.items[j])));
    }
    for (i = 0; i < consts.count; i++)
        list_add(&out, format("%-16s .word %ld", format("%s:", consts.items[i]), const_values[i]));

    /* make the stack a bit bigger so the program ends on a full memory row */
    total = stack_words;
    for (i = 0; i < out.count; i++)
        total += takes_a_word(out.items[i]);
    stack_words += (8 - total % 8) % 8;
    list_add(&out, "");
    list_add(&out, format("; stack (%d words, it grows down from stack_top)", stack_words));
    list_add(&out, format("%-16s .space %d", "stack:", stack_words));
    list_add(&out, "stack_top:");

    /* join the lines into one string */
    for (i = 0; i < out.count; i++)
        length += strlen(out.items[i]) + 1;
    result = malloc(length + 1);
    if (!result) {
        fprintf(stderr, "out of memory\n");
        exit(1);
    }
    result[0] = '\0';
    length = 0;
    for (i = 0; i < out.count; i++) {
        size_t n = strlen(out.items[i]);
        memcpy(result + length, out.items[i], n);
        length += n;
        result[length++] = '\n';
    }
    result[length] = '\0';
    return result;
}

/* Split the source into lines so we can copy them as comments. */
static void split_source_lines(const char *source)
{
    const char *p = source;
    int capacity = 1;

    for (; *p; p++)
        if (*p == '\n')
            capacity++;
    src_lines = alloc(sizeof(char *) * (size_t)capacity);
    src_line_count = 0;
    p = source;
    while (*p) {
        const char *end = strchr(p, '\n');
        size_t n = end ? (size_t)(end - p) : strlen(p);
        src_lines[src_line_count++] = format("%.*s", (int)n, p);
        p += n;
        if (*p == '\n')
            p++;
    }
}

char *compile_c(const char *source, const char *name, char *err, size_t err_size)
{
    char *result = NULL;

    error_text = err;
    error_size = err_size;
    chunks = NULL;
    memset(&out, 0, sizeof out);
    memset(&consts, 0, sizeof consts);
    label_no = 0;
    info_count = 0;

    if (setjmp(on_error) == 0) {
        split_source_lines(source);
        tokenize(source);
        parse_program();
        result = generate(name);
    }
    free_all();
    return result;
}
