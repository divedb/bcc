	.text
	.globl strlen2
strlen2:
	xorl %eax, %eax
.Ltop:
	cmpb $0, (%rdi,%rax)
	je .Ldone
	incq %rax
	cmpq $100, %rax
	jl .Ltop
	jmp .Ltop
.Ldone:
	ret
	.globl branchy
branchy:
	testl %edi, %edi
	je .La
	jl .Lb
	jg .Lc
.La:
	movl $1, %eax
	ret
.Lb:
	movl $2, %eax
	ret
.Lc:
	movl $3, %eax
	ret
