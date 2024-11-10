// spin locks have harsh performance costs
// So far we've only seen locks for kernel resources like the process table, page allocator, and console,
// for all of which operations should be relatively fast (few dozen CPU cycles at most)
// For the Disk driver and file system implementation, we'll need locks too, but disk operations are slow
// Spin locks were the best we could do at the time, since we didn't have any infrastructure to support
// more complex locks - but now we have kernel building blocks in place relating to proecsses, including
// a bunch of syscalls, e.g. sleep() and wakeup(), which let a process give up the CPU until some condition
// is met. This condition could be that a lock is free to acquire - sleep locks

// If we want a process holding a sleep-lock to give up the processor in the middle of a critical section,
// then sleep-locks have to work well when held across context switches
// They also have to leave interrupts enabled
// This couldn't happen with spin-locks - it was important they disable interrupts to prevent deadlocks
// and ensure a kernel thread can't get rescheduled in the middle of updating some important data structure
// Leaving interrupts on adds some extra challenges
// - Have to make sure the lock can still be acquired atomically
//   - Make each sleep-lock a two-tiered deal with a spin-lock to protect its acquisition
// - Have to make sure any operations in the critical section can safely resume after being interrupted

// Long-term locks for processes
struct sleeplock {
  uint locked;       // Is the lock held? Just like spin-locks
  struct spinlock lk; // spinlock protecting this sleep lock (i.e. protecting 'locked' field)
  
  // For debugging:
  char *name;        // Name of lock.
  int pid;           // Process holding lock
};

