	.text
	.globl compute
	.type compute, @function
compute:
	movq %rax, %rbx
	addq $10, %rbx
	subl $1, %ecx
	imulq %rbx, %rax
	andq $0xff, %rax
	xorl %edx, %edx
	negq %rbx
	shlq $4, %rax
	movzbl %al, %eax
	movslq %eax, %rax
	leaq 16(%rsp), %rdi
	cmpq $0, %rax
	testq $0x10, %rax
	ret
