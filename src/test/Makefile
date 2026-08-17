# Standalone builds of the FOCUS sources for testing outside a MAME tree.
# The real build happens inside MAME; this exists so the same source can be
# exercised without one.

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter
SRCDIR    = ../cpu/focus

all: dasmtest statetest

dasmtest: dasmtest.cpp $(SRCDIR)/focusdasm.cpp $(SRCDIR)/focusdasm.h \
          $(SRCDIR)/focusdasm_tables.ipp emu.h
	$(CXX) $(CXXFLAGS) -I. dasmtest.cpp $(SRCDIR)/focusdasm.cpp -o $@

# focusstate.h has no MAME dependency, so it needs no shim.
statetest: statetest.cpp $(SRCDIR)/focusstate.h
	$(CXX) $(CXXFLAGS) statetest.cpp -o $@

# The opcode tables are generated; docs/opcodes.csv is the source of truth.
$(SRCDIR)/focusdasm_tables.ipp: ../../docs/opcodes.csv ../../tools/gen_dasm_tables.py
	python3 ../../tools/gen_dasm_tables.py

.PHONY: all test clean
test: all
	@./statetest
	@echo
	@./run_difftest.sh

clean:
	rm -f dasmtest statetest
