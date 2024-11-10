// Per-CPU state
// At any point in time, a processor will be running one of
// - its own initialization routine (only once the kernel is setting up)
// - a user process (or any interrupts or system calls that come up)
// - a scheduler routine to run the next process
// Thus see 'started', 'proc' properties
// The scheduler isn't itself a process - it uses the 'kpgdir' page directory and has its own context - we
// store the context in 'scheduler' property
struct cpu {
  uchar apicid;                // Local APIC ID (local interrupt controller)
  struct context *scheduler;   // kernel context at the top of scheduler stack
  struct taskstate ts;         // Used by x86 to find stack for interrupt (TSS)
  struct segdesc gdt[NSEGS];   // x86 global descriptor table
  volatile uint started;       // Has the CPU started?
  int ncli;                    // Depth of pushcli nesting.
  int intena;                  // Were interrupts enabled before pushcli?
  struct proc *proc;           // The process running on this cpu or null
};

// mp.c
extern struct cpu cpus[NCPU];
extern int ncpu;

//PAGEBREAK: 17
// Saved registers for kernel context switches.
// Don't need to save all the segment registers (%cs, etc),
// because they are constant across kernel contexts.
// Don't need to save %eax, %ecx, %edx, because the
// x86 convention is that the caller has saved them.
// Contexts are stored at the bottom of the stack they
// describe; the stack pointer is the address of the context.
// The layout of the context matches the layout of the stack in swtch.S
// at the "Switch stacks" comment. Switch doesn't save eip explicitly,
// but it is on the stack and allocproc() manipulates it.
// Process context is saved by pushing this on the stack, then the stack pointer is effectively a
// pointer to the context
// Can find full list of registers on OSDev wiki - https://wiki.osdev.org/CPU_Registers_x86
// There are general-purpose registers, %eip, segment registers, a flags register, control registers, and
// the GDT and IDT registers (xv6 (x86?) doesn't use the debug, test or LDT registers)
// flags, control, GDT/IDT registers shouldn't change between processes, so we don't need to save them
// We made the segment registers identity maps, same for all processes
// There are separate segments for user and kernel mode, but context switches will always occur in kernel
// mode, so the segment registers shouldn't change
// Definitely save %eip, as it points to where we should resume
// Only general-purpose registers remain - %ebp, %esp, %eax, %ebx, %ecx, %edx, %esi, %edi
// %esp tells us where to find the context, which must mean we'll already have it through some other means
// %eax, %ecx, %edx are caller-saved
struct context {
  uint edi;
  uint esi;
  uint ebx;
  uint ebp;
  uint eip;
};

enum procstate { UNUSED, EMBRYO, SLEEPING, RUNNABLE, RUNNING, ZOMBIE };
// UNUSED - process doesn't exist
// ZOMBIE - killing a process requires cleanup before it goes back to UNUSED
// EMBRYO - setup before RUNNABLE
// SLEEPING - blocked waiting for somethign (e.g. I/O)

// Per-process state
struct proc {
  uint sz;                     // Size of process memory (bytes)
  pde_t* pgdir;                // Page table
  char *kstack;                // Bottom of kernel stack for this process
  enum procstate state;        // Process state
  int pid;                     // Process ID
  struct proc *parent;         // Parent process
  struct trapframe *tf;        // Trap frame for interrupts or current syscall
  struct context *context;     // Process context at the top of its stack
  void *chan;                  // If non-zero, sleeping on chan
  int killed;                  // If non-zero, have been killed/should be killed soon
  struct file *ofile[NOFILE];  // Open files
  struct inode *cwd;           // Current directory
  char name[16];               // Process name (debugging)
};

// Process memory is laid out contiguously, low addresses first:
//   text
//   original data and bss
//   fixed-size stack
//   expandable heap
