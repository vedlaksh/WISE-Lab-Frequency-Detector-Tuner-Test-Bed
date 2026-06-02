.section .text.start
.globl _start
_start:
  la sp, _stack_end
  call rust_main
1: j 1b

