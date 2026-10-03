#!/usr/bin/env bash
# Fetch Hewlett-Packard's software and manuals, which this repository
# points at (MEDIA.txt) and does not carry.
#
#   tools/fetch-media.sh              the software the emulator boots: six
#                                     tape and disc images, 46 MB
#   tools/fetch-media.sh --manuals    the manuals as well: 35 PDFs and five
#                                     text files, 230 MB; and their text
#                                     in docs/notes, made with pdftotext
#   tools/fetch-media.sh PATH...      only these files, named as MEDIA.txt
#                                     names them
#   tools/fetch-media.sh --check      verify what is here against MEDIA.txt
#                                     (size and SHA-256); no network
#   tools/fetch-media.sh --list       what MEDIA.txt lists: each file, its
#                                     size, where it comes from, and
#                                     whether it is here
#   -h, --help                        this
#
# The files are Hewlett-Packard's, not this project's, and they come from
# third-party archives: bitsavers.org (mirror bitsavers.trailing-edge.com)
# and hparchive.com (mirror: the Internet Archive). MEDIA.txt says which
# file comes from where, and on what terms this project points at them.
#
# What it does for each file that is not here: tries the primary URL, then
# the mirrors, with curl (or wget); resumes a download an earlier run left
# unfinished (PATH.part); checks the size and the SHA-256 against MEDIA.txt
# and only then moves the file into place. A download that does not match
# is deleted and reported, never installed. A file that is already here
# and matches is left alone, so running it again costs a few seconds and
# no network; one that is here and does NOT match is reported and left for
# you to look at. Nothing outside the listed paths is written.
#
# With --manuals (or when a PDF is named), the text extractions in
# docs/notes that MEDIA.txt lists are regenerated from the PDFs with
# pdftotext (poppler-utils), if it is installed. They were made with
# poppler 26.08.0; another version may space a page differently, which is
# reported as a warning and is not an error.
#
# ENVIRONMENT
#   HPFOCUS_MEDIA_CACHE=DIR   a directory laid out like this tree
#                             (DIR/software/cct/..., DIR/docs/bitsavers/...).
#                             A file found there, and matching, is copied
#                             instead of downloaded; a file downloaded is
#                             copied there too. One download then serves
#                             every clone, and a machine with no network
#                             can be fed from a copy of the directory.
#   HPFOCUS_FETCH=curl|wget   use this downloader (default: curl if it is
#                             installed, else wget)
#
# Exit status: 0 when everything asked for is here and verified; 1 when a
# file could not be fetched or does not match; 2 for a usage error.

set -u
cd "$(dirname "$0")/.." || exit 1
ROOT=$(pwd)
MANIFEST=$ROOT/MEDIA.txt
VERSION=0.1
UA="hpfocus-fetch-media/$VERSION (+https://github.com/retrac0/hpfocus)"
NOTES_POPPLER=26.08.0

MODE=fetch
MANUALS=0
NAMED=()
while [ $# -gt 0 ]; do
	case "$1" in
		--manuals|--all) MANUALS=1 ;;
		--check) MODE=check ;;
		--list) MODE=list ;;
		-h|--help) sed -n '2,/^# Exit status/p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
		-*) echo "unknown option '$1' (try --help)" >&2; exit 2 ;;
		*) NAMED+=("${1#./}") ;;
	esac
	shift
done

have() { command -v "$1" >/dev/null 2>&1; }
mb() { echo $(( ($1 + 524288) / 1048576 )); }

# ------------------------------------------------------------ the manifest
# Parallel arrays (bash 3.2 has no associative ones): F_* the files, with
# F_URLS[i] the URLs joined by spaces; T_* the text extractions.
[ -r "$MANIFEST" ] || { echo "error: $MANIFEST is missing: it is the list this script works from" >&2; exit 1; }
F_CLASS=(); F_SIZE=(); F_SHA=(); F_PATH=(); F_URLS=()
T_MODE=(); T_SIZE=(); T_SHA=(); T_PATH=(); T_PDF=()
nf=0; nt=0; lineno=0
while IFS= read -r line || [ -n "$line" ]; do
	lineno=$((lineno + 1))
	case "$line" in
	''|'#'*) continue ;;
	esac
	# shellcheck disable=SC2086
	set -- $line
	case "$1" in
	file)
		[ $# -eq 5 ] || { echo "error: MEDIA.txt line $lineno: file wants CLASS BYTES SHA256 PATH" >&2; exit 1; }
		F_CLASS[nf]=$2; F_SIZE[nf]=$3; F_SHA[nf]=$4; F_PATH[nf]=$5; F_URLS[nf]=
		nf=$((nf + 1)) ;;
	url)
		[ $# -eq 2 ] && [ "$nf" -gt 0 ] || { echo "error: MEDIA.txt line $lineno: a url line follows a file line and holds one URL" >&2; exit 1; }
		F_URLS[nf-1]="${F_URLS[nf-1]}${F_URLS[nf-1]:+ }$2" ;;
	text)
		[ $# -eq 6 ] || { echo "error: MEDIA.txt line $lineno: text wants MODE BYTES SHA256 PATH PDF" >&2; exit 1; }
		T_MODE[nt]=$2; T_SIZE[nt]=$3; T_SHA[nt]=$4; T_PATH[nt]=$5; T_PDF[nt]=$6
		nt=$((nt + 1)) ;;
	*)
		echo "error: MEDIA.txt line $lineno: unknown record '$1'" >&2; exit 1 ;;
	esac
done < "$MANIFEST"
[ "$nf" -gt 0 ] || { echo "error: MEDIA.txt lists no files" >&2; exit 1; }

# Which records this run is about. A named path may be a file or one of
# the text extractions, which brings its PDF with it.
F_WANT=(); T_WANT=()
i=0; while [ "$i" -lt "$nf" ]; do F_WANT[i]=0; i=$((i + 1)); done
i=0; while [ "$i" -lt "$nt" ]; do T_WANT[i]=0; i=$((i + 1)); done
want_file() {
	local p=$1 k=0
	while [ "$k" -lt "$nf" ]; do
		if [ "${F_PATH[k]}" = "$p" ]; then F_WANT[k]=1; return 0; fi
		k=$((k + 1))
	done
	return 1
}
want_texts_of() {
	local pdf=$1 k=0
	while [ "$k" -lt "$nt" ]; do
		[ "${T_PDF[k]}" = "$pdf" ] && T_WANT[k]=1
		k=$((k + 1))
	done
}
if [ ${#NAMED[@]} -gt 0 ]; then
	for p in "${NAMED[@]}"; do
		case "$p" in "$ROOT"/*) p=${p#"$ROOT"/} ;; esac
		if want_file "$p"; then
			want_texts_of "$p"
			continue
		fi
		found=0; i=0
		while [ "$i" -lt "$nt" ]; do
			if [ "${T_PATH[i]}" = "$p" ]; then
				T_WANT[i]=1; want_file "${T_PDF[i]}"; found=1
			fi
			i=$((i + 1))
		done
		if [ "$found" = 0 ]; then
			echo "error: MEDIA.txt does not list '$p' (tools/fetch-media.sh --list shows what it does)" >&2
			exit 2
		fi
	done
else
	i=0
	while [ "$i" -lt "$nf" ]; do
		if [ "${F_CLASS[i]}" = software ] || [ "$MANUALS" = 1 ]; then F_WANT[i]=1; fi
		i=$((i + 1))
	done
	if [ "$MANUALS" = 1 ]; then
		i=0; while [ "$i" -lt "$nt" ]; do T_WANT[i]=1; i=$((i + 1)); done
	fi
fi

# ------------------------------------------------------------ verification
size_of() { wc -c < "$1" | tr -d ' '; }
if have sha256sum; then
	sha_of() { sha256sum "$1" | awk '{ print $1 }'; }
elif have shasum; then
	sha_of() { shasum -a 256 "$1" | awk '{ print $1 }'; }
elif have openssl; then
	sha_of() { openssl dgst -sha256 -r "$1" | awk '{ print $1 }'; }
else
	# The tools need Python anyway (tools/setup.sh checks for it).
	sha_of() {
		python3 -c 'import hashlib, sys
h = hashlib.sha256()
with open(sys.argv[1], "rb") as f:
    for b in iter(lambda: f.read(1 << 20), b""):
        h.update(b)
print(h.hexdigest())' "$1"
	}
fi

# verify FILE BYTES SHA256: status 0 if it matches; else WHY says how not.
WHY=
verify() {
	local got
	got=$(size_of "$1")
	if [ "$got" != "$2" ]; then
		WHY="it is $got bytes, MEDIA.txt says $2"
		return 1
	fi
	got=$(sha_of "$1")
	if [ "$got" != "$3" ]; then
		WHY="its SHA-256 is $got, MEDIA.txt says $3"
		return 1
	fi
	return 0
}

host_of() { local h=${1#*://}; echo "${h%%/*}"; }

# ------------------------------------------------------------------ --list
if [ "$MODE" = list ]; then
	last=
	i=0
	while [ "$i" -lt "$nf" ]; do
		if [ "${F_CLASS[i]}" != "$last" ]; then
			last=${F_CLASS[i]}
			n=0; b=0; k=0
			while [ "$k" -lt "$nf" ]; do
				if [ "${F_CLASS[k]}" = "$last" ]; then n=$((n + 1)); b=$((b + F_SIZE[k])); fi
				k=$((k + 1))
			done
			[ "$i" -gt 0 ] && echo
			case "$last" in
				software) echo "SOFTWARE: $n files, $(mb "$b") MB -- tools/fetch-media.sh" ;;
				manuals) echo "MANUALS: $n files, $(mb "$b") MB -- tools/fetch-media.sh --manuals" ;;
				*) echo "$last: $n files, $(mb "$b") MB" ;;
			esac
		fi
		if [ -f "${F_PATH[i]}" ]; then state=here; else state=absent; fi
		printf '  %-6s %11s  %s\n' "$state" "${F_SIZE[i]}" "${F_PATH[i]}"
		for u in ${F_URLS[i]}; do printf '  %19s %s\n' '' "$u"; done
		i=$((i + 1))
	done
	echo
	echo "TEXT OF THE MANUALS: $nt files in docs/notes, made by pdftotext from the PDFs above"
	i=0
	while [ "$i" -lt "$nt" ]; do
		if [ -f "${T_PATH[i]}" ]; then state=here; else state=absent; fi
		case "${T_MODE[i]}" in layout) how="pdftotext -layout" ;; *) how="pdftotext" ;; esac
		printf '  %-6s %11s  %s\n' "$state" "${T_SIZE[i]}" "${T_PATH[i]}"
		printf '  %19s %s %s\n' '' "$how" "${T_PDF[i]}"
		i=$((i + 1))
	done
	echo
	echo "(here/absent is whether the file exists; --check verifies sizes and hashes.)"
	exit 0
fi

# ----------------------------------------------------------------- --check
# Everything MEDIA.txt lists is looked at; what decides the exit status is
# what was asked for (the software, or with --manuals everything, or the
# named paths): all of it here and matching.
if [ "$MODE" = check ]; then
	status=0
	sw_n=0; sw_ok=0; mn_n=0; mn_ok=0; tx_ok=0; tx_warn=0
	i=0
	while [ "$i" -lt "$nf" ]; do
		p=${F_PATH[i]}
		if [ "${F_CLASS[i]}" = software ]; then sw_n=$((sw_n + 1)); else mn_n=$((mn_n + 1)); fi
		if [ -f "$p" ]; then
			if verify "$p" "${F_SIZE[i]}" "${F_SHA[i]}"; then
				if [ "${F_CLASS[i]}" = software ]; then sw_ok=$((sw_ok + 1)); else mn_ok=$((mn_ok + 1)); fi
			else
				echo "BAD      $p: $WHY"
				status=1
			fi
		elif [ "${F_WANT[i]}" = 1 ]; then
			echo "missing  $p"
			status=1
		fi
		i=$((i + 1))
	done
	i=0
	while [ "$i" -lt "$nt" ]; do
		p=${T_PATH[i]}
		if [ -f "$p" ]; then
			if verify "$p" "${T_SIZE[i]}" "${T_SHA[i]}"; then
				tx_ok=$((tx_ok + 1))
			else
				echo "differs  $p: $WHY"
				tx_warn=$((tx_warn + 1))
			fi
		fi
		i=$((i + 1))
	done
	echo "software: $sw_ok of $sw_n here and verified"
	echo "manuals:  $mn_ok of $mn_n here and verified"
	echo "text of the manuals (docs/notes): $tx_ok of $nt here and as MEDIA.txt lists them"
	if [ "$tx_warn" -gt 0 ]; then
		echo "warning: $tx_warn text file(s) differ from what poppler $NOTES_POPPLER wrote; another"
		echo "         pdftotext may space a page differently ($(pdftotext -v 2>&1 | head -1 || echo 'pdftotext is not installed here'))"
	fi
	if [ "$status" != 0 ]; then
		echo "tools/fetch-media.sh$( [ "$MANUALS" = 1 ] && echo ' --manuals') fetches what is missing; a BAD file is not" >&2
		echo "what MEDIA.txt lists: move it away and fetch again." >&2
	fi
	exit "$status"
fi

# ------------------------------------------------------------------- fetch
TOOL=${HPFOCUS_FETCH:-}
if [ -z "$TOOL" ]; then
	if have curl; then TOOL=curl; elif have wget; then TOOL=wget; fi
fi
case "$TOOL" in
	curl|wget) have "$TOOL" || { echo "error: HPFOCUS_FETCH=$TOOL, and $TOOL is not installed" >&2; exit 1; } ;;
	'') ;;	# reported when a download is actually needed
	*) echo "error: HPFOCUS_FETCH takes curl or wget" >&2; exit 2 ;;
esac

CACHE=${HPFOCUS_MEDIA_CACHE:-}
if [ -n "$CACHE" ]; then
	case "$CACHE" in /*) ;; *) CACHE=$OLDPWD/$CACHE ;; esac
	[ -d "$CACHE" ] || echo "note: HPFOCUS_MEDIA_CACHE=$CACHE does not exist yet; it is created for what is downloaded"
fi

# download URL PART: fetch URL into PART, resuming if PART exists. Status 0
# if the transfer finished; else REFUSED says why in words.
REFUSED=
download() {
	local url=$1 part=$2 rc=0 code= host
	host=$(host_of "$url")
	REFUSED=
	case "$url" in
	file://*)	# a local mirror named in a private MEDIA.txt
		cp "${url#file://}" "$part" 2>/dev/null && return 0
		REFUSED="cannot read ${url#file://}"; return 1 ;;
	esac
	if [ "$TOOL" = curl ]; then
		local quiet=-sS
		[ -t 2 ] && quiet=-#
		# -f: an HTTP error is an exit status, not an error page appended
		# to a partial file. -C -: resume from the size of PART.
		code=$(curl -f -L $quiet --connect-timeout 30 --retry 2 -A "$UA" \
			-C - -o "$part" -w '%{http_code}' "$url") || rc=$?
		case "$rc" in
		0) return 0 ;;
		6) REFUSED="cannot resolve $host: is this machine online?" ;;
		7) REFUSED="cannot connect to $host: the host is down, or this machine is offline" ;;
		28) REFUSED="$host timed out" ;;
		35|51|58|60|77) REFUSED="no secure connection to $host (curl exit $rc: its certificate, or this machine's CA store)" ;;
		22)
			case "$code" in
			403) REFUSED="$host refused the request (HTTP 403). bitsavers.org does that to User-Agents it does not like; this script sent '$UA'" ;;
			404|410) REFUSED="$host no longer has the file at that URL (HTTP $code)" ;;
			416) REFUSED="resume"; rm -f "$part" ;;
			429) REFUSED="$host says too many requests (HTTP 429): wait a while before trying again" ;;
			*) REFUSED="$host answered HTTP $code" ;;
			esac ;;
		33|36) REFUSED="resume"; rm -f "$part" ;;
		18) REFUSED="the connection to $host broke off; what arrived is kept, and the next run resumes it" ;;
		*) REFUSED="curl failed (exit $rc)" ;;
		esac
		return 1
	fi
	# wget: -c resumes; -nv keeps its own error lines.
	local show=-nv
	[ -t 2 ] && wget --help 2>/dev/null | grep -q -- --show-progress && show="-q --show-progress"
	# shellcheck disable=SC2086
	wget $show -c --timeout=30 --tries=3 -U "$UA" -O "$part" "$url" || rc=$?
	case "$rc" in
	0) return 0 ;;
	4) REFUSED="cannot reach $host: the host is down, or this machine is offline" ;;
	5) REFUSED="no secure connection to $host (its certificate, or this machine's CA store)" ;;
	8) REFUSED="$host refused the request or no longer has the file (wget prints the HTTP status above; 403 is what bitsavers.org answers a User-Agent it does not like, and this script sent '$UA')" ;;
	*) REFUSED="wget failed (exit $rc)" ;;
	esac
	# wget -O creates PART even when nothing arrived.
	[ -s "$part" ] || rm -f "$part"
	return 1
}

# The partial downloads are the only files this script leaves behind that
# MEDIA.txt does not list: PATH.part, kept across runs so that a broken
# transfer resumes, removed as soon as the file verifies or fails to.
fetched=0; present=0; failed=0; copied=0
FAILED_LIST=()
announced=0
announce() {
	[ "$announced" = 1 ] && return 0
	announced=1
	local n=0 b=0 k=0
	while [ "$k" -lt "$nf" ]; do
		if [ "${F_WANT[k]}" = 1 ] && [ ! -f "${F_PATH[k]}" ]; then n=$((n + 1)); b=$((b + F_SIZE[k])); fi
		k=$((k + 1))
	done
	echo "fetching $n file(s), $(mb "$b") MB, of Hewlett-Packard's software and manuals."
	echo "They are HP's, not this project's, and come from third-party archives"
	echo "(MEDIA.txt lists each file, its source and its SHA-256):"
	k=0
	local hosts=" " h u
	while [ "$k" -lt "$nf" ]; do
		if [ "${F_WANT[k]}" = 1 ] && [ ! -f "${F_PATH[k]}" ]; then
			u=${F_URLS[k]%% *}; h=$(host_of "$u")
			case "$hosts" in *" $h "*) ;; *) hosts="$hosts$h "; echo "  $h" ;; esac
		fi
		k=$((k + 1))
	done
	[ -n "$CACHE" ] && echo "  (first from the cache, $CACHE)"
	return 0
}

fetch_one() {
	local k=$1 p size sha part url tried=0 again
	p=${F_PATH[k]}; size=${F_SIZE[k]}; sha=${F_SHA[k]}; part=$p.part
	if [ -f "$p" ]; then
		if verify "$p" "$size" "$sha"; then
			present=$((present + 1))
			rm -f "$part"
			return 0
		fi
		echo "error: $p is here but is not the file MEDIA.txt lists:" >&2
		echo "       $WHY." >&2
		echo "       It is left as it is. Move it away (or delete it) and run this again." >&2
		failed=$((failed + 1)); FAILED_LIST+=("$p")
		return 1
	fi
	announce
	mkdir -p "$(dirname "$p")" || { failed=$((failed + 1)); FAILED_LIST+=("$p"); return 1; }

	# The cache first.
	if [ -n "$CACHE" ] && [ -f "$CACHE/$p" ]; then
		if verify "$CACHE/$p" "$size" "$sha"; then
			if cp "$CACHE/$p" "$part.cache" && mv -f "$part.cache" "$p"; then
				rm -f "$part"
				echo "  cached   $p"
				copied=$((copied + 1))
				return 0
			fi
			rm -f "$part.cache"
			echo "error: cannot copy $CACHE/$p to $p (disc full?)" >&2
			failed=$((failed + 1)); FAILED_LIST+=("$p")
			return 1
		fi
		echo "warning: $CACHE/$p is not the file MEDIA.txt lists ($WHY); downloading instead" >&2
	fi

	if [ -z "$TOOL" ]; then
		echo "error: neither curl nor wget is installed, and $p has to be downloaded." >&2
		echo "       Install one, or download ${F_URLS[k]%% *}" >&2
		echo "       to $p with a browser and run tools/fetch-media.sh --check." >&2
		failed=$((failed + 1)); FAILED_LIST+=("$p")
		return 1
	fi

	# A part that is already as long as the file (the last run was stopped
	# between the download and the check) is not resumed, only checked; a
	# longer one is not this file.
	if [ -f "$part" ]; then
		local have_bytes
		have_bytes=$(size_of "$part")
		if [ "$have_bytes" -gt "$size" ]; then
			rm -f "$part"
		elif [ "$have_bytes" -gt 0 ] && [ "$have_bytes" -lt "$size" ]; then
			echo "  resuming $p at $have_bytes of $size bytes"
		fi
	fi

	for url in ${F_URLS[k]}; do
		tried=$((tried + 1))
		again=1
		while [ "$again" = 1 ]; do
			again=0
			if [ -f "$part" ] && [ "$(size_of "$part")" = "$size" ]; then
				:	# complete already: straight to the check
			else
				echo "  fetching $p ($(mb "$size") MB) from $(host_of "$url")"
				if ! download "$url" "$part"; then
					if [ "$REFUSED" = resume ]; then
						# The server would not resume; PART is gone, start over.
						again=1
						continue
					fi
					echo "  $(host_of "$url"): $REFUSED" >&2
					break
				fi
			fi
			if verify "$part" "$size" "$sha"; then
				mv -f "$part" "$p" || { rm -f "$part"; break; }
				echo "  verified $p"
				fetched=$((fetched + 1))
				if [ -n "$CACHE" ] && [ ! -f "$CACHE/$p" ]; then
					mkdir -p "$(dirname "$CACHE/$p")" 2>/dev/null \
						&& cp "$p" "$CACHE/$p.part" 2>/dev/null \
						&& mv -f "$CACHE/$p.part" "$CACHE/$p" \
						|| { rm -f "$CACHE/$p.part"; echo "note: could not save $p to the cache" >&2; }
				fi
				return 0
			fi
			# Never left in place, and never resumed from: the next URL, or
			# the next run, starts this file from nothing.
			rm -f "$part"
			echo "error: what $(host_of "$url") sent for $p is NOT the file MEDIA.txt lists:" >&2
			echo "       $WHY." >&2
			echo "       It has been deleted. URL: $url" >&2
		done
	done
	failed=$((failed + 1)); FAILED_LIST+=("$p")
	echo "error: could not fetch $p from any of its $tried source(s)." >&2
	return 1
}

i=0
while [ "$i" -lt "$nf" ]; do
	[ "${F_WANT[i]}" = 1 ] && fetch_one "$i"
	i=$((i + 1))
done

# ------------------------------------------------------- text of the manuals
made=0; made_warn=0; no_pdftotext=0; no_pdf=0
i=0
while [ "$i" -lt "$nt" ]; do
	if [ "${T_WANT[i]}" = 1 ] && [ ! -f "${T_PATH[i]}" ]; then
		p=${T_PATH[i]}; pdf=${T_PDF[i]}
		if [ ! -f "$pdf" ]; then
			no_pdf=$((no_pdf + 1))
		elif ! have pdftotext; then
			no_pdftotext=$((no_pdftotext + 1))
		else
			[ "$made" = 0 ] && [ "$made_warn" = 0 ] && echo "extracting the manuals' text into docs/notes (pdftotext)..."
			mkdir -p "$(dirname "$p")"
			rm -f "$p.part"
			case "${T_MODE[i]}" in
				layout) pdftotext -layout "$pdf" "$p.part" 2>/dev/null ;;
				*) pdftotext "$pdf" "$p.part" 2>/dev/null ;;
			esac
			if [ ! -s "$p.part" ]; then
				rm -f "$p.part"
				echo "warning: pdftotext wrote nothing for $pdf; $p not made" >&2
				made_warn=$((made_warn + 1))
			else
				if verify "$p.part" "${T_SIZE[i]}" "${T_SHA[i]}"; then
					made=$((made + 1))
				else
					made_warn=$((made_warn + 1))
					echo "warning: $p differs from the project's copy ($WHY)" >&2
				fi
				mv -f "$p.part" "$p"
			fi
		fi
	fi
	i=$((i + 1))
done
if [ "$made_warn" -gt 0 ]; then
	echo "warning: $made_warn text file(s) are not byte for byte what poppler $NOTES_POPPLER wrote." >&2
	echo "         This is $(pdftotext -v 2>&1 | head -1): another version may space a page" >&2
	echo "         differently. The text is usable; line numbers cited from it may be off." >&2
fi
if [ "$no_pdftotext" -gt 0 ]; then
	echo "note: pdftotext is not installed, so $no_pdftotext text file(s) in docs/notes were not made." >&2
	echo "      Install poppler-utils (Debian/Ubuntu, Fedora), poppler (Arch, Homebrew) and" >&2
	echo "      run tools/fetch-media.sh --manuals again; the PDFs are not downloaded twice." >&2
fi

# ----------------------------------------------------------------- summary
total=$((present + fetched + copied + failed))
if [ "$total" -gt 0 ]; then
	echo "media: $present already here, $fetched downloaded, $copied from the cache$( [ "$made" -gt 0 ] || [ "$made_warn" -gt 0 ] && echo ", $((made + made_warn)) text file(s) made" )$( [ "$failed" -gt 0 ] && echo ", $failed FAILED" )"
fi
if [ "$failed" -gt 0 ]; then
	{
		echo
		echo "Not fetched:"
		for p in "${FAILED_LIST[@]}"; do echo "  $p"; done
		echo "Run tools/fetch-media.sh again when the network is back: finished files are"
		echo "kept and an unfinished one resumes. Or fetch a file yourself -- its URLs"
		echo "are in MEDIA.txt (tools/fetch-media.sh --list) -- put it at the path"
		echo "above, and run tools/fetch-media.sh --check."
	} >&2
	exit 1
fi
exit 0
