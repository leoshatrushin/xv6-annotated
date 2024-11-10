// The I/O APIC manages hardware interrupts for an SMP system.
// http://www.intel.com/design/chipsets/datashts/29056601.pdf
// See also picirq.c.

// Primary function - recceive external interrupt events from the system, associated with I/O devices
// and relay them to the LAPIC as interrupt messages
// with the exception of the LAPIC timer, all external devices are going to use the IRQs provided by it
// (like it was done in the past by the PIC)

// configure the I/O APIC
// - get the I/O APIC base address from the MADT
//   - the MADT is available within the RSDT data, search for MADT item type 1
//   - byte 2 - I/O APIC ID (mostly fluff, as we'll be accessing the IOAPIC by its MMIO address)
//   - byte 3 - reserved (0)
//   - bytes 4-7 - IOAPIC address
//   - bytes 8-11 - global system interrupt base (first interrupt number the IOAPIC handles)
//     - in the case of most systems with only a single IOAPIC, this is 0
// - read the I/O APIC Interrupt Source Override Table
// - initialize the IO redirection table entries for the interrupt we want to enable

// Interrupt source overrides
// they contain the differences between the IA-PC standard and the dual 8250 interrupt definitions
// the ISA interrupts should be identity mapped into the first IOAPIC sources, but most of the time
// there will be at least one exception, this table contains those exceptions
// e.g. PIT Timer is connected to ISA IRQ 0, but when the APIC is enabled it is connected to IOAPIC
// interrupt pin 2, so in this case we need an interrupt source override where the source entry (bus source)
// is 0 and the global system interrupt is 2
// the values stored in the IOAPIC interrupt source overrides in the MADT are
// byte 2 - bus source (should be 0, the ISA IRQ source), starting from ACPIv2 also a reserved field
// byte 3 - IRQ source (source IRQ pin)
// bytes 4-7 - global system interrupt (target IRQ on the APIC)
// bytes 8-9 - flags
// - bits 0-1 - polarity
//   - 00 - use default settings - active-low for level-triggered interrupts
//   - 01 - active high
//   - 10 - reserved
//   - 11 - active low
// - bits 2-3 - trigger mode of APIC I/O input signals
//   - 00 - use default settings - in the ISA is edge-triggered
//   - 01 - edge-triggered
//   - 10 - reserved
//   - 11 - level-triggered
// - bits 4-15 - reserved, must be 0

#include "types.h"
#include "defs.h"
#include "traps.h"

#define IOAPIC  0xFEC00000   // Default physical address of IO APIC
// IOAPIC has 2 memory-mapped registers for accessing the other IOAPIC registers
#define IOREGSEL IOAPIC+0x00  // I/O register select, used to select the offset of the I/O register to access
                              // bits 0-7 - APIC register address
                              // bits 8-31 - reserved
#define IOWIN    IOAPIC+0x10  // I/O Window (data), used to access data selected by IOREGSEL
                              // i.e. data is read/written from here, when the register is accessed

// 4 I/O registers that can be accessed using the two above
#define REG_ID     0x00  // IOAPIC ID (R/W)
#define REG_VER    0x01  // IOAPIC version (RO)
#define REG_ARB    0x02  // IOAPIC BUS arbitration priority (RO)
#define REG_TABLE  0x10  // Redirection table base (RW)
#define IOAPICID   REG_ID
#define IOAPICVER  REG_VER
#define IOAPICARB  REG_ARB
// IOREDTBL - 0x03-0x3F // redirection tables
// each entry is 2 registers starting from offset 0x10
// lower 4 bytes is basically an LVT entry
// upper 4 bytes
// - bits 17-55 - reserved
// - bits 56-59 - destination field, in physical addressing mode (see the destination bit of the entry)
//   it is the LAPIC ID to forward the interrupts to, for more info read the IOAPIC datasheet
// number of items stored in the IOAPIC MADT entry, but usually on modern architectures is 24

// number of inputs an IOAPIC supports - bits 16-23 of IOAPICVER + 1

// The redirection table starts at REG_TABLE and uses
// two registers to configure each interrupt.
// The first (low) register in a pair contains configuration bits.
// The second (high) register contains a bitmask telling which
// CPUs can serve that interrupt.
#define INT_DISABLED   0x00010000  // Interrupt disabled
#define INT_LEVEL      0x00008000  // Level-triggered (vs edge-)
#define INT_ACTIVELOW  0x00002000  // Active low (vs high)
#define INT_LOGICAL    0x00000800  // Destination is CPU id (vs APIC ID)

volatile struct ioapic *ioapic;

// IO APIC MMIO structure: write reg, then read or write data.
// Note - alternative to MMIO is called PMIO (port-mapped)
struct ioapic {
  uint reg;
  uint pad[3];
  uint data;
};

static uint
ioapicread(int reg)
{
  ioapic->reg = reg;
  return ioapic->data;
}

static void
ioapicwrite(int reg, uint data)
{
  ioapic->reg = reg;
  ioapic->data = data;
}

void
ioapicinit(void)
{
  int i, id, maxintr;

  ioapic = (volatile struct ioapic*)IOAPIC;
  maxintr = (ioapicread(REG_VER) >> 16) & 0xFF;
  id = ioapicread(REG_ID) >> 24;
  if(id != ioapicid)
    cprintf("ioapicinit: id isn't equal to ioapicid; not a MP\n");

  // Mark all interrupts edge-triggered, active high, disabled,
  // and not routed to any CPUs.
  for(i = 0; i <= maxintr; i++){
    ioapicwrite(REG_TABLE+2*i, INT_DISABLED | (T_IRQ0 + i));
    ioapicwrite(REG_TABLE+2*i+1, 0);
  }
}

void
ioapicenable(int irq, int cpunum)
{
  // Mark interrupt edge-triggered, active high,
  // enabled, and routed to the given cpunum,
  // which happens to be that cpu's APIC ID.
  ioapicwrite(REG_TABLE+2*irq, T_IRQ0 + irq);
  ioapicwrite(REG_TABLE+2*irq+1, cpunum << 24);
}
