# Context switch
#
#   void swtch(struct context **old, struct context *new);

# Each timer interrupt will force the current process to yield the processor and perform a context
# switch back into the scheduler so it can run the next available process

# Current stack layout
# 8(%esp) - struct context *new
# 4(%esp) - struct context **old
# 0(%esp) - return address %eip
.globl swtch
swtch:
  movl 4(%esp), %eax # save 'old' into caller-saved %eax
  movl 8(%esp), %edx # save 'new' into caller-saved %ecx (%edx is last caller-saved register)

  # save context at the top of the stack
  # order must match layout of 'struct context'
  # %eip is already saved
  pushl %ebp
  pushl %ebx
  pushl %esi
  pushl %edi

  # save pointer to this old struct context
  movl %esp, (%eax) # *old = %esp (dereference using ())
  # switch stacks, new context is at the top
  movl %edx, %esp # %esp = new

  # Restore context
  popl %edi
  popl %esi
  popl %ebx
  popl %ebp
  ret # pop %eip and resume execution
