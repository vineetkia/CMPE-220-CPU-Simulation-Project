/*
 * compiler.h
 * s16cc: our small C compiler. It turns a subset of C into SPARTAN-16
 * assembly text. See compiler.c for what it supports.
 */
#ifndef COMPILER_H
#define COMPILER_H

#include <stddef.h>

/* Compile C source text. Returns the assembly text (free it with free()),
 * or NULL with the reason written into err. */
char *compile_c(const char *source, const char *name, char *err, size_t err_size);

#endif
