# SPARTAN-16 Makefile
#
#   make           build the ./spartan program
#   make test      build and run the tests
#   make outputs   save the traces of our programs in output/
#   make report    build the PDF report (needs pdflatex)
#   make clean     remove everything that was built

CC     ?= cc
CFLAGS ?= -std=c99 -Wall -Wextra -pedantic -O2

PARTS   = src/isa.c src/components.c src/cpu.c src/loader.c src/assembler.c src/compiler.c src/display.c
HEADERS = src/isa.h src/components.h src/cpu.h src/loader.h src/assembler.h src/compiler.h src/display.h
PDF     = SPARTAN16_CPU_Design_Report.pdf

all: spartan

spartan: $(PARTS) src/main.c $(HEADERS)
	$(CC) $(CFLAGS) -o spartan $(PARTS) src/main.c

build:
	mkdir -p build

build/test_spartan: $(PARTS) tests/test_spartan.c $(HEADERS) | build
	$(CC) $(CFLAGS) -Isrc -o build/test_spartan $(PARTS) tests/test_spartan.c

build/gen_report: $(PARTS) report/gen_report.c $(HEADERS) | build
	$(CC) $(CFLAGS) -Isrc -o build/gen_report $(PARTS) report/gen_report.c

test: build/test_spartan
	./build/test_spartan

outputs: spartan
	mkdir -p output
	./spartan run programs/factorial_of_3.asm --no-color > output/factorial_of_3_trace.txt
	./spartan run programs/factorial.asm --no-color > output/factorial_trace.txt
	./spartan run programs/power.c --no-color > output/power_trace.txt
	./spartan asm programs/factorial.asm > output/factorial_listing.txt
	./spartan compile programs/power.c -o output/power.asm

# pdflatex runs twice: the second run fills in the total page count
report: build/gen_report
	mkdir -p report/generated output
	./build/gen_report
	cd report && pdflatex -interaction=nonstopmode -halt-on-error report.tex > /dev/null || (tail -20 report.log; exit 1)
	cd report && pdflatex -interaction=nonstopmode -halt-on-error report.tex > /dev/null || (tail -20 report.log; exit 1)
	mv report/report.pdf $(PDF)
	rm -f report/report.aux report/report.log report/report.out
	@echo "wrote $(PDF)"

clean:
	rm -rf spartan build report/report.aux report/report.log report/report.out

.PHONY: all test outputs report clean
