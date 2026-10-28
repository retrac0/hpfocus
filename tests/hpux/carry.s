#	Carry chains, written in HP's own assembler syntax and laid
#	out the way cc -S lays out a function. Arguments sit at
#	Q-31, Q-35, Q-39, Q-43 in order; the result goes through
#	Q-20,i. The ",c" suffix is the b bit of the add/sub group.
#
#	long fn(ah, al, bh, bl): a doubleword op, returning the high
#	word.  long fn(a, b): a single op, returning 1 if Carry is
#	set after it.  The *z forms return 1 if the chain leaves CCE.
	ddata
	idata
	text
	ent	_daddhi
_daddhi:	proc
	ldx	~L.1
	pshn
	lds	Q-35
	lds	Q-43
	ladd
	lds	Q-31
	lds	Q-39
	add,c
	sts	Q-20,i
	exit	0
~L.1:	word	0
	ent	_dsubhi
_dsubhi:	proc
	ldx	~L.1
	pshn
	lds	Q-35
	lds	Q-43
	lsub
	lds	Q-31
	lds	Q-39
	sub,c
	sts	Q-20,i
	exit	0
~L.1:	word	0
	ent	_laddc
_laddc:	proc
	ldx	~L.1
	pshn
	lds	Q-31
	lds	Q-35
	ladd
	bcy	~L.2
	zero
	sts	Q-20,i
	exit	0
~L.2:	ldis	1
	sts	Q-20,i
	exit	0
~L.1:	word	0
	ent	_lsubc
_lsubc:	proc
	ldx	~L.1
	pshn
	lds	Q-31
	lds	Q-35
	lsub
	bcy	~L.2
	zero
	sts	Q-20,i
	exit	0
~L.2:	ldis	1
	sts	Q-20,i
	exit	0
~L.1:	word	0
#	The high pair goes on first so that nothing between the two
#	limbs touches the condition code: DEL drops the low result.
	ent	_dsubz
_dsubz:	proc
	ldx	~L.1
	pshn
	lds	Q-31
	lds	Q-39
	lds	Q-35
	lds	Q-43
	lsub
	del
	sub,c
	beqs	~L.2
	zero
	sts	Q-20,i
	exit	0
~L.2:	ldis	1
	sts	Q-20,i
	exit	0
~L.1:	word	0
	ent	_daddz
_daddz:	proc
	ldx	~L.1
	pshn
	lds	Q-31
	lds	Q-39
	lds	Q-35
	lds	Q-43
	ladd
	del
	add,c
	beqs	~L.2
	zero
	sts	Q-20,i
	exit	0
~L.2:	ldis	1
	sts	Q-20,i
	exit	0
~L.1:	word	0
	ent	_ddladhi
_ddladhi:	proc
	ldx	~L.1
	pshn
	lds	Q-35
	lds	Q-43
	dlad
	lds	Q-31
	lds	Q-39
	dlad,c
	sts	Q-20,i
	exit	0
~L.1:	word	0
	ent	_ddlsbhi
_ddlsbhi:	proc
	ldx	~L.1
	pshn
	lds	Q-35
	lds	Q-43
	dlsb
	lds	Q-31
	lds	Q-39
	dlsb,c
	sts	Q-20,i
	exit	0
~L.1:	word	0
	idata
	text
