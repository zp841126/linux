#ifndef _ASM_POWERPC_SECTIONS_H
#define _ASM_POWERPC_SECTIONS_H
#ifdef __KERNEL__

#include <linux/elf.h>
#include <linux/uaccess.h>
#include <asm-generic/sections.h>

#ifdef __powerpc64__

extern char __start_interrupts[];
extern char __end_interrupts[];

extern char __prom_init_toc_start[];
extern char __prom_init_toc_end[];

static inline int in_kernel_text(unsigned long addr)
{
	if (addr >= (unsigned long)_stext && addr < (unsigned long)__init_end)
		return 1;

	return 0;
}

static inline unsigned long kernel_toc_addr(void)
{
	/* Defined by the linker, see vmlinux.lds.S */
	extern unsigned long __toc_start;

	/*
	 * The TOC register (r2) points 32kB into the TOC, so that 64kB of
	 * the TOC can be addressed using a single machine instruction.
	 */
	return (unsigned long)(&__toc_start) + 0x8000UL;
}

static inline int overlaps_interrupt_vector_text(unsigned long start,
							unsigned long end)
{
	unsigned long real_start, real_end;
	real_start = __start_interrupts - _stext;
	real_end = __end_interrupts - _stext;

	return start < (unsigned long)__va(real_end) &&
		(unsigned long)__va(real_start) < end;
}

static inline int overlaps_kernel_text(unsigned long start, unsigned long end)
{
	return start < (unsigned long)__init_end &&
		(unsigned long)_stext < end;
}

#if !defined(_CALL_ELF) || _CALL_ELF != 2
#undef dereference_function_descriptor
static inline void *dereference_function_descriptor(void *ptr)
{
	struct ppc64_opd_entry *desc = ptr;
	void *p;

	if (!probe_kernel_address(&desc->funcaddr, p))
		ptr = p;
	return ptr;
}
#endif

#endif

#endif /* __KERNEL__ */
#endif	/* _ASM_POWERPC_SECTIONS_H */
