# Standalone builds of the FOCUS sources for testing outside a MAME tree.
# The real build happens inside MAME; this exists so the same source can be
# exercised without one.

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter
SRCDIR    = ../cpu/focus

all: dasmtest statetest addrtest dsttest exectest ioptest

dasmtest: dasmtest.cpp $(SRCDIR)/focusdasm.cpp $(SRCDIR)/focusdasm.h \
          $(SRCDIR)/focusdasm_tables.ipp emu.h
	$(CXX) $(CXXFLAGS) -I. dasmtest.cpp $(SRCDIR)/focusdasm.cpp -o $@

# focusstate.h has no MAME dependency, so it needs no shim.
statetest: statetest.cpp $(SRCDIR)/focusstate.h
	$(CXX) $(CXXFLAGS) statetest.cpp -o $@

addrtest: addrtest.cpp $(SRCDIR)/focusaddr.h $(SRCDIR)/focusstate.h
	$(CXX) $(CXXFLAGS) addrtest.cpp -o $@

dsttest: dsttest.cpp $(SRCDIR)/focusdst.h $(SRCDIR)/focusaddr.h $(SRCDIR)/focusstate.h
	$(CXX) $(CXXFLAGS) dsttest.cpp -o $@

exectest: exectest.cpp $(SRCDIR)/focusexec.h $(SRCDIR)/focusdst.h \
          $(SRCDIR)/focusaddr.h $(SRCDIR)/focusstate.h
	$(CXX) $(CXXFLAGS) exectest.cpp -o $@

ioptest: ioptest.cpp ../iop/focusiop.h $(SRCDIR)/focusexec.h
	$(CXX) $(CXXFLAGS) ioptest.cpp -o $@

# The opcode tables are generated; docs/opcodes.csv is the source of truth.
$(SRCDIR)/focusdasm_tables.ipp: ../../docs/opcodes.csv ../../tools/gen_dasm_tables.py
	python3 ../../tools/gen_dasm_tables.py

.PHONY: all test clean
test: all
	@./statetest
	@echo
	@./addrtest
	@echo
	@./dsttest
	@echo
	@./exectest
	@echo
	@./ioptest
	@echo
	@./run_difftest.sh

clean:
	rm -f dasmtest statetest addrtest dsttest exectest ioptest
