# Standalone build of the FOCUS disassembler for differential testing.
# The real build happens inside MAME; this exists only so the same source can
# be exercised against tools/focusdis.py without a MAME tree.

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter
SRCDIR    = ../cpu/focus

dasmtest: dasmtest.cpp $(SRCDIR)/focusdasm.cpp $(SRCDIR)/focusdasm.h \
          $(SRCDIR)/focusdasm_tables.ipp emu.h
	$(CXX) $(CXXFLAGS) -I. dasmtest.cpp $(SRCDIR)/focusdasm.cpp -o $@

# The opcode tables are generated; docs/opcodes.csv is the source of truth.
$(SRCDIR)/focusdasm_tables.ipp: ../../docs/opcodes.csv ../../tools/gen_dasm_tables.py
	python3 ../../tools/gen_dasm_tables.py

.PHONY: test clean
test: dasmtest
	./run_difftest.sh

clean:
	rm -f dasmtest
