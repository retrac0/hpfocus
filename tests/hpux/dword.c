/*
 * Doubleword loads and stores through a pointer, every path the
 * instruction set has (ldst.s), checked word by word from C. The
 * pointer is a global data segment pointer -- malloc's memory, like
 * everything an ordinary program can name, is unpaged here.
 */
char *malloc();
long lddhi(), lddlo(), lddshi(), lddslo(), ldsihi(), ldsilo();

int pass, fail;

t(name, k, got, want)
char *name;
int k;
long got, want;
{
	if (got == want)
		pass++;
	else {
		fail++;
		printf("FAIL %s %d: got %lx want %lx\n", name, k, got, want);
	}
}

main()
{
	long *b, *w, hi, lo;
	int k;

	b = (long *) malloc(1024);
	for (k = 0; k < 8; k++) {
		w = b + 3 * k + 1;
		hi = 0x11111111L * (k + 1);
		lo = 0x22222222L + k;
		w[0] = hi;
		w[1] = lo;
		t("ldd hi", k, lddhi(w), hi);
		t("ldd lo", k, lddlo(w), lo);
		t("ldds hi", k, lddshi(w), hi);
		t("ldds lo", k, lddslo(w), lo);
		t("ldsi hi", k, ldsihi(w), hi);
		t("ldsi lo", k, ldsilo(w), lo);
		w[0] = w[1] = 0;
		stdw(w, hi, lo);
		t("std hi", k, w[0], hi);
		t("std lo", k, w[1], lo);
		w[0] = w[1] = 0;
		sdsiw(w, hi, lo);
		t("sdsi hi", k, w[0], hi);
		t("sdsi lo", k, w[1], lo);
	}
	printf("dwordtest %d passed %d failed\n", pass, fail);
}
