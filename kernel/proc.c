#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "x86.h"
#include "proc.h"
#include "spinlock.h"

// global process table
struct {
  struct spinlock lock;
  struct proc proc[NPROC];
} ptable;

// first process - so other files can set it up
static struct proc *initproc;

int nextpid = 1;
extern void forkret(void);
extern void trapret(void);

static void wakeup1(void *chan);

void
pinit(void)
{
  initlock(&ptable.lock, "ptable");
}

// DANGER - Must be called with interrupts disabled
// Want ID guaranteed to start from 0 - so not local interrupt controller ID
int
cpuid() {
  return mycpu()-cpus;
}

// Must be called with interrupts disabled to avoid the caller being
// rescheduled on another CPU between reading lapicid and running through the loop.
// Normally would use pushcli() or popcli(), but they call this function - infinite recursion
struct cpu*
mycpu(void)
{
  int apicid, i;
  
  if(readeflags()&FL_IF)
    panic("mycpu called with interrupts enabled\n");
  
  apicid = lapicid();
  // APIC IDs are not guaranteed to be contiguous. Maybe we should have
  // a reverse map, or reserve a register to store &cpus[i].
  for (i = 0; i < ncpu; ++i) {
    if (cpus[i].apicid == apicid)
      return &cpus[i];
  }
  panic("unknown apicid\n");
}

// Disable interrupts so that we are not rescheduled
// while reading proc from the cpu structure
struct proc*
myproc(void) {
  struct cpu *c;
  struct proc *p;
  pushcli();
  c = mycpu();
  p = c->proc;
  popcli();
  return p;
}

// Important thing is how xv6 creates new processes and sets them up to start running
// Basically, it uses some stack and function call trickery to make the scheduler start running a new
// process with the code in forkret(), then trapret(), before switching context into user mode

//PAGEBREAK: 32
// Look in the process table for an UNUSED proc.
// If found, change state to EMBRYO and initialize
// state required to run in the kernel.
// Otherwise return 0.
static struct proc*
allocproc(void)
{
  struct proc *p;
  char *sp;

  acquire(&ptable.lock);

  // find UNUSED slot in global process table
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == UNUSED)
      goto found;

  // no slot found, return null
  release(&ptable.lock);
  return 0;

found:
  p->state = EMBRYO;
  p->pid = nextpid++;

  release(&ptable.lock);

  // Allocate page for process's kernel thread to use as a stack
  if((p->kstack = kalloc()) == 0){
    p->state = UNUSED;
    return 0;
  }

  // We won't set up pgdir yet - that'll happen in fork()
  // But we do need to set up the process so it'll start executing code somewhere
  // It'll start in kernel mode, then context switch back into user mode and start running its code
  // xv6 sets up every new process to start off by "returning" from a non-existent syscall
  // Thus the context switch mechanism can be reused for new processes too
  // New processes are created via fork(), so we'll return into a function called forkret()
  // forkret() has to return into trapret(), which closes out a trap by restoring saved registers and
  // switching into user mode
  // Challenge - "return" into a function that never called us
  // x86 call pushes arguments, return address, and %ebp
  // When the scheduler first runs the new process, it'll check its context via p->context to get its
  // register contents, including %eip
  // So if we want to start executing code in forkret(), the %eip field of the context should be forkret()
  // Then can trick into thinking the previous caller was trapret() by setting up arguments and a return
  // address in the stack
  // Start by getting a pointer to the bottom of the stack
  sp = p->kstack + KSTACKSIZE;

  // Now we should push any arguments for trapret() on the stack (it takes a struct trapframe arg)
  // So we leave room for a trap frame and make the process point to it with p->tf.
  sp -= sizeof *p->tf;
  p->tf = (struct trapframe*)sp;

  // Add "return address" to beginning of trapret() after that
  // Set up new context to start executing at forkret,
  // which returns to trapret.
  sp -= 4;
  *(uint*)sp = (uint)trapret;

  // Lastly, save some space for the process's context on the stack and point p->context to it
  sp -= sizeof *p->context;
  p->context = (struct context*)sp;
  // 0 it, except for the eip field, which will point to the beginning of forkret
  memset(p->context, 0, sizeof *p->context);
  p->context->eip = (uint)forkret;

  return p;
}

//PAGEBREAK: 32
// Set up first user process.
void
userinit(void)
{
  struct proc *p;
  // see https://pdos.csail.mit.edu/6.828/2008/
  extern char _binary_initcode_start[], _binary_initcode_size[];

  p = allocproc();
  
  initproc = p;
  if((p->pgdir = setupkvm()) == 0)
    panic("userinit: out of memory?");
  inituvm(p->pgdir, _binary_initcode_start, (int)_binary_initcode_size);
  p->sz = PGSIZE;
  memset(p->tf, 0, sizeof(*p->tf));
  p->tf->cs = (SEG_UCODE << 3) | DPL_USER;
  p->tf->ds = (SEG_UDATA << 3) | DPL_USER;
  p->tf->es = p->tf->ds;
  p->tf->ss = p->tf->ds;
  p->tf->eflags = FL_IF;
  p->tf->esp = PGSIZE;
  p->tf->eip = 0;  // beginning of initcode.S

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  // this assignment to p->state lets other cores
  // run this process. the acquire forces the above
  // writes to be visible, and the lock is also needed
  // because the assignment might not be atomic.
  acquire(&ptable.lock);

  p->state = RUNNABLE;

  release(&ptable.lock);
}

// Grow current process's memory (address space) by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint sz;
  struct proc *curproc = myproc();

  // get current size
  sz = curproc->sz;
  // grow or shrink process
  if(n > 0){
    if((sz = allocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  } else if(n < 0){
    if((sz = deallocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  }
  curproc->sz = sz;
  // update page directory and TSS
  switchuvm(curproc);
  return 0;
}

// Create a new process copying p as the parent.
// Sets up stack to return as if from system call.
// Caller must set state of returned proc to RUNNABLE.
// unlike other syscalls, fork() is used almost exclusively by the user code as a syscall - the kernel never
// calls it
int
fork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *curproc = myproc();

  // Allocate process - create slot in process table for child and setup its stack so it'll return into
  // forkret(), then trapret(), before context switching into user mode, and sets child state EMBRYO
  if((np = allocproc()) == 0){
    return -1;
  }

  // Copy process state from proc.
  if((np->pgdir = copyuvm(curproc->pgdir, curproc->sz)) == 0){ // copy page directory
    kfree(np->kstack); // fail - free stack allocproc() created and set child state UNUSED
    np->kstack = 0;
    np->state = UNUSED;
    return -1;
  }
  // copy size and trap frame (ensures child starts executing after trapret() with same register contents)
  np->sz = curproc->sz;
  np->parent = curproc; // set parent
  *np->tf = *curproc->tf;

  // Clear %eax so that fork returns 0 in the child.
  // this register will be restored from the trap frame before switching into user mode
  np->tf->eax = 0;

  // copy open files and cwd
  for(i = 0; i < NOFILE; i++)
    if(curproc->ofile[i])
      np->ofile[i] = filedup(curproc->ofile[i]);
  np->cwd = idup(curproc->cwd);

  // copy parent process name
  // like strncpy(), but guaranteed to nul-terminate
  // fairly common practice to write your own safe wrappers for some C stdlib functions, especially ones
  // in string.h, which are so often error-prone and dangerous
  safestrcpy(np->name, curproc->name, sizeof(curproc->name));

  pid = np->pid;

  acquire(&ptable.lock);

  np->state = RUNNABLE;

  release(&ptable.lock);

  return pid; // for parent
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait() to find out it exited.
void
exit(void)
{
  struct proc *curproc = myproc();
  struct proc *p;
  int fd;

  if(curproc == initproc)
    panic("init exiting");

  // Close all open files.
  for(fd = 0; fd < NOFILE; fd++){
    if(curproc->ofile[fd]){
      fileclose(curproc->ofile[fd]);
      curproc->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(curproc->cwd);
  end_op();
  curproc->cwd = 0;

  acquire(&ptable.lock);

  // Parent might be sleeping in wait().
  wakeup1(curproc->parent);

  // Pass abandoned children to init.
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->parent == curproc){
      p->parent = initproc;
      if(p->state == ZOMBIE)
        wakeup1(initproc);
    }
  }

  // Jump into the scheduler, never to return.
  curproc->state = ZOMBIE;
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait(void)
{
  struct proc *p;
  int havekids, pid;
  struct proc *curproc = myproc();
  
  acquire(&ptable.lock);
  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->parent != curproc)
        continue;
      havekids = 1;
      if(p->state == ZOMBIE){
        // Found one.
        pid = p->pid;
        kfree(p->kstack);
        p->kstack = 0;
        freevm(p->pgdir);
        p->pid = 0;
        p->parent = 0;
        p->name[0] = 0;
        p->killed = 0;
        p->state = UNUSED;
        release(&ptable.lock);
        return pid;
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || curproc->killed){
      release(&ptable.lock);
      return -1;
    }

    // Wait for children to exit.  (See wakeup1 call in proc_exit.)
    sleep(curproc, &ptable.lock);  //DOC: wait-sleep
  }
}

//PAGEBREAK: 42
// Per-CPU process scheduler.
// Each CPU's mpmain() setup ends with calling scheduler().
// Scheduler never returns.  It loops, doing:
//  - choose a RUNNABLE process from the process table to run
//  - swtch to that process to resume it
//  - eventually process swtches back to the scheduler.
// Interrupts were disabled in the bootloader, in xv6 the scheduler enables them for the first time
// Thus from this point on, with the exception of interrupts and syscalls, the kernel will only ever do
// one thing - schedule processes to run
void
scheduler(void)
{
  struct proc *p;
  struct cpu *c = mycpu(); // ok to call because interrupts are disabled
  c->proc = 0; // a CPU running the scheduler isn't running a process
  
  for(;;){
    // Enable interrupts on this processor. Chance to handle outstanding interrupts (e.g. disk interrupt to
    // unblock SLEEPING processes) while lock is released (to prevent deadlocks if an interrupt handler
    // needs to acquire the lock)
    sti();

    // Loop over process table looking for a RUNNABLE process to run.
    acquire(&ptable.lock); // acquiring a lock disables interrupts

    // scheduling algorithm
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->state != RUNNABLE)
        continue;

      // Switch to chosen process.  It is the process's job
      // to release ptable.lock and then reacquire it
      // before jumping back to us.
      // switch to the process pgdir
      // kernel code continues to be safe to execute because it uses addresses in the higher half, which are
      // the same for every page directory (setupkvm())
      c->proc = p;
      switchuvm(p);
      p->state = RUNNING;

      // pick up where process left off - in kernel mode, which handled a syscall, interrupt or exception
      // before calling the scheduler
      // process will still be holding the ptable.lock, this is the main reason for the existence of forkret()
      // DANGER - if you want to add a new syscall that will let go of the CPU, it must release the process
      // table lock at the point at which it starts executing after switching to it from the scheduler
      // Can't release before calling swtch() and reacquire after - think of locks as protecting some
      // invariant, which may be violated temporarily while you hold the lock - the process table protects
      // invariants related to the process's p->state and p->context fields
      // - CPU registers must hold process's register values
      // - RUNNABLE process must be able to run by any idle CPU's scheduler
      // - etc.
      // These don't hold true while executing in swtch() - problem if another CPU decides to run the
      // process before swtch() is done executing
      //
      // At some point, the process will be done running and will give up the CPU again
      // Before it switches back into the scheduler, it has to acquire the process table lock again
      // DANGER - make sure to acquire the process table lock if you add your own scheduling-related syscall
      swtch(&(c->scheduler), p->context);

      // Eventually process will swtch back
      switchkvm(); // switch back to kpgdir

      // Process is done running for now.
      // It should have changed its p->state before coming back.
      c->proc = 0;
    }
    release(&ptable.lock);

  }
}

// We say code that runs after switching away from the scheduler, this runs after switching to the scheduler
// Functions can't just call scheduler(), it probably left off last time halfway through the loop and
// should resume in the same place
// Should be called after acquiring the process table lock and without holding any other locks (lest we cause
// Enter scheduler.  Must hold only ptable.lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->ncli, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void)
{
  int intena;
  struct proc *p = myproc();

  if(!holding(&ptable.lock)) // should be holding process table lock
    panic("sched ptable.lock");
  if(mycpu()->ncli != 1) // should not be holding any other locks (lest we cause a deadlock)
    panic("sched locks");
  if(p->state == RUNNING) // should not be RUNNING since we're about to stop running it
    panic("sched running");
  if(readeflags()&FL_IF)
    panic("sched interruptible");
  // pushcli() and popcli() check whether interrupts were enabled before turning them off while holding
  // a lock, but this is really a property of this kernel thread, not of this CPU, so we need to save that
  intena = mycpu()->intena;
  // call swtch() to pick up where the scheduler left off (line after its own call to swtch())
  swtch(&p->context, mycpu()->scheduler);
  // this process will resume executing eventually, at which point we'll restore the data about whether
  // interrupts were enabled and let it run again
  mycpu()->intena = intena;
}

// Example of how all this comes together
// Forces process to give up the CPU for one scheduling round.
// e.g. used to handle timer interrupts
// Now that we know how scheduling works in xv6, yield() is easy
void
yield(void)
{
  acquire(&ptable.lock);  //DOC: yieldlock
  myproc()->state = RUNNABLE; // can be picked up in next scheduling round
  sched(); // switch into scheduler
  release(&ptable.lock); // release lock when we eventually return here
}

// A fork child's very first scheduling by scheduler()
// will swtch here.  "Return" to user space.
// i.e. example of where a process might start to execute after being scheduled
// All processes (first, fork()ed) will start running code in forkret(), then return from here into trapret()
// Most of the time, the function does 1 thing - release the process table lock
// However, there are two kernel initialization functions that have to be run from user mode, so we can't
// just call them from main()
// forkret() is as good a place as any to call them
// Thus the first call to forkret() calls these two startup functions
// Any other kernel code that switches into the scheduler (e.g. sleep() and yield()) will have a similar lock
// release right after returning from the scheduler
void
forkret(void)
{
  static int first = 1;
  // Still holding ptable.lock from scheduler.
  release(&ptable.lock);

  if (first) {
    // Some initialization functions must be run in the context
    // of a regular process (e.g., they call sleep), and thus cannot
    // be run from main().
    // Part of xv6's file system code
    first = 0;
    iinit(ROOTDEV);
    initlog(ROOTDEV);
  }

  // Return to "caller", actually trapret (see allocproc).
}

// Basics of sleep() and wakeup() - act as mechanisms for sequence coordination or conditional synchronization
// This allows processes to communicate with each other by sleeping while waiting for conditions to be
// fulfilled and waking up other processes when those conditions are satisfied
// Processes go to sleep on a channel and wake up other processes sleeping on a channel
// In many OSes, this is achieved via channel queues or even more complex data structures, but xv6 makes it
// as simple as possible by simply using pointers (or equivalently integers) as channels
// The kernel can just use any convenient address as a pointer for one process to sleep on while other
// processes send a wakeup call using the same pointer
// Multiple processes may be sleeping on the same channel, either because they're waiting for the same
// condition or because two sleep()/wakeup() pairs accidentally used the same channel
// Thus a process may be awoken before its condition is fulfilled
// Thus require every call to sleep() to occur inside a loop that checks the condition so it is put back to
// sleep on spurious wakeups - see an example in sys_sleep() which checks if the right number of ticks passed
// A common concurrency danger with conditional synchronization in any OS is the problem of missed wakeup
// calls - if the process that's supposed to send the wakeup call runs before the process that's supposed to
// sleep, it's possible the sleeping process will never be awoken again
// This problem is more general than processes - it applies to devices too
// Scenario
// - a process tries to read from disk
// - it'll check whether the data is ready yet and go to sleep in a loop until it is
// - if the disk runs first, the process will just find the data ready and waiting for it and can
//   continue
// - if the process runs first, we'll see the data isn't ready yet and sleep in a loop until it is; the disk
//   wakes up the process once the data is ready
// - if they run at the same time, or in between each other, the process does its check and finds the data
//   isn't ready, but before it can go to sleep, a timer interrupt or other trap goes off and the kernel
//   switches processes. Then the disk finishes reading and starts a disk interrupt that sends a wakeup
//   call to any sleeping processes, but the process isn't sleeping yet. When the process starts running
//   again later on, it'll go to sleep - having already missed its wakeup call
// - i.e. the problem is the process can get interrupted between checking the condition and going to sleep
// - idea - disable interrupts there with pushcli() and popcli()
// - but - the disk driver may be running simultaneously on another CPU. The other CPU may still send the
//   disk's wakeup call too early
// - idea - use a lock - process holds lock while it checks condition and sleeps, and the disk driver will
//   have to acquire the lock before it can send its wakeup call
// - but - if the process holds the lock while it's sleeping, the disk driver will never be able to acquire
//   it - deadlock
// - idea - use a lock, but have sleep() release it right away, then reacquire before waking up. That way the
//   lock will be free while the process is sleeping so the disk driver can acquire it
// - but - back to the original problem - if the lock gets released inside sleep() before the process is
//   actually sleeping, the wakeup call might happen in between those and get missed
// - so - we need a lock, and we can't hold the lock while sleeping, or we'd get a deadlock. But we also
//   can't release it before sleeping, or we might miss a wakeup call
// - see sleep() implementation for how it solves the problem
// DANGER - any lock passed to sleep() must always get acquired before ptable.lock to avoid deadlock
// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void
sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();
  
  if(p == 0)
    panic("sleep"); // CPU is running a process and not the scheduler (which can't go to sleep)

  if(lk == 0)
    panic("sleep without lk"); // caller passed in an arbitrary lock

  // we need to release the lock and put the process to sleep
  // this will require modifying its state, so we acquire the process table lock
  // but we must not reacquire the same lock - just keep using it without releasing
  // Must acquire ptable.lock in order to
  // change p->state and then call sched.
  // Once we hold ptable.lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup runs with ptable.lock locked),
  // so it's okay to release lk.
  if(lk != &ptable.lock){  //DOC: sleeplock0
    acquire(&ptable.lock);  //DOC: sleeplock1
    release(lk);
  }
  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;

  // perform context switch into scheduler so it can run a new process
  // remember we have to be holding the process table lock
  sched();

  // when the process gets a wakeup call and wakes up, it'll eventually be run by the scheduler, at which
  // point it will context switch back here
  // reset channel and reacquire the original lock before returning
  
  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  if(lk != &ptable.lock){  //DOC: sleeplock2
    release(&ptable.lock);
    acquire(lk);
  }
}
// reasoning for this implementation not missing any wakeup calls
// after all, we release the original lock before putting the process to sleep
// we're holding the process table lock at that point, which at least means interrupts are disabled
// but the process waking this one might already be running on another CPU and might send the wakeup signal
// in between releasing the original lock and updating this process's channel and state
// we will see how this gets solved in wakeup()

//PAGEBREAK!
// Wake up all processes sleeping on chan.
// The ptable lock must be held.
static void
wakeup1(void *chan)
{
  struct proc *p;

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == SLEEPING && p->chan == chan)
      p->state = RUNNABLE;
}

// Wake up all processes sleeping on chan.
void
wakeup(void *chan)
{
  acquire(&ptable.lock);
  wakeup1(chan);
  release(&ptable.lock);
}

// One of the functions that can get called both by the kernel and as a syscall
// Kernel uses it to terminate malicious or buggy processes
// Killing a process immediately would present all kinds of risks (corrupting kernel data structures being
// updated, etc.), thus use p->killed
// trap() will actually kill the next time the process passes through there
// Also, some calls to sleep() will occur in a while loop that checks if p->killed has been set since the
// process started sleeping, so we can hasten its death by setting its state to RUNNABLE so it'll wake up
// and encounter those checks faster
// No risk of screwing up by waking a process too early, since each call to sleep() should be in a loop
// that will put it back to sleep if it's not ready to wake up yet
// Kill the process with the given pid.
// Process won't exit until it returns
// to user space (see trap in trap.c).
int
kill(int pid)
{
  struct proc *p;

  acquire(&ptable.lock);
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->pid == pid){
      p->killed = 1;
      // Wake process from sleep if necessary.
      if(p->state == SLEEPING)
        p->state = RUNNABLE;
      release(&ptable.lock);
      return 0;
    }
  }
  release(&ptable.lock);
  return -1;
}

//PAGEBREAK: 36
// Print a process listing to console.  For debugging.
// Runs when user types ^P on console. (keyboard interrupt handler function sets this up)
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char *states[] = {
  [UNUSED]    "unused",
  [EMBRYO]    "embryo",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
  };
  int i;
  struct proc *p;
  char *state;
  uint pc[10];

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    cprintf("%d %s %s", p->pid, state, p->name);
    // sleep() and wakeup() syscalls involve some lock trickery
    // so sleeping processes could be a common cause of concurrency issues like deadlocks
    // Thus print out call stack of sleeping processes
    if(p->state == SLEEPING){
      getcallerpcs((uint*)p->context->ebp+2, pc);
      for(i=0; i<10 && pc[i] != 0; i++)
        cprintf(" %p", pc[i]);
    }
    cprintf("\n");
  }
}
