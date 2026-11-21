# SPARTAN-16: CMPE 220 CPU Design Project

SPARTAN-16 is a 16-bit CPU that we designed and simulate in C. The simulator has:

- a CPU with special purpose registers (PC, IR, MAR, MDR, FLAGS), general purpose registers (R0 to R7) and an ALU
- a system bus (address, data and control) and 256 words of RAM
- a clock: every instruction takes 3 ticks, one each for fetch, decode and execute

We wrote two programs in C, translated them **by hand** into our assembly and then into binary, and put the binary **into RAM by hand**. There is no assembler and no loader. The simulator then runs the binary with the fetch, decode, execute cycle and shows the CPU and RAM after every instruction.

The report is `SPARTAN16_CPU_Design_Report.pdf`.

## How to get it and run it

You need `git`, a C compiler (`cc`, `gcc` or `clang`) and `make`.

```bash
git clone https://github.com/vineetkia/CMPE-220-CPU-Simulation-Project.git
```
```bash
cd CMPE-220-CPU-Simulation-Project
```
```bash
make
```

Program 1 adds two numbers (5 + 7). It ends with `Program output: 12`:

```bash
./spartan run add
```

Program 2 multiplies two numbers using only ADD (5 x 3). It ends with `Program output: 15`:

```bash
./spartan run multiply
```

If you don't have `make`, this one command builds `spartan` too:

```bash
cc -o spartan src/isa.c src/components.c src/cpu.c src/programs.c src/display.c src/main.c
```

Other commands:

| Command | What it shows |
|---|---|
| `./spartan run add --quiet` | only the answer |
| `./spartan list` | the programs |
| `./spartan isa` | the instruction table with the binary code of each instruction |
| `make test` | runs our tests (one compares the answers with gcc) |
| `make outputs` | saves both runs into `output/` |
| `make report` | builds the PDF again (needs `pdflatex`) |
| `make clean` | removes everything that was built |

## What a run shows

1. **The C program** we started from (`programs/add.c` or `programs/multiply.c`).
2. **Our hand translation:** each C line, the assembly we wrote for it, and the 16-bit binary we worked out from the instruction table.
3. **RAM after we put the binary in by hand.** The program starts at address 0x00, where PC starts.
4. **Every instruction**, one after another:
   - the three clock ticks: fetch, decode, execute
   - the special purpose registers PC, IR, MAR, MDR in binary, hex, decimal and ASCII, plus FLAGS
   - the general purpose registers R0 to R7 in binary, decimal and ASCII
   - what the ALU did, in binary, decimal and ASCII
   - the RAM block: every word in binary, hex and ASCII

Here is instruction 3 of program 1:

```
INSTRUCTION 3   (clock ticks 7 to 9)   ADD R3, R1, R2
  tick 7   FETCH    MAR <- PC = 02, RAM[02] -> MDR = 1650, IR <- MDR, PC <- 03
  tick 8   DECODE   IR = 0001 011 001 010 000 -> ADD R3, R1, R2
  tick 9   EXECUTE  R3 <- 5 + 7 = 12

  CPU special purpose registers
          binary            hex     decimal  ASCII
    PC    00000011          0x03    3        .
    IR    0001011001010000  0x1650  5712     .P
    MAR   00000010          0x02    2        .
    MDR   0001011001010000  0x1650  5712     .P
    FLAGS  Z=0 N=0 C=0 V=0
  CPU general purpose registers (binary, decimal, ASCII)
    R0 = 0000000000000000 (    0) ..      R4 = 0000000000000000 (    0) ..
    R1 = 0000000000000101 (    5) ..      R5 = 0000000000000000 (    0) ..
    R2 = 0000000000000111 (    7) ..      R6 = 0000000000000000 (    0) ..
    R3 = 0000000000001100 (   12) .. *    SP = 0000000000000000 (    0) ..
  CPU ALU (binary, decimal, ASCII)
    ADD  A = 0000000000000101 (5) ..   B = 0000000000000111 (7) ..
         result = 0000000000001100 (12) ..

  RAM (> next instruction, * just written)
     ADDR  BINARY            HEX   ASCII  CONTENT
     0x00  1000001000000110  8206  ..     LD R1, a
     0x01  1000010000000111  8407  ..     LD R2, b
     0x02  0001011001010000  1650  .P     ADD R3, R1, R2
  >  0x03  1001011000001000  9608  ..     ST R3, c
     ...
```

## The CPU in short

| | |
|---|---|
| Word size | 16 bits |
| RAM | 256 words of 16 bits (8-bit addresses) |
| Special purpose registers | PC, IR, MAR, MDR, FLAGS (Z N C V) |
| General purpose registers | R0 to R7 (R0 is always 0, R7 is SP) |
| Buses | address bus (8 bits), data bus (16 bits), control bus |
| Clock | 3 ticks per instruction: fetch, decode, execute |
| Instructions | 16 opcodes, each instruction is one 16-bit word |

| Opcode | Instruction | Opcode | Instruction |
|---|---|---|---|
| 0000 | `HALT` | 1000 | `LD r, addr` or `LD r, [rb+imm5]` |
| 0001 | `ADD rd, rs, rt` | 1001 | `ST r, addr` or `ST r, [rb+imm5]` |
| 0010 | `SUB rd, rs, rt` | 1010 | `CMP rs, rt` |
| 0011 | `MUL rd, rs, rt` | 1011 | `JMP/BEQ/BNE/BLT/BGE/BGT/BLE addr` |
| 0100 | `AND rd, rs, rt` | 1100 | `CALL addr` |
| 0101 | `OR rd, rs, rt` | 1101 | `RET` |
| 0110 | `ADDI rd, rs, imm6` | 1110 | `PUSH r` or `POP r` |
| 0111 | `LI rd, imm9` | 1111 | `OUT r` |

## Folders

```
Makefile                 builds everything
programs/add.c           program 1 in C
programs/multiply.c      program 2 in C
src/programs.c           both programs translated by hand into assembly and binary
src/components.c         CPU parts: registers, ALU, flags, buses, RAM, clock, control unit
src/cpu.c                the CPU and the fetch, decode, execute cycle
src/isa.c                the instruction set: opcodes, formats, decoding
src/display.c            prints the CPU and RAM in binary, hex and ASCII
src/main.c               command line tool
tests/test_spartan.c     tests
report/                  report.tex, team.tex and gen_report.c (makes the report tables)
output/                  saved runs
```

## Team

- Vineet Kumar: instruction set, CPU parts, clock, fetch / decode / execute cycle
- Sakshat Nandkumar Patil: hand translation of both programs into assembly and binary, trace printer, tests
- Aishwarya Madhave: the two C programs, the CPU diagram, the report and its generator

The PDF reads the names and contributions from `report/team.tex`.
