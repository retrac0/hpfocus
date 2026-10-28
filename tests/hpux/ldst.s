#	Doubleword loads and stores through a data segment pointer,
#	each path the instruction set has: LDD/STD and LDDS through
#	an indirect local, and LDSI/SDSI off the stack. The pointer
#	is the first argument (Q-31); hi and lo follow.
#	*hi(p) and *lo(p) return one word of the doubleword at p.
	ddata
	idata
	text
	ent	_lddhi
_lddhi:	proc
	ldx	~L.1
	pshn
	ldd	Q-31,i
	del
	sts	Q-20,i
	exit	0
~L.1:	word	0
	ent	_lddlo
_lddlo:	proc
	ldx	~L.1
	pshn
	ldd	Q-31,i
	sts	Q-20,i
	exit	0
~L.1:	word	0
	ent	_lddshi
_lddshi:	proc
	ldx	~L.1
	pshn
	ldds	Q-31,i
	del
	sts	Q-20,i
	exit	0
~L.1:	word	0
	ent	_lddslo
_lddslo:	proc
	ldx	~L.1
	pshn
	ldds	Q-31,i
	sts	Q-20,i
	exit	0
~L.1:	word	0
	ent	_ldsihi
_ldsihi:	proc
	ldx	~L.1
	pshn
	lds	Q-31
	ldsi
	del
	sts	Q-20,i
	exit	0
~L.1:	word	0
	ent	_ldsilo
_ldsilo:	proc
	ldx	~L.1
	pshn
	lds	Q-31
	ldsi
	sts	Q-20,i
	exit	0
~L.1:	word	0
	ent	_stdw
_stdw:	proc
	ldx	~L.1
	pshn
	lds	Q-35
	lds	Q-39
	std	Q-31,i
	exit	0
~L.1:	word	0
	ent	_sdsiw
_sdsiw:	proc
	ldx	~L.1
	pshn
	lds	Q-31
	lds	Q-35
	lds	Q-39
	sdsi
	exit	0
~L.1:	word	0
	idata
	text
