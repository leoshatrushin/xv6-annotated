#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "x86.h"
#include "traps.h"
#include "spinlock.h"

// Two jobs
// 1) put trap handler functions in 'vectors' into an IDT
// 2) figure out what to do with each interrupt type

// Interrupt descriptor table (shared by all CPUs).
struct gatedesc idt[256];
extern uint vectors[];  // in vectors.S: array of 256 entry pointers
struct spinlock tickslock;
uint ticks; // number of timer interrupts so far (rough timer)

// loads all assembly trap handler functions in 'vectors' into the IDT
void
tvinit(void)
{
  int i;

  for(i = 0; i < 256; i++)
    SETGATE(idt[i], 0, SEG_KCODE<<3, vectors[i], 0);
  SETGATE(idt[T_SYSCALL], 1, SEG_KCODE<<3, vectors[T_SYSCALL], DPL_USER);

  initlock(&tickslock, "time");
}

// tell processor where to find the IDT
// devices interrupt through vectors, set up here
// the only difference between vector 32 (timer) and vector 64 (syscalls) is 32 is an interrupt gate while
// 64 is a trap gate
// Interrupt gates clear IF
// From here on until 'trap', interrupts follow the same code path as system calls and exceptions, building
// up a trap frame
// 'trap' for a timer interrupt does 2 things - increment ticks variable, and call wakeup, which may cause
// the interrupt to return in a diffent process
void
idtinit(void)
{
  lidt(idt, sizeof(idt));
}

//PAGEBREAK: 41
// called by alltraps, switches based on trap number pushed on stack
void
trap(struct trapframe *tf)
{
  if(tf->trapno == T_SYSCALL){
    if(myproc()->killed) // process done or caused an exception
      exit();
    myproc()->tf = tf;
    syscall();
    if(myproc()->killed)
      exit();
    return;
  }

  switch(tf->trapno){
  case T_IRQ0 + IRQ_TIMER:
    if(cpuid() == 0){
      acquire(&tickslock);
      ticks++;
      wakeup(&ticks); // checks if any processes went to sleep until the next tick; switch to running any process it finds
      release(&tickslock);
    }
    lapiceoi();
    break;
  case T_IRQ0 + IRQ_IDE: // disk interrupt
    ideintr();
    lapiceoi();
    break;
  case T_IRQ0 + IRQ_IDE+1:
    // Bochs generates spurious IDE1 interrupts.
    break;
  case T_IRQ0 + IRQ_KBD: // keyboard interrupt
    kbdintr();
    lapiceoi();
    break;
  case T_IRQ0 + IRQ_COM1: // serial port interrupt
    uartintr();
    lapiceoi();
    break;
  // devices occasionally generate spurious interrupts due to hardware malfunctions
  case T_IRQ0 + 7:
  case T_IRQ0 + IRQ_SPURIOUS:
    cprintf("cpu%d: spurious interrupt at %x:%x\n",
            cpuid(), tf->cs, tf->eip);
    lapiceoi();
    break;

  //PAGEBREAK: 13
  default: // rest of traps are software exceptions
    if(myproc() == 0 || (tf->cs&3) == 0){
      // In kernel, it must be our mistake.
      cprintf("unexpected trap %d from cpu %d eip %x (cr2=0x%x)\n",
              tf->trapno, cpuid(), tf->eip, rcr2());
      panic("trap");
    }
    // In user space, assume process misbehaved.
    cprintf("pid %d %s: trap %d err %d on cpu %d "
            "eip 0x%x addr 0x%x--kill proc\n",
            myproc()->pid, myproc()->name, tf->trapno,
            tf->err, cpuid(), tf->eip, rcr2());
    // don't kill immediately becuase it might be executing kernel code right now - e.g. syscalls allow
    // other interrupts and exceptions to occur while they're being handled, thus killing it might corrupt
    // whatever it's doing
    myproc()->killed = 1;
  }

  // Force process exit if it has been killed and is in user space.
  // (If it is still executing in the kernel, let it keep running
  // until it gets to the regular system call return or it generates a trap.)
  if(myproc() && myproc()->killed && (tf->cs&3) == DPL_USER)
    exit();

  // Force process to give up CPU on clock tick.
  // If interrupts were on while locks held, would need to check nlock.
  if(myproc() && myproc()->state == RUNNING &&
     tf->trapno == T_IRQ0+IRQ_TIMER)
    yield();

  // Check if the process has been killed since we yielded
  if(myproc() && myproc()->killed && (tf->cs&3) == DPL_USER)
    exit();
  // returns into 'trapret', which will go back to user mode
}

// Summary
// The xv6 kernel has 4 main functions
// 1) finish the boot process - set up virtual memory and hardware devices (keyboard, serial port, console, disk)
// 2) virtualize resources via virtual memory and processes to isolate processes
// 3) schedule processes to run
// 4) interface between user processes and hardware devices
//   - primary mechanism is traps
//   - x86 'int' instruction finds IDT and looks up entry for that trap number (which calls alltraps() which calls trap() which switches on the trap number)
//   - interface presents hardware device in a simplified way
// system calls take care of 2) and 4)
