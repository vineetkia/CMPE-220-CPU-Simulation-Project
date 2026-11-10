# SPARTAN-16: CMPE 220 CPU Design Project

SPARTAN-16 is a 16-bit CPU that we designed ourselves. Everything is written in C:

- a CPU simulator that shows the registers, flags, buses, ALU and memory after every instruction
- an assembler for our assembly language
- `s16cc`, a small C compiler that turns C code into our assembly
- a report (`SPARTAN16_CPU_Design_Report.pdf`) with the CPU diagram, the instruction encoding table and the full runs

You need a C compiler (`cc`, `gcc` or `clang`) and `make`. Nothing else has to be installed.

## How to build and run it

Open a terminal in this folder and build the `spartan` program:

```bash
make
```

If you don't have `make`, this one command does the same thing:

```bash
cc -o spartan src/isa.c src/components.c src/cpu.c src/loader.c src/assembler.c src/compiler.c src/display.c src/main.c
```

Then run the small sample first. It works out 3! = 6 in 19 instructions:

```bash
./spartan run programs/factorial_of_3.asm
```

Program 1 is 8! written straight in assembly. It ends with `Program output: 40320`:

```bash
./spartan run programs/factorial.asm
```

Program 2 is 8^2 written in C. It gets compiled to assembly first, then runs, and ends with `Program output: 64`:

```bash
./spartan run programs/power.c
```

Other commands:

| Command | What it shows |
|---|---|
| `./spartan run programs/power.c --quiet` | only the answer |
| `./spartan asm programs/factorial.asm` | the machine code (address, hex, binary, source) |
| `./spartan compile programs/power.c` | the assembly our compiler made from the C code |
| `./spartan isa` | the list of instructions |
| `make test` | runs our 15 tests (one of them compares with gcc) |
| `make outputs` | saves the runs into the `output/` folder |
| `make report` | builds the PDF again (needs `pdflatex`) |
| `make clean` | removes everything that was built |

We also saved the output of these runs in the `output/` folder.

## What a run shows

`./spartan run` shows the whole simulation in three phases:

1. **Assemble:** each line is translated into a 16-bit word. The assembler looks the instruction name up in the ISA lookup table (`OPCODE_TABLE` in `src/isa.c`), which gives the opcode and format, then fills in the register and address bits.
2. **Load:** the loader (`src/loader.c`) writes each word into RAM over the system bus: the address goes on the address bus, the word on the data bus, and the control bus says MEM WRITE. The program starts at address 0x00, which is where PC starts, so 0x00 is the entry point.
3. **Execute:** the CPU runs fetch, decode, execute for every instruction until HALT, printing the machine state after each one.

Here is what the load phase looks like for the 3! sample:

```
  LOAD  ADDRESS BUS  DATA BUS  CONTROL BUS  RAM WORD           WHAT IT IS
  1     0x00         8409      MEM WRITE    RAM[0x00] = 8409  LD R2, n
  2     0x01         7201      MEM WRITE    RAM[0x01] = 7201  LI R1, 1
  ...
  11    0x0A         0000      MEM WRITE    RAM[0x0A] = 0000  data (result)
```

## Reading the trace

Here is one step from the execute phase of the 3! sample:

```
STEP 4  | PC 0x03 | IR 920A = 1001 001 0 00001010 | ST R1, result
  result : M[0x0A] (result) <- R1 = 3
  bus    : fetch: read M[0x03] = 920A | execute: write M[0x0A] = 0003
  alu    : not used
  cpu    : PC=04 IR=920A MAR=0A MDR=0003   flags Z=0 N=0 C=0 V=0   cycles 4
  regs   : R0=0000  R1=0003  R2=0003  R3=0000  R4=0000  R5=0000  R6=0000  SP=0000
  vars   : n=3  result=3
  ADDR    +0    +1    +2    +3    +4    +5    +6    +7
  0x00   8409  7201  3250  920A >64BF  9409  B202  F200   code
  0x08   0000  0003 [0003] 0000  0000  0000  0000  0000   code/data
```

- `result` is what the instruction did.
- `bus` shows every transfer on the system bus. The fetch always reads the instruction. A load or store then adds a second transfer.
- `alu` shows the ALU's inputs and output, if the ALU was used.
- `cpu` shows PC, IR, MAR, MDR, the flags and how many clock cycles the step took.
- In the memory dump, `>64BF` is the next instruction and `[0003]` was just written. A `*` after a register means it just changed.

## The CPU in short

| | |
|---|---|
| Word size | 16 bits |
| Memory | 256 words of 16 bits (8-bit addresses) |
| Registers | R0 to R7 (R0 is always 0, R7 is SP), plus PC, IR, MAR, MDR and flags Z N C V |
| Buses | address bus (8 bits), data bus (16 bits), control bus |
| Instructions | 16 opcodes, each instruction is one 16-bit word |

Each part of the CPU has its own type and functions in `src/components.c`:

- `Register` is used for PC, IR, MAR and MDR.
- `RegisterFile` holds R0 to R7.
- `Flags` holds Z, N, C and V.
- `ALU` and `alu_compute()` do the math.
- `SystemBus` holds the address, data and control buses.
- `Memory` is the RAM.
- `OutputPort` prints the numbers sent by `OUT`.
- `control_decode()` and `control_branch_taken()` are the control unit.

`src/loader.c` puts the program into RAM, and `src/cpu.c` connects the parts and runs fetch, decode, execute.

| Op | Instruction | Op | Instruction |
|---|---|---|---|
| 0 | `HALT` | 8 | `LD r, addr` or `LD r, [rb+imm5]` |
| 1 | `ADD rd, rs, rt` | 9 | `ST r, addr` or `ST r, [rb+imm5]` |
| 2 | `SUB rd, rs, rt` | A | `CMP rs, rt` |
| 3 | `MUL rd, rs, rt` | B | `JMP/BEQ/BNE/BLT/BGE/BGT/BLE addr` |
| 4 | `AND rd, rs, rt` | C | `CALL addr` |
| 5 | `OR rd, rs, rt` | D | `RET` |
| 6 | `ADDI rd, rs, imm6` | E | `PUSH r` or `POP r` |
| 7 | `LI rd, imm9` | F | `OUT r` |

## Folders

```
Makefile                 builds everything
src/main.c               command line tool
src/components.c         CPU parts: registers, ALU, flags, buses, memory, control unit
src/cpu.c                connects the parts, runs fetch / decode / execute
src/loader.c             writes the program into RAM over the bus
src/isa.c                opcodes, formats, the ISA lookup table, encoding and decoding
src/assembler.c          the assembler (translates with the lookup table)
src/compiler.c           s16cc, the C compiler
src/display.c            prints the trace
programs/                factorial_of_3.asm, factorial.asm, power.c, extras/*.c
tests/test_spartan.c     tests
report/                  report.tex, team.tex and gen_report.c (makes the report tables)
output/                  saved runs
```

## What our C compiler supports

- `int` variables and functions with parameters
- `if`/`else`, `while`, `for` and `return`
- `+ - * & |`, comparisons, `&& || !` and `+= -= *= ++ --`
- `printf("%d\n", x)`

Every C variable gets its own word in memory, so you can watch it change in the trace.

It has a few limits:

- **No recursion:** each variable lives in one fixed memory word, so a function can't call itself.
- **No `/` or `%`:** the CPU has no divide instruction.
- **Unsigned output:** `OUT` prints numbers as unsigned 16-bit values.

## Team

The team names and who did what are in `report/team.tex`. The PDF reads them from there.
