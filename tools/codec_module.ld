ENTRY(module_entry)
SECTIONS {
    . = 0;
    .text : { *(.text*) *(.glue_7*) *(.vfp11_veneer) *(.rodata*) }
    .ARM.extab : { *(.ARM.extab*) }
    .ARM.exidx : { __exidx_start = .; *(.ARM.exidx*) __exidx_end = .; }
    .init_array : {
        __init_array_start = .;
        KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*))) KEEP(*(.init_array))
        __init_array_end = .;
    }
    .fini_array : {
        __fini_array_start = .;
        KEEP(*(SORT_BY_INIT_PRIORITY(.fini_array.*))) KEEP(*(.fini_array))
        __fini_array_end = .;
    }
    .data : { *(.data*) }
    .got ALIGN(4) : { *(.got*) LONG(0xFFFFFFFF) }
    .bss : { *(.bss*) *(COMMON) }
    end = .; _end = .;
}
