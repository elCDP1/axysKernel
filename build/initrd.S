.section .rodata
.balign 16
.balign 16
initrd_start_0: .incbin "build/user/init.elf"
initrd_end_0:
initrd_path_0: .asciz "/sbin/init"
.balign 16
initrd_start_1: .incbin "build/user/hello.elf"
initrd_end_1:
initrd_path_1: .asciz "/bin/hello"
.balign 16
initrd_start_2: .incbin "build/user/crash.elf"
initrd_end_2:
initrd_path_2: .asciz "/bin/crash"
.balign 16
initrd_start_3: .incbin "build/user/heap.elf"
initrd_end_3:
initrd_path_3: .asciz "/bin/heap"
.balign 16
initrd_start_4: .incbin "build/user/fileio.elf"
initrd_end_4:
initrd_path_4: .asciz "/bin/fileio"
.balign 16
initrd_start_5: .incbin "build/user/perms.elf"
initrd_end_5:
initrd_path_5: .asciz "/bin/perms"
.balign 16
initrd_start_6: .incbin "build/user/probe.elf"
initrd_end_6:
initrd_path_6: .asciz "/bin/probe"
.balign 8
.global axys_initrd_table
axys_initrd_table:
  .quad initrd_path_0, initrd_start_0, initrd_end_0
  .quad initrd_path_1, initrd_start_1, initrd_end_1
  .quad initrd_path_2, initrd_start_2, initrd_end_2
  .quad initrd_path_3, initrd_start_3, initrd_end_3
  .quad initrd_path_4, initrd_start_4, initrd_end_4
  .quad initrd_path_5, initrd_start_5, initrd_end_5
  .quad initrd_path_6, initrd_start_6, initrd_end_6
  .quad 0, 0, 0
.section .note.GNU-stack,"",@progbits
