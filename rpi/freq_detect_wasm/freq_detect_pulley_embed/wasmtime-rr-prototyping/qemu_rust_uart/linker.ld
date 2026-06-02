ENTRY(_start)

MEMORY {
  RAM (rwx) : ORIGIN = 0x80000000, LENGTH = 128M
}

SECTIONS {
  . = ORIGIN(RAM);

  /* --- Text --- */
  .text : ALIGN(4) {
    KEEP(*(.text.start))
    *(.text .text.*)
  } > RAM
  PROVIDE(_etext = .);

  /* --- Read-only data --- */
  .rodata : ALIGN(4) {
    *(.rodata .rodata.*)
  } > RAM

  /* --- Data --- */
  .data : ALIGN(4) {
    PROVIDE(_sdata = .);
    *(.data .data.*)
    PROVIDE(_edata = .);
  } > RAM

  /* --- BSS --- */
  .bss : ALIGN(4) {
    PROVIDE(_sbss = .);
    *(.bss .bss.*)
    *(COMMON)
    PROVIDE(_ebss = .);
  } > RAM

  /* Optional: end-of-image symbol */
  PROVIDE(_end = .);

  /* Stack top */
  PROVIDE(_stack_end = ORIGIN(RAM) + LENGTH(RAM));
}

