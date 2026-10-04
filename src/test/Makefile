# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Joel Heikkila
#
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

all: dasmtest statetest addrtest dsttest exectest ioptest muxtest hpibtest asitest bootprobe bootrun

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

ioptest: ioptest.cpp ../iop/focusiop.h ../iop/focusscm.h ../iop/focusgfx.h \
         ../iop/focusasi.h ../iop/focuscards.h ../iop/focusprint.h ../iop/focusrtc.h \
         ../iop/focusdisc.h $(CORE)
	$(CXX) $(CXXFLAGS) ioptest.cpp -o $@

muxtest: muxtest.cpp ../iop/focusiop.h ../iop/focusmux.h $(CORE)
	$(CXX) $(CXXFLAGS) muxtest.cpp -o $@

hpibtest: hpibtest.cpp ../iop/focusiop.h ../iop/focushpib.h $(CORE)
	$(CXX) $(CXXFLAGS) hpibtest.cpp -o $@

asitest: asitest.cpp ../iop/focusiop.h ../iop/focusasi.h $(CORE)
	$(CXX) $(CXXFLAGS) asitest.cpp -o $@

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

# Every harness runs, whatever an earlier one reported, and the target
# ends with one line naming the ones that failed (and exits non-zero) or
# saying that all passed. It used to stop at the first failure, and as
# exectest carried stale failures for weeks, ioptest, muxtest and the
# disassembler difftest went unrun all that time. check_knobs.py is a
# source check, not a run: the knob catalogue against the source that
# names them.
TESTS = statetest addrtest dsttest exectest ioptest muxtest hpibtest asitest run_difftest.sh

.PHONY: all test clean
test: all
	@failed=""; skipped=""; \
	python3 ../../tools/check_knobs.py || failed="$$failed check_knobs"; \
	for t in $(TESTS); do \
		echo; \
		./$$t; rc=$$?; \
		if [ $$rc = 77 ]; then skipped="$$skipped $$t"; \
		elif [ $$rc != 0 ]; then failed="$$failed $$t"; fi; \
	done; \
	echo; \
	if [ -n "$$failed" ]; then \
		echo "make test: FAILED:$$failed"; exit 1; \
	fi; \
	if [ -n "$$skipped" ]; then \
		echo "make test: every harness that ran passed; SKIPPED (media not here):$$skipped"; \
	else \
		echo "make test: every harness passed (check_knobs $(TESTS))"; \
	fi

clean:
	rm -f dasmtest statetest addrtest dsttest exectest ioptest muxtest hpibtest asitest bootprobe bootrun
