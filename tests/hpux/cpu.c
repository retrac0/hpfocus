/*
 * CPU checks, compiled and run under HP-UX 5.30 by HP's own cc, as and
 * ld. The routines are in carry.s; the expected values are the ERS's.
 * Prints each failure, then one summary line the test runner matches.
 */
long daddhi(), dsubhi(), laddc(), lsubc(), dsubz(), daddz();
long ddladhi(), ddlsbhi();

int pass, fail;

t(name, got, want)
char *name;
long got, want;
{
	if (got == want)
		pass++;
	else {
		fail++;
		printf("FAIL %s: got %lx want %lx\n", name, got, want);
	}
}

main()
{
	/* LADD,B adds Carry; LSUB,B subtracts 1-Carry (sheet 76) */
	t("ladd/add,c carry", daddhi(0L, -1L, 0L, 1L), 1L);
	t("ladd/add,c none", daddhi(0L, 1L, 0L, 1L), 0L);
	t("lsub/sub,c borrow", dsubhi(1L, 0L, 0L, 1L), 0L);
	t("lsub/sub,c none", dsubhi(1L, 5L, 0L, 3L), 1L);
	t("lsub/sub,c big", dsubhi(0L, 0L, 0L, 1L), -1L);

	/* Carry out: set on a carry, and on NO borrow */
	t("ladd carry", laddc(-1L, 1L), 1L);
	t("ladd no carry", laddc(1L, 1L), 0L);
	t("lsub no borrow", lsubc(5L, 3L), 1L);
	t("lsub borrow", lsubc(3L, 5L), 0L);
	t("lsub equal", lsubc(3L, 3L), 1L);

	/* With b set a zero result leaves CC alone (sheets 74-75) */
	t("sub,c zero keeps CCG", dsubz(1L, 5L, 1L, 3L), 0L);
	t("sub,c zero keeps CCE", dsubz(1L, 3L, 1L, 3L), 1L);
	t("add,c zero keeps CCG", daddz(0L, 1L, 0L, 1L), 0L);
	t("add,c zero keeps CCE", daddz(0L, 0L, 0L, 0L), 1L);

	/* The decimal pair chains the same way (sheet 93.8) */
	t("dlad/dlad,c carry", ddladhi(0L, 0x99999999L, 0L, 1L), 1L);
	t("dlsb/dlsb,c borrow", ddlsbhi(1L, 0L, 0L, 1L), 0L);

	printf("cputest %d passed %d failed\n", pass, fail);
}
