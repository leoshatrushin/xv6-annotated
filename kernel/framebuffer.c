// ways to get output on the screen
// - linear framebuffer (pixels linear in memory)
//   - the only framebuffer type reliably available on x86_64 and other platforms
//   - often obtained through BIOS routines on older systems or the Graphics Output Protocol (GOP) in UEFI
//   - most boot protocols will present these framebuffers in a uniform way to the kernel
// - in real mode there were BIOS routines that could be called to print to the display
//   - there were sometimes extensions for hardware-accerated drawing too
//   - this is not implemented in modern systems and only available to real mode software
// - in legacy systems some display controllers supported something called 'text mode' where the screen
//   is an array of characters
//   - this is long deprecated, and UEFI actually requires all displays to operate as pixel-based framebuffers
//   - often the text mode buffer lived around 0xB800, composed of byte pairs (ascii, fore/background color)

// requesting a framebuffer mode using Grub (can also be done with UEFI)
// add the relevant tag in the multiboot2 header

// accessing the framebuffer
// the multiboot header provided in the documentation already has a struct multiboot_tag_framebuffer
// this is provided by grub in a 'framebuffer_info' tag
