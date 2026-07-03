	.text
	mov (%rax), %rbx
	mov (%r13), %rax
	mov (%r12), %rax
	mov (%rsp), %rax
	mov 8(%rbp), %rax
	mov 0x1000(%rax), %rbx
	mov (%rax,%rcx,4), %rdx
	mov 8(%rax,%rcx,4), %rdx
	mov (,%rcx,8), %rdx
	mov (%r8,%r9,4), %r10
	movq $0, (%rdi)
	movl $1, 4(%rsp)
	incq (%rax)
	.data
	.globl table
table:
	.quad table
	.long 0x11223344
	.byte 1, 2, 3, 4
	.asciz "hello"
