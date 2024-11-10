// x86 trap and interrupt constants.
// A number of reserved interrupts require flags to be set to fire
// Others like bounds range and device not available can only occur on specific instructions, and are
// generally unseen

// 0-31 processor-defined by default:
#define T_DIVIDE         0      // divide error
#define T_DEBUG          1      // debug exception
#define T_NMI            2      // non-maskable interrupt
#define T_BRKPT          3      // breakpoint
#define T_OFLOW          4      // overflow
#define T_BOUND          5      // bounds check
#define T_ILLOP          6      // illegal opcode
#define T_DEVICE         7      // device not available
#define T_DBLFLT         8      // double fault, very bad
                                // system is not in a state that can be recovered from
                                // commonly because the CPU could not call the GP fault handler
                                // can be triggered by hardware conditions too
                                // consider it as a last change to clean up and save state
                                // if not handled, the CPu will 'triple fault', meaning a reset
// #define T_COPROC      9      // reserved (not used since 486)
#define T_TSS           10      // invalid task switch segment
#define T_SEGNP         11      // segment not present
#define T_STACK         12      // stack segment exception
#define T_GPFLT         13      // general protection fault, generally from an instruction dealing with
                                // segment registers in some way, e.g. iret, lidt/ltr, pushes error code
                                // also when trying to execute a privileged instruction
#define T_PGFLT         14      // page fault (VA translation), pushes error code
                                // error code describes what was attempted, not why it failed
                                // %cr2 contains the VA being translated
                                // common bits in error code:
                                // 0 - present - (all page table entries), thus protection violation
                                // 1 - write - read/write attempt
                                // 2 - user - CPU was in user mode (CPL=3)
                                // 3 - reserved bit set - in a page table entry, best to walk page tables
                                // 4 - instruction fetch - NX enabled in EFER, and attempt from NX page
// #define T_RES        15      // reserved
#define T_FPERR         16      // floating point error, requires CR0.NE
#define T_ALIGN         17      // aligment check
#define T_MCHK          18      // machine check
#define T_SIMDERR       19      // SIMD floating point error, requires enabling SSE

// other interrupts that push an error code (excluding always-zero ones) use the following format to
// indicate which selector caused the fault:
// 0 - external - hardware interrupt
// 1 - IDT - error code refers to GDT or LDT vs IDT
// 2 - table index - error code refers to GDT vs LDT
// 31:3 - index - index into table the error code refers to

// before the current layout of the IDT existed there were a pair of devices called the PICs that handled
// interrupts for the CPU
// Can cause 8 interrupts each, by default 0-7 and 8-15
// We offset these vectors by 32 to avoid overlap with reserved vectors

// note - must hlt in a loop so returns from interrupts don't continue execution

// These are arbitrarily chosen, but with care not to overlap
// processor defined exceptions or interrupt vectors.
#define T_SYSCALL       64      // system call
#define T_DEFAULT      500      // catchall

// IRQ = Interrupt ReQuest, for interrupts from other hardware devices
#define T_IRQ0          32      // IRQ 0 corresponds to int T_IRQ

#define IRQ_TIMER        0
#define IRQ_KBD          1
#define IRQ_COM1         4
#define IRQ_IDE         14
#define IRQ_ERROR       19
#define IRQ_SPURIOUS    31 // 0xFF interrupt number for spurious interrupts

