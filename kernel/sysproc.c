#include "types.h"
#include "x86.h"
#include "defs.h"
#include "date.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"

int
sys_fork(void)
{
  return fork();
}

int
sys_exit(void)
{
  exit(); // closes a process but puts it in ZOMBIE state
  return 0;  // not reached, just for the compiler
}

int
sys_wait(void)
{
  return wait();
}

int
sys_kill(void)
{
  int pid;

  if(argint(0, &pid) < 0)
    return -1;
  return kill(pid); // tags pid with 'killed' field, trap() will check this
}

int
sys_getpid(void)
{
  return myproc()->pid;
}

int
sys_sbrk(void)
{
  int addr;
  int n;

  if(argint(0, &n) < 0)
    return -1;
  addr = myproc()->sz;
  if(growproc(n) < 0)
    return -1;
  return addr;
}

// sleep plays a dual role in xv6
// it can be used by processes or by the kernel for processes that need to wait for something, e.g. disk
// in the latter case cannot know how long to sleep for
// thus sleep() makes process state SLEEPING on a *channel* (int)
// e.g. kernel puts a process waiting on the disk to sleep using a channel assigned to the disk
// and disk interrupt wakes up any processes sleeping on the disk channel
// for process use, the channel is the address of the ticks counter
// thus process awoken at every timer interrupt; we loop until the right amount of ticks have passed
int
sys_sleep(void)
{
  int n;
  uint ticks0;

  if(argint(0, &n) < 0) // ticks to sleep for
    return -1;
  acquire(&tickslock);
  ticks0 = ticks;
  while(ticks - ticks0 < n){
    if(myproc()->killed){ // hasten process death
      release(&tickslock);
      return -1;
    }
    sleep(&ticks, &tickslock); // releases lock and reacquires before waking up
  }
  release(&tickslock);
  return 0;
}

// return how many clock tick interrupts have occurred
// since start.
int
sys_uptime(void)
{
  uint xticks;

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);
  return xticks;
}
