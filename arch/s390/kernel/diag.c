/*
 * Implementation of s390 diagnose codes
 *
 * Copyright IBM Corp. 2007
 * Author(s): Michael Holzheu <holzheu@de.ibm.com>
 */

#include <linux/module.h>
#include <asm/diag.h>

/*
 * Diagnose 14: Input spool file manipulation
 */
int diag14(unsigned long rx, unsigned long ry1, unsigned long subcode)
{
	register unsigned long _ry1 asm("2") = ry1;
	register unsigned long _ry2 asm("3") = subcode;
	int rc = 0;

	asm volatile(
#ifdef CONFIG_64BIT
		"   sam31\n"
		"   diag    %2,2,0x14\n"
		"   sam64\n"
#else
		"   diag    %2,2,0x14\n"
#endif
		"   ipm     %0\n"
		"   srl     %0,28\n"
		: "=d" (rc), "+d" (_ry2)
		: "d" (rx), "d" (_ry1)
		: "cc");

	return rc;
}
EXPORT_SYMBOL(diag14);

static inline int __diag204(unsigned long subcode, unsigned long size, void *addr)
{
	register unsigned long _subcode asm("0") = subcode;
	register unsigned long _size asm("1") = size;

	asm volatile(
		"	diag	%2,%0,0x204\n"
		"0:\n"
		EX_TABLE(0b,0b)
		: "+d" (_subcode), "+d" (_size) : "d" (addr) : "memory");
	if (_subcode)
		return -1;
	return _size;
}

int diag204(unsigned long subcode, unsigned long size, void *addr)
{
	return __diag204(subcode, size, addr);
}
EXPORT_SYMBOL(diag204);

/*
 * Diagnose 210: Get information about a virtual device
 */
int diag210(struct diag210 *addr)
{
	/*
	 * diag 210 needs its data below the 2GB border, so we
	 * use a static data area to be sure
	 */
	static struct diag210 diag210_tmp;
	static DEFINE_SPINLOCK(diag210_lock);
	unsigned long flags;
	int ccode;

	spin_lock_irqsave(&diag210_lock, flags);
	diag210_tmp = *addr;

#ifdef CONFIG_64BIT
	asm volatile(
		"	lhi	%0,-1\n"
		"	sam31\n"
		"	diag	%1,0,0x210\n"
		"0:	ipm	%0\n"
		"	srl	%0,28\n"
		"1:	sam64\n"
		EX_TABLE(0b, 1b)
		: "=&d" (ccode) : "a" (&diag210_tmp) : "cc", "memory");
#else
	asm volatile(
		"	lhi	%0,-1\n"
		"	diag	%1,0,0x210\n"
		"0:	ipm	%0\n"
		"	srl	%0,28\n"
		"1:\n"
		EX_TABLE(0b, 1b)
		: "=&d" (ccode) : "a" (&diag210_tmp) : "cc", "memory");
#endif

	*addr = diag210_tmp;
	spin_unlock_irqrestore(&diag210_lock, flags);

	return ccode;
}
EXPORT_SYMBOL(diag210);

/*
 * Diagnose 26C: Access Certain System Information
 */
static inline int __diag26c(void *req, void *resp, enum diag26c_sc subcode)
{
	register unsigned long _req asm("2") = (addr_t) req;
	register unsigned long _resp asm("3") = (addr_t) resp;
	register unsigned long _subcode asm("4") = subcode;
	register unsigned long _rc asm("5") = -EOPNOTSUPP;

	asm volatile(
		"	sam31\n"
		"	diag	%[rx],%[ry],0x26c\n"
		"0:	sam64\n"
		EX_TABLE(0b,0b)
		: "+d" (_rc)
		: [rx] "d" (_req), "d" (_resp), [ry] "d" (_subcode)
		: "cc", "memory");
	return _rc;
}

int diag26c(void *req, void *resp, enum diag26c_sc subcode)
{
	return __diag26c(req, resp, subcode);
}
EXPORT_SYMBOL(diag26c);
int diag224(void *ptr)
{
	int rc = -EOPNOTSUPP;

	asm volatile(
		"	diag	%1,%2,0x224\n"
		"0:	lhi	%0,0x0\n"
		"1:\n"
		EX_TABLE(0b,1b)
		: "+d" (rc) :"d" (0), "d" (ptr) : "memory");
	return rc;
}
EXPORT_SYMBOL(diag224);
