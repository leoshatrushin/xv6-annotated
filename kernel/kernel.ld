/* Simple linker script for the JOS kernel.
   See the GNU ld 'info' manual ("info ld") to learn the syntax. */

/* Purpose - specify virtual and physical address of kernel binary */

/* Linker steps
 * 1. Assign each object a location in memory
 * 2. Resolve outstanding symbols by looking them up in other files and replacing addresses. Linkers can define their own symbols too
 * 3. Create an ELF file
 * Linker scripts used when command-line flags are too basic
*/

OUTPUT_FORMAT("elf32-i386", "elf32-i386", "elf32-i386")
OUTPUT_ARCH(i386)
ENTRY(_start) /* convention used by linker */

SECTIONS
{
	/* Link the kernel at this address: "." means the current address */
        /* Must be equal to KERNLINK */
	/* these addresses are virtual - since we want to set up our kernel in the higher half of VM,
	 * we tell the klinker to link the code start at 0x80100000 */
	/* Not 0x80000000 (physical address 0) to avoid address space of bootloader and MMIO devices */
	. = 0x80100000;

	.text : AT(0x100000) { /* "load address" - specify where in physical memory to place */
		*(.text .stub .text.* .gnu.linkonce.t.*) /* which sections to include */
	}

	PROVIDE(etext = .);	/* Define the 'etext' symbol to this value */

	.rodata : {
		*(.rodata .rodata.* .gnu.linkonce.r.*)
	}

	/* Page-align data segment */
	. = ALIGN(0x1000);

	/* Conventionally, Unix linkers provide pseudo-symbols
	 * etext, edata, and end, at the end of the text, data, and bss.
	 * For the kernel mapping, we need the address at the beginning
	 * of the data section, but that's not one of the conventional
	 * symbols, because the convention started before there was a
	 * read-only rodata section between text and data. */
	/* If the code provides its own version of a symbol, the linker symbol is thrown away */
	PROVIDE(data = .);

	.data : {
		*(.data)
	}

	PROVIDE(edata = .);

	.bss : {
		*(.bss)
	}

	PROVIDE(end = .);

	/DISCARD/ : {
		*(.eh_frame .note.GNU-stack)
	}
}
