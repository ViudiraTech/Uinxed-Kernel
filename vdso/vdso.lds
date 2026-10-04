/*
 * Link the vDSO image as a single loadable segment whose virtual address 0 is
 * the ELF header.
 *
 * The dynamic linker derives the image's link-time base from the program
 * headers, so the image has to look exactly like the one Linux hands out: one
 * PT_LOAD starting at 0.  A second PT_LOAD -- even one carrying nothing but the
 * clock data page -- makes the linker pick that segment's address as the base
 * and resolve every dynamic entry a page off, which faults the moment it reads
 * the dynamic section.
 *
 * The clock data page therefore sits page aligned behind the code, inside the
 * same segment.  The kernel maps its shared page over that address, and because
 * the linker resolves the code's access to it to a fixed PC-relative
 * displacement, one image serves every process at any base.
 */
OUTPUT_FORMAT("elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)
ENTRY(__vdso_clock_gettime)

PHDRS
{
    /* FILEHDR PHDRS starts the segment at offset 0 so it covers the ELF header. */
    text PT_LOAD FILEHDR PHDRS FLAGS(5); /* R + X: the whole image */
    /* Declared explicitly: a section alone does not produce this segment. */
    dynamic PT_DYNAMIC FLAGS(4);
}

SECTIONS
{
    . = SIZEOF_HEADERS;

    .hash : { *(.hash) } :text
    .gnu.hash : { *(.gnu.hash) } :text
    .dynsym : { *(.dynsym) } :text
    .dynstr : { *(.dynstr) } :text
    .gnu.version : { *(.gnu.version) } :text
    .gnu.version_d : { *(.gnu.version_d) } :text
    .rodata : { *(.rodata*) } :text
    .text : { *(.text*) } :text
    .dynamic : { *(.dynamic) } :text :dynamic

    /*
     * 0x1000 is where the kernel puts the shared clock data page, so the code
     * and its tables have to stay inside the page below it.  Keep the number in
     * step with VDSO_DATA_OFFSET in include/kernel/vdso/vdso.h; the assertion
     * catches a build where the code grew past the page it is given.
     */
    ASSERT(. <= 0x1000, "vDSO code outgrew the page before its data page")
    . = 0x1000;
    .vvar : { *(.vvar) } :text

    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) *(.interp) }
}

VERSION
{
    LINUX_2.6 {
        global:
            __vdso_clock_gettime;
            __vdso_clock_getres;
            __vdso_gettimeofday;
            __vdso_time;
        local: *;
    };
}
