# The HP-UX 5.30 machine's media and state -- sourced, not executed.
#
# THE RULE: nothing boots the only copy of anything. Every run of the 540
# changes the disc it boots (HP-UX writes to it) and the battery file
# (HP-UX rewrites its configuration memory, and the clock runs), and what
# the next boot does depends on both. So there are three layers:
#
#   software/cct/7914_unitSave_hpux4.0*.bin.gz
#        the two unit-save tapes, tracked in git. Never written.
#
#   var/pristine/hpux530.bin       (read-only, mode 0444)
#   var/pristine/hpux530-scm.nv    (read-only, mode 0444)
#        the PRISTINE machine: the volume tools/mkhpuxdisc.py assembles
#        from the tapes, exactly, plus a battery that has been through
#        one 3-second throwaway boot so HP-UX has initialised its
#        configuration memory (a blank one fails HP-UX's checksum, is
#        rewritten on the first boot, and throws away any clock seed --
#        measured by the clock lane; FOCUS_RTCTIME=now then gives the
#        right date from the very first real boot). Made once; never
#        booted writable. hpux530.bin.sha256 records what it was.
#
#   var/hpux530/disc.bin, var/hpux530/scm.nv
#        the WORKING machine, a copy of the pristine one, which runs use
#        and keep. tools/runhpux.sh --reset puts it back to pristine;
#        --state DIR keeps a machine somewhere else, so several can exist;
#        --scratch boots a throwaway copy that is deleted afterwards.
#
# HPFOCUS_VAR moves the whole var/ tree (it needs ~300 MB, plus 128 MB for
# each further machine). Everything in it is derived and gitignored.
#
#   hpux_paths            set VAR, PRISTINE_*, CCT, STATE_DEFAULT
#   hpux_make_pristine    assemble var/pristine/hpux530.bin if missing
#   hpux_prime_battery    make var/pristine/hpux530-scm.nv if missing
#                         (needs the emulator; skipped quietly without it)
#   hpux_state_init DIR   make DIR a working machine if it is not one
#   hpux_reset DIR        put DIR back to the pristine machine
#   hpux_lock DIR         refuse a second run on the same machine
#   hpux_copy SRC DST     copy a disc image (a reflink where the file
#                         system can, so a reset costs no space or time)

hpux_paths() {
	VAR=${HPFOCUS_VAR:-$FOCUS_REPO/var}
	PRISTINE_DIR=$VAR/pristine
	PRISTINE_DISC=$PRISTINE_DIR/hpux530.bin
	PRISTINE_NV=$PRISTINE_DIR/hpux530-scm.nv
	CCT=$FOCUS_REPO/software/cct
	STATE_DEFAULT=$VAR/hpux530
}

hpux_die() { echo "error: $*" >&2; exit 1; }

hpux_sha256() {
	python3 -c 'import hashlib, sys
h = hashlib.sha256()
with open(sys.argv[1], "rb") as f:
    for b in iter(lambda: f.read(1 << 20), b""):
        h.update(b)
print(h.hexdigest())' "$1"
}

hpux_copy() {
	local src=$1 dst=$2
	rm -f "$dst.part"
	# GNU cp: a reflink on btrfs/xfs, else a plain copy. macOS cp: -c
	# clones on APFS and fails elsewhere, hence the fallback.
	cp --reflink=auto "$src" "$dst.part" 2>/dev/null \
		|| cp -c "$src" "$dst.part" 2>/dev/null \
		|| cp "$src" "$dst.part" \
		|| { rm -f "$dst.part"; hpux_die "cannot copy $src to $dst (disc full?)"; }
	chmod u+w "$dst.part"
	mv -f "$dst.part" "$dst"
}

# The pristine disc. The tapes are decompressed into var/tmp only for
# the assembly (unless decompressed copies already sit next to the .gz
# files, as tools/runhpux.sh used to leave them), and removed afterwards.
hpux_make_pristine() {
	[ -f "$PRISTINE_DISC" ] && return 0
	local t1=7914_unitSave_hpux4.0.bin t2=7914_unitSave_hpux4.0_tape2.bin
	local tmp=$VAR/tmp tapes=() t made=()
	mkdir -p "$PRISTINE_DIR" "$tmp" || hpux_die "cannot create $VAR"
	for t in "$t1" "$t2"; do
		if [ -f "$CCT/$t" ]; then
			tapes+=("$CCT/$t")
		elif [ -f "$CCT/$t.gz" ]; then
			echo "decompressing $t..."
			gzip -dc "$CCT/$t.gz" > "$tmp/$t.part" \
				|| { rm -f "$tmp/$t.part"; hpux_die "cannot decompress $CCT/$t.gz"; }
			mv -f "$tmp/$t.part" "$tmp/$t"
			tapes+=("$tmp/$t"); made+=("$tmp/$t")
		else
			hpux_die "the unit-save tape $CCT/$t.gz is missing (is this a full clone?)"
		fi
	done
	echo "assembling the pristine HP-UX 5.30 disc from the two unit-save tapes..."
	rm -f "$PRISTINE_DISC.new"
	if ! python3 "$FOCUS_REPO/tools/mkhpuxdisc.py" "${tapes[@]}" "$PRISTINE_DISC.new"; then
		rm -f "$PRISTINE_DISC.new" "${made[@]}"
		hpux_die "tools/mkhpuxdisc.py failed"
	fi
	rm -f ${made[@]+"${made[@]}"}
	rmdir "$tmp" 2>/dev/null || true
	echo "$(hpux_sha256 "$PRISTINE_DISC.new")  hpux530.bin" > "$PRISTINE_DISC.sha256"
	chmod a-w "$PRISTINE_DISC.new"
	mv -f "$PRISTINE_DISC.new" "$PRISTINE_DISC"
	echo "pristine disc: $PRISTINE_DISC (read-only)"
}

# The pristine battery: one throwaway boot of the pristine disc, read-only
# (FOCUS_HDNOSAVE), with the clock left unset (FOCUS_RTCTIME=none), for
# 3 emulated seconds -- by then HP-UX has checked its configuration memory,
# found it blank and rewritten it (an 8-second boot leaves the same memory;
# only the clock bytes differ). About 3 s of wall clock.
hpux_prime_battery() {
	[ -f "$PRISTINE_NV" ] && return 0
	[ -x "$FOCUS_BIN" ] || return 0
	[ -f "$PRISTINE_DISC" ] || return 0
	local log=$PRISTINE_DIR/prime.log
	echo "initialising the pristine battery (one 3-second throwaway boot)..."
	rm -f "$PRISTINE_NV.new"
	( cd "$FOCUS_REPO/mame" && \
	  env -u FOCUS_TYPE -u FOCUS_TYPEAT -u FOCUS_TAP \
		SDL_VIDEODRIVER=dummy FOCUS_HDNOSAVE=1 FOCUS_RTCTIME=none \
		FOCUS_SCMNV="$PRISTINE_NV.new" \
		"$FOCUS_BIN" hp9040 -nothrottle -video none -sound none \
		-mpb3 1m -mpb4 1m -hard1 "$PRISTINE_DISC" -str 3 ) > "$log" 2>&1
	if [ ! -s "$PRISTINE_NV.new" ]; then
		rm -f "$PRISTINE_NV.new"
		echo "warning: the priming boot left no battery file (see $log);" >&2
		echo "         the first boot of each machine will say CLOCK AND DATE NOT SET" >&2
		return 0
	fi
	chmod a-w "$PRISTINE_NV.new"
	mv -f "$PRISTINE_NV.new" "$PRISTINE_NV"
}

# Check the pristine disc is what was assembled. 0.3 s for 128 MB, and the
# only thing that stands between a stray write and every later reset.
hpux_verify_pristine() {
	[ -f "$PRISTINE_DISC.sha256" ] || return 0
	local want got
	want=$(cut -d' ' -f1 < "$PRISTINE_DISC.sha256")
	got=$(hpux_sha256 "$PRISTINE_DISC")
	if [ "$want" != "$got" ]; then
		echo "error: $PRISTINE_DISC has changed since it was assembled." >&2
		echo "Delete it (and $PRISTINE_DISC.sha256) to assemble it again" >&2
		echo "from the tapes: chmod u+w $PRISTINE_DISC; rm $PRISTINE_DISC*" >&2
		exit 1
	fi
}

hpux_state_init() {
	local dir=$1
	mkdir -p "$dir" || hpux_die "cannot create $dir"
	if [ ! -f "$dir/disc.bin" ]; then
		hpux_make_pristine
		hpux_verify_pristine
		hpux_copy "$PRISTINE_DISC" "$dir/disc.bin"
		{
			echo "created  $(date '+%Y-%m-%d %H:%M:%S %z')"
			echo "from     $PRISTINE_DISC"
			if [ -f "$PRISTINE_DISC.sha256" ]; then
				echo "sha256   $(cut -d' ' -f1 < "$PRISTINE_DISC.sha256")"
			fi
		} > "$dir/ORIGIN"
	fi
	if [ ! -f "$dir/scm.nv" ]; then
		hpux_prime_battery
		if [ -f "$PRISTINE_NV" ]; then
			cp "$PRISTINE_NV" "$dir/scm.nv" && chmod u+w "$dir/scm.nv"
		fi
	fi
}

hpux_reset() {
	local dir=$1 f
	# The pristine machine first: a reset must never leave no machine.
	hpux_make_pristine
	hpux_verify_pristine
	hpux_lock "$dir"
	for f in disc.bin disc.bin.part disc.bin.unsaved scm.nv scm.nv.new ORIGIN; do
		rm -f "$dir/$f"
	done
	hpux_unlock
	hpux_state_init "$dir"
	echo "reset: $dir is the pristine machine again"
}

# One run per machine. The emulator itself refuses a second run on a disc
# another run has open once the storage lane's change is in (it flocks the
# image); this catches it before anything is touched, on any build. The
# pid file names the run holding it, and a stale one (no such process) is
# ignored.
HPUX_LOCKED=
hpux_lock() {
	local dir=$1 pid
	mkdir -p "$dir"
	if [ -f "$dir/run.pid" ]; then
		pid=$(cat "$dir/run.pid" 2>/dev/null)
		if [ -n "$pid" ] && [ "$pid" != "$$" ] && kill -0 "$pid" 2>/dev/null; then
			hpux_die "another run (pid $pid) is using $dir; stop it first, or use --state DIR or --scratch"
		fi
	fi
	echo $$ > "$dir/run.pid"
	HPUX_LOCKED=$dir/run.pid
}
hpux_unlock() {
	if [ -n "$HPUX_LOCKED" ]; then
		rm -f "$HPUX_LOCKED"
	fi
	HPUX_LOCKED=
}
