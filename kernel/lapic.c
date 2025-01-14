// The local APIC (Advancced Programmable Interrupt Controller) manages internal (non-I/O) interrupts.
// Updated Intel standard for PIC, used in multiprocessor systems
// Used for sophisticated interrupt redirection, and for sending interrupts between processors, which was
// not possible in PIC
// See Chapter 8 & Appendix C of Intel processor manual volume 3.

// In an APIC-based system, each CPU is made of a "core" and a "local APIC"
// local APIC is responsible for cpu-specific interrupt configuration
// e.g. contains the Local Vector Table (LVT) that translates events such as "internal clock" and
// other "local" interrupt sources into a interrupt vector
// In addition, there is an I/O APIC (e.g. intel 82093AA) that is part of the chipset and provides
// multi-processor interrupt management, incorporating both static and dynamic symmetric interrupt
// distribution across all processors
// Each interrupt pin is programmable as edge or level triggered
// Each interrupt has interrupt vector and interrupt steering information
// Inter-Processor Interrupts (IPIs) are generated by a local APIC and can be used as basic signalling
// for scheduling coordination, multi-processor bootstrapping, etc.

// xv6 is designed for a board for multiple processors - it ignores interrupts from the PIC, and configures
// the IOAPIC (part in the I/O system), and the local APIC (part in each CPU)
// IOAPIC has a table and the processor can program entries in the table through MMIO
// During initialization, xv6 maps interrupt 0 to IRQ 0 and so on, but disables them all
// Specific devices enable particular interrupts and say to which processor to route them
// e.g. keyboard interrupts to processor 0, disk interrupts to highest number processor
// The timer chip is inside the lapic, so each processor can receive timer interrupts independently

// The APIC is a device used to manage incoming interrupts to a core
// It replaces the old PIC8259 (that remains still available) and offers more functionalities, especially
// when dealing with SMP
// The biggest limitation of the PIC was that it could only deal with one CPU at a time
// Itenl later developed a version of the APIC called the SAPIC for the Itanium platform
// These are referred to as the *xapic*, parse this as 'local APIC' in documentation

// There are two types of APIC
// Local APIC
// - present in every core, responsible for handling interrupts for that core
// - can also be used for sending an IPI (Inter-processor interrupt) to other cores
// - as well as generating some interrupts itself
// - interrupts generated by the local APIC are controlled by the LVT (local vector table), part of the
//   local APIC registers
// - the most interesting of these is the timer LVT
// I/O APIC
// - acts as a gateway for devices in the system to send interrupts to local APICs
// - most PCs have 1 I/O APIC, more complex systems like servers or industrial equipment may have multiple
// - has a number of input pins which a connected device triggers when it wants to send a local interrupt
// - when a pin is triggered, the I/O APIC will send an interrupt to one or more local APICs, depending on
//   the redirection entry for that pin
// Both types are accessed via MMIO, the base addresses should be fetched from the proper places as the
// firmware (or even the bootloader) may move these around

#include "param.h"
#include "types.h"
#include "defs.h"
#include "date.h"
#include "memlayout.h"
#include "traps.h"
#include "mmu.h"
#include "x86.h"

// Timer
// When the local APIC is built in core's crystal, core's frequency is used, else bus frequency
// APIC timer frequency is equal to the bus frequency or the core crystal's frequency divided by the chosen
// frequency divider
// these can be found in the CPUID functions 0x15 and 0x16

// Timer modes
// Periodic
// - software sets an "initial count" and the lapic uses it for a "current count"
// - lapic decrements until 0, then generates a timer IRQ and resets
// - decrement rate depends on CPU's external frequency ("bus frequency") divided by lapic's TDCR register
// One-shot
// - count not reset
// - software has to set a new count each time
// - advantage - precise control
// - e.g. OS sets for process depending on priority
// - e.g. OS sets based on earliest future event - preempt task, sleeping task wakeup, alarm to be sent
// - disadvantage - harder to track real time
// - disadvantage - special care to avoid race conditions, especially if new count is set before old expires
// TSC-Deadline (only on newer CPUS)
// - IRQ when CPU's time stamp counter reaches "deadline" value
// - advantage - higher precision, since it uses CPU's (nominal) internal frequency instead of CPU's external/bus frequency
// - advantage - easier to avoid/handle race conditions

// Discovering the LAPIC
// to get the location of the memory-mapped APIC registers we need to read the MSR (model-specific register)
// 'rdmsr' - reads content of MSR specified in ecx, result placed in eax and edx
// here want to read IA32_APIC_BASE (0x1B)
// - bits 0-7 - reserved
// - bit 8 - processor is the Bootstrap Processor (BSP)
// - bits 9-10 - reserved
// - bit 11 - APIC global enable, clear to disable LAPIC (realistically no reason to do this on modern cores)
// - bits 12-31 - LAPIC MMIO base physical address
// - bits 32-63 - reserved
// typically base address is 0xFEE0000

// X2 APIC
// an extension of the XAPIC (LAPIC in its regular mode)
// main difference is registers are now accessed via MSRs and the ID register is 32-bits
// perfectly fine not to support this mode
// check for support - cpuid leaf 1, bit 21 in ecx
// enable - bit 10 in IA32_APIC_BASE MSR
// - once enabled, cannot transition back to regular APIC without a reset
// - once enabled, LAPIC registers are no longer memory-mapped (error on access)
// - instead accessed as a range of MSRs starting at 0x800
// - since each MSR is 64-bits wide, right-shift offset by 4 bits, e.g. spurious MST is 0x80F
// since each MSR is 64-bits, the upper 32 bits are 0 on reads and ignored on writes
// except for the ICR register (used for sending IPIs to other cores), which is now a single 64-bit register

// Handling interrupts
// once an interrupt for the LAPIC is served, it won't send further interrupts until an EOI signal is sent
// a separate mechanism to the interrupt flag IF which also disables interrupts
// there are some exceptions where sending an EOI isn't needed, mainly spurious interrupts and NMIs
// EOI can be sent at any time before iret

// Sending an inter-processor interrupt (IPI)
// to support SMP (symmetric multi-processing), we need to inform other cores an event has occured
// IPIs don't carry any information, only a signal
// to send data a struct is usually placed in memory somewhere, sometimes called a *mailbox*
// to send an IPI we need to know the LAPIC ID of the target core
// also need a vector in the IDT for IPIs
// with these two things we can use the 64-bit ICR (interrupt command register)
// IPI is sent when the lower ICR register is written to
// so we set up the destination in the higher half first, then write the vector in the lower half
// ICR also contains a few fields but most can be safely ignored and left to 0
// bits 56-63 (32-63 in X2APIC mode) - target LAPIC ID
// bits 0-7 - interrupt vector that will be served on the target core
// bits 18-19 - shorthand field in the ICR which overrides the destination ID
// - 00 - no shorthand, use destination ID
// - 01 - send to ourselves
// - 10 - send to all LAPICs, including ourselves
// - 11 - send to all LAPICs, not including ourselves

// Local Vector Table (LVT) has 6 items
// allows software to specify how local interrupts are delivered
// 0 - Timer - useful for controlling the local APIC timer
// 1 - Thermal monitor - used for configuring interrupts when certain thermal conditions are met
// 2 - Performance counter - allows an interrupt to be generated when a performance counter overflows
// 3 - LINT0 - specify interrupt delivery when an interrupt is signalled on the LINT0 pin
// 4 - LINT1 - specify interrupt delivery when an interrupt is signalled on the LINT1 pin
// - these pins are mostly used for emulating the legacy PIC, but may also be used as NMI sources
// - best left untouched until we have parsed the MADT
// 5 - Error - configure how LAPIC should report an internal error
// LVT entries except timer LVT use the following format
// thermal sensor and performance entries ignore bits 13:15
// - 0:7 - interrupt vector, IDT entry to trigger for this interrupt
// - 8:10 - delivery mode, how APIC should present interrupt to processor, fixed mode (0b000) fine in
//   almost all cases, other modes are for specific functions or advanced usage
// - 11 - destimation mode (physical vs logical)
// - 12 - delivery status (read-only), whether interrupt has been served or not
// - 13 - pin polarity - active-high vs active-low
// - 14 - remote IRR (read-only), used by APIC for manging level-triggered interrupts
// - 15 - trigger mode, edge-triggered vs level-triggered
// - 16 - interrupt mask, 1=disabled

// LAPIC is usually located at 0xFEE00000, but this should be obtained from the MSR instead of hardcoding

// Local APIC registers, divided by 4 for use as uint[] indices.
#define ID      (0x0020/4)   // 8-bit physical ID, unique & assigned by the firmware on first startup
                             // often used to distinguish processors
                             // recommended to treat as read-only
#define VER     (0x0030/4)   // Version, contains some useful (if not really needed) information
#define TPR     (0x0080/4)   // Task Priority
#define EOI     (0x00B0/4)   // EOI (end of interrupt)
#define SVR     (0x00F0/4)   // Spurious Interrupt Vector, also contains misc config
                             // bits 0-7 - spurious vector number - for spurious interrupts generated by APIC
                             // older CPUs force this between 0xF0-0xFF, best to follow for compatibility
                             // bit 8 - APIC software enable/disable
                             // bit 9 - focus processor checking, optional advanced feature indicating some
                             // interrupts can be routed according to a list of priorities
                             // bits 10-31 - reserved, read-only
  #define ENABLE     0x00000100   // Unit Enable
#define ESR     (0x0280/4)   // Error Status
#define ICRLO   (0x0300/4)   // Interrupt Command
  #define INIT       0x00000500   // INIT/RESET
  #define STARTUP    0x00000600   // Startup IPI
  #define DELIVS     0x00001000   // Delivery status
  #define ASSERT     0x00004000   // Assert interrupt (vs deassert)
  #define DEASSERT   0x00000000
  #define LEVEL      0x00008000   // Level triggered
  #define BCAST      0x00080000   // Send to all APICs, including self.
  #define BUSY       0x00001000
  #define FIXED      0x00000000
#define ICRHI   (0x0310/4)   // Interrupt Command [63:32]
#define TIMER   (0x0320/4)   // Local Vector Table (LVT) 0 (TIMER)
  #define X1         0x0000000B   // divide counts by 1
  #define PERIODIC   0x00020000   // Periodic
#define THERM   (0x0330/4)   // Local Vector Table Thermal Monitor
#define PCINT   (0x0340/4)   // Local Vector Table Performance Counter
// these local interrupt lines can be used to handle interrupts generated by certain local events
// such as non-maskable interrupts (NMIs) or the interrupt request (INTR) pin
#define LINT0   (0x0350/4)   // Local Vector Table 1 (LINT0)
#define LINT1   (0x0360/4)   // Local Vector Table 2 (LINT1)
#define ERROR   (0x0370/4)   // Local Vector Table 3 (ERROR)
  #define MASKED     0x00010000   // Interrupt masked
#define TICR    (0x0380/4)   // Timer Initial Count
#define TCCR    (0x0390/4)   // Timer Current Count
#define TDCR    (0x03E0/4)   // Timer Divide Configuration

volatile uint *lapic;  // Initialized in mp.c

//PAGEBREAK!
static void
lapicw(int index, int value)
{
  lapic[index] = value;
  lapic[ID];  // wait for write to finish, by reading
}

void
lapicinit(void)
{
  if(!lapic)
    return;

  // Enable local APIC; set spurious interrupt vector to interrupt 0xFF
  lapicw(SVR, ENABLE | (T_IRQ0 + IRQ_SPURIOUS));

  // The timer repeatedly counts down at bus frequency
  // from lapic[TICR] and then issues an interrupt.
  // If xv6 cared more about precise timekeeping,
  // TICR would be calibrated using an external time source.
  lapicw(TDCR, X1);
  // interrupt 32 in periodic mode
  lapicw(TIMER, PERIODIC | (T_IRQ0 + IRQ_TIMER));
  lapicw(TICR, 10000000);

  // Disable logical interrupt lines.
  lapicw(LINT0, MASKED);
  lapicw(LINT1, MASKED);

  // Disable performance counter overflow interrupts
  // on machines that provide that interrupt entry.
  if(((lapic[VER]>>16) & 0xFF) >= 4) // isolates 8 bits that count max number of LVT entries
    lapicw(PCINT, MASKED);

  // Map error interrupt to IRQ_ERROR.
  lapicw(ERROR, T_IRQ0 + IRQ_ERROR);

  // Clear error status register (requires back-to-back writes).
  lapicw(ESR, 0);
  lapicw(ESR, 0);

  // Ack any outstanding interrupts.
  lapicw(EOI, 0);

  // Send an Init Level De-Assert to synchronise arbitration ID's.
  lapicw(ICRHI, 0);
  lapicw(ICRLO, BCAST | INIT | LEVEL);
  while(lapic[ICRLO] & DELIVS)
    ;

  // Enable interrupts on the APIC (but not on the processor).
  lapicw(TPR, 0);
}

int
lapicid(void)
{
  if (!lapic)
    return 0;
  return lapic[ID] >> 24;
}

// Tell local interrupt controller we acknowledge the current interrupt so it can clear it and get ready
// for more interrupts
void
lapiceoi(void)
{
  if(lapic)
    lapicw(EOI, 0);
}

// Spin for a given number of microseconds.
// On real hardware would want to tune this dynamically.
void
microdelay(int us)
{
}

#define CMOS_PORT    0x70
#define CMOS_RETURN  0x71

// Start additional processor running entry code at addr.
// See Appendix B of MultiProcessor Specification.
void
lapicstartap(uchar apicid, uint addr)
{
  int i;
  ushort *wrv;

  // "The BSP must initialize CMOS shutdown code to 0AH
  // and the warm reset vector (DWORD based at 40:67) to point at
  // the AP startup code prior to the [universal startup algorithm]."
  outb(CMOS_PORT, 0xF);  // offset 0xF is shutdown code
  outb(CMOS_PORT+1, 0x0A);
  wrv = (ushort*)P2V((0x40<<4 | 0x67));  // Warm reset vector
  wrv[0] = 0;
  wrv[1] = addr >> 4;

  // "Universal startup algorithm."
  // Send INIT (level-triggered) interrupt to reset other CPU.
  lapicw(ICRHI, apicid<<24);
  lapicw(ICRLO, INIT | LEVEL | ASSERT);
  microdelay(200);
  lapicw(ICRLO, INIT | LEVEL);
  microdelay(100);    // should be 10ms, but too slow in Bochs!

  // Send startup IPI (twice!) to enter code.
  // Regular hardware is supposed to only accept a STARTUP
  // when it is in the halted state due to an INIT.  So the second
  // should be ignored, but it is part of the official Intel algorithm.
  // Bochs complains about the second one.  Too bad for Bochs.
  for(i = 0; i < 2; i++){
    lapicw(ICRHI, apicid<<24);
    lapicw(ICRLO, STARTUP | (addr>>12));
    microdelay(200);
  }
}

#define CMOS_STATA   0x0a
#define CMOS_STATB   0x0b
#define CMOS_UIP    (1 << 7)        // RTC update in progress

#define SECS    0x00
#define MINS    0x02
#define HOURS   0x04
#define DAY     0x07
#define MONTH   0x08
#define YEAR    0x09

static uint
cmos_read(uint reg)
{
  outb(CMOS_PORT,  reg);
  microdelay(200);

  return inb(CMOS_RETURN);
}

static void
fill_rtcdate(struct rtcdate *r)
{
  r->second = cmos_read(SECS);
  r->minute = cmos_read(MINS);
  r->hour   = cmos_read(HOURS);
  r->day    = cmos_read(DAY);
  r->month  = cmos_read(MONTH);
  r->year   = cmos_read(YEAR);
}

// qemu seems to use 24-hour GWT and the values are BCD encoded
void
cmostime(struct rtcdate *r)
{
  struct rtcdate t1, t2;
  int sb, bcd;

  sb = cmos_read(CMOS_STATB);

  bcd = (sb & (1 << 2)) == 0;

  // make sure CMOS doesn't modify time while we read it
  for(;;) {
    fill_rtcdate(&t1);
    if(cmos_read(CMOS_STATA) & CMOS_UIP)
        continue;
    fill_rtcdate(&t2);
    if(memcmp(&t1, &t2, sizeof(t1)) == 0)
      break;
  }

  // convert
  if(bcd) {
#define    CONV(x)     (t1.x = ((t1.x >> 4) * 10) + (t1.x & 0xf))
    CONV(second);
    CONV(minute);
    CONV(hour  );
    CONV(day   );
    CONV(month );
    CONV(year  );
#undef     CONV
  }

  *r = t1;
  r->year += 2000;
}
