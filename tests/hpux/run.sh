#	Build and run each check with HP's cc, as and ld. Every program
#	ends with "<name> N passed M failed"; the runner waits for the
#	last line, which only says ok when every program reported 0.
ok=yes
for t in "cputest cpu.c carry.s" "dwordtest dword.c ldst.s"
do
	set $t
	n=$1
	shift
	if cc -o $n $* > $n.cc 2>&1
	then
		./$n > $n.out 2>&1
		cat $n.out
		grep " 0 failed" $n.out > /dev/null || ok=no
	else
		cat $n.cc
		ok=no
	fi
done
echo "hpux cc tests: $ok"
