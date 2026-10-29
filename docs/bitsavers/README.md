# HP's manuals for the Series 500 — fetched, not carried

This directory is empty in the repository. The documents the project's
notes and source comments cite — the FOCUS Instruction Set and I/O ERS, the
520 and 530/540 service manuals and CE handbooks, the CIO card references,
the CS/80 instruction set manual, and five CS/80 Describe captures — are
Hewlett-Packard's, and are hosted by [bitsavers.org](https://bitsavers.org/pdf/hp/).

    tools/fetch-media.sh --manuals     # 40 files, 230 MB, into docs/bitsavers and docs/hpjournal

fetches them here under the names the citations use, checks each against
the SHA-256 recorded in [MEDIA.txt](../../MEDIA.txt), and extracts their
text into [docs/notes](../notes/) with `pdftotext`. `tools/fetch-media.sh
--list` shows every file, its size and its source.
