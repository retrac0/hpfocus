# Standalone builds of the FOCUS sources for testing outside a MAME tree.
# The real build happens inside MAME; this exists so the same source can be
# exercised without one.

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter
SRCDIR    = ../cpu/focus
KNOBS     = $(SRCDIR)/focusknobs.h
# The execution core is one class split across fragments the header
# includes; every one of them is a dependency of anything using it.
CORE      = $(SRCDIR)/focusexec.h $(wildcard $(SRCDIR)/focusexec_*.ipp) \
            $(SRCDIR)/focusdst.h $(SRCDIR)/focusaddr.h \
            $(SRCDIR)/focusstate.h $(KNOBS)

all: dasmtest statetest addrtest dsttest exectest ioptest bootprobe bootrun

dasmtest: dasmtest.cpp $(SRCDIR)/focusdasm.cpp $(SRCDIR)/focusdasm.h \
          $(SRCDIR)/focusdasm_tables.ipp emu.h
	$(CXX) $(CXXFLAGS) -I. dasmtest.cpp $(SRCDIR)/focusdasm.cpp -o $@

# focusstate.h has no MAME dependency, so it needs no shim.
statetest: statetest.cpp $(SRCDIR)/focusstate.h
	$(CXX) $(CXXFLAGS) statetest.cpp -o $@

addrtest: addrtest.cpp $(SRCDIR)/focusaddr.h $(SRCDIR)/focusstate.h $(KNOBS)
	$(CXX) $(CXXFLAGS) addrtest.cpp -o $@

dsttest: dsttest.cpp $(SRCDIR)/focusdst.h $(SRCDIR)/focusaddr.h $(SRCDIR)/focusstate.h $(KNOBS)
	$(CXX) $(CXXFLAGS) dsttest.cpp -o $@

exectest: exectest.cpp $(CORE)
	$(CXX) $(CXXFLAGS) exectest.cpp -o $@

ioptest: ioptest.cpp ../iop/focusiop.h ../iop/focusscm.h $(CORE)
	$(CXX) $(CXXFLAGS) ioptest.cpp -o $@

# Not a test: an experiment harness for locating a boot module's entry point.
bootprobe: bootprobe.cpp ../iop/focusiop.h ../iop/focusscm.h $(CORE)
	$(CXX) $(CXXFLAGS) bootprobe.cpp -o $@

# Not a test: runs a boot image through the reverse-engineered loader handoff.
bootrun: bootrun.cpp ../iop/focusiop.h ../iop/focusscm.h ../iop/focuscards.h \
         $(CORE)
	$(CXX) $(CXXFLAGS) bootrun.cpp -o $@

# The opcode tables are generated; docs/opcodes.csv is the source of truth.
$(SRCDIR)/focusdasm_tables.ipp: ../../docs/opcodes.csv ../../tools/gen_dasm_tables.py
	python3 ../../tools/gen_dasm_tables.py

.PHONY: all test clean
test: all
	@# A source check, not a run: the knob catalogue against the source
	@# that names them. First, because it costs nothing and because
	@# exectest below still has its 15 recorded failures.
	@python3 ../../tools/check_knobs.py
	@echo
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
	rm -f dasmtest statetest addrtest dsttest exectest ioptest bootprobe
