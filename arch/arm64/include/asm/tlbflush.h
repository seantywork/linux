/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Based on arch/arm/include/asm/tlbflush.h
 *
 * Copyright (C) 1999-2003 Russell King
 * Copyright (C) 2012 ARM Ltd.
 */
#ifndef __ASM_TLBFLUSH_H
#define __ASM_TLBFLUSH_H

#ifndef __ASSEMBLY__

#include <linux/bitfield.h>
#include <linux/mm_types.h>
#include <linux/sched.h>
#include <linux/mmu_notifier.h>
#include <asm/cputype.h>
#include <asm/mmu.h>

/*
 * Raw TLBI operations.
 *
 * Where necessary, use the __tlbi() macro to avoid asm()
 * boilerplate. Drivers and most kernel code should use the TLB
 * management routines in preference to the macro below.
 *
 * The macro can be used as __tlbi(op) or __tlbi(op, arg), depending
 * on whether a particular TLBI operation takes an argument or
 * not. The macros handles invoking the asm with or without the
 * register argument as appropriate.
 */
#define __TLBI_0(op, arg) asm (ARM64_ASM_PREAMBLE			       \
			       "tlbi " #op "\n"				       \
		   ALTERNATIVE("nop\n			nop",		       \
			       "dsb ish\n		tlbi " #op,	       \
			       ARM64_WORKAROUND_REPEAT_TLBI,		       \
			       CONFIG_ARM64_WORKAROUND_REPEAT_TLBI)	       \
			    : : )

#define __TLBI_1(op, arg) asm (ARM64_ASM_PREAMBLE			       \
			       "tlbi " #op ", %0\n"			       \
		   ALTERNATIVE("nop\n			nop",		       \
			       "dsb ish\n		tlbi " #op ", %0",     \
			       ARM64_WORKAROUND_REPEAT_TLBI,		       \
			       CONFIG_ARM64_WORKAROUND_REPEAT_TLBI)	       \
			    : : "r" (arg))

#define __TLBI_N(op, arg, n, ...) __TLBI_##n(op, arg)

#define __tlbi(op, ...)		__TLBI_N(op, ##__VA_ARGS__, 1, 0)

#define __tlbi_user(op, arg) do {						\
	if (arm64_kernel_unmapped_at_el0())					\
		__tlbi(op, (arg) | USER_ASID_FLAG);				\
} while (0)

/* This macro creates a properly formatted VA operand for the TLBI */
#define __TLBI_VADDR(addr, asid)				\
	({							\
		unsigned long __ta = (addr) >> 12;		\
		__ta &= GENMASK_ULL(43, 0);			\
		__ta |= (unsigned long)(asid) << 48;		\
		__ta;						\
	})

/*
 * Get translation granule of the system, which is decided by
 * PAGE_SIZE.  Used by TTL.
 *  - 4KB	: 1
 *  - 16KB	: 2
 *  - 64KB	: 3
 */
#define TLBI_TTL_TG_4K		1
#define TLBI_TTL_TG_16K		2
#define TLBI_TTL_TG_64K		3

static inline unsigned long get_trans_granule(void)
{
	switch (PAGE_SIZE) {
	case SZ_4K:
		return TLBI_TTL_TG_4K;
	case SZ_16K:
		return TLBI_TTL_TG_16K;
	case SZ_64K:
		return TLBI_TTL_TG_64K;
	default:
		return 0;
	}
}

/*
 * Level-based TLBI operations.
 *
 * When ARMv8.4-TTL exists, TLBI operations take an additional hint for
 * the level at which the invalidation must take place. If the level is
 * wrong, no invalidation may take place. In the case where the level
 * cannot be easily determined, the value TLBI_TTL_UNKNOWN will perform
 * a non-hinted invalidation. Any provided level outside the hint range
 * will also cause fall-back to non-hinted invalidation.
 *
 * For Stage-2 invalidation, use the level values provided to that effect
 * in asm/stage2_pgtable.h.
 */
#define TLBI_TTL_MASK		GENMASK_ULL(47, 44)

#define TLBI_TTL_UNKNOWN	INT_MAX

#define __tlbi_level(op, addr, level) do {				\
	u64 arg = addr;							\
									\
	if (alternative_has_cap_unlikely(ARM64_HAS_ARMv8_4_TTL) &&	\
	    level >= 0 && level <= 3) {					\
		u64 ttl = level & 3;					\
		ttl |= get_trans_granule() << 2;			\
		arg &= ~TLBI_TTL_MASK;					\
		arg |= FIELD_PREP(TLBI_TTL_MASK, ttl);			\
	}								\
									\
	__tlbi(op, arg);						\
} while(0)

#define __tlbi_user_level(op, arg, level) do {				\
	if (arm64_kernel_unmapped_at_el0())				\
		__tlbi_level(op, (arg | USER_ASID_FLAG), level);	\
} while (0)

/*
 * This macro creates a properly formatted VA operand for the TLB RANGE. The
 * value bit assignments are:
 *
 * +----------+------+-------+-------+-------+----------------------+
 * |   ASID   |  TG  | SCALE |  NUM  |  TTL  |        BADDR         |
 * +-----------------+-------+-------+-------+----------------------+
 * |63      48|47  46|45   44|43   39|38   37|36                   0|
 *
 * The address range is determined by below formula: [BADDR, BADDR + (NUM + 1) *
 * 2^(5*SCALE + 1) * PAGESIZE)
 *
 * Note that the first argument, baddr, is pre-shifted; If LPA2 is in use, BADDR
 * holds addr[52:16]. Else BADDR holds page number. See for example ARM DDI
 * 0487J.a section C5.5.60 "TLBI VAE1IS, TLBI VAE1ISNXS, TLB Invalidate by VA,
 * EL1, Inner Shareable".
 *
 */
#define TLBIR_ASID_MASK		GENMASK_ULL(63, 48)
#define TLBIR_TG_MASK		GENMASK_ULL(47, 46)
#define TLBIR_SCALE_MASK	GENMASK_ULL(45, 44)
#define TLBIR_NUM_MASK		GENMASK_ULL(43, 39)
#define TLBIR_TTL_MASK		GENMASK_ULL(38, 37)
#define TLBIR_BADDR_MASK	GENMASK_ULL(36,  0)

#define __TLBI_VADDR_RANGE(baddr, asid, scale, num, ttl)		\
	({								\
		unsigned long __ta = 0;					\
		unsigned long __ttl = (ttl >= 1 && ttl <= 3) ? ttl : 0;	\
		__ta |= FIELD_PREP(TLBIR_BADDR_MASK, baddr);		\
		__ta |= FIELD_PREP(TLBIR_TTL_MASK, __ttl);		\
		__ta |= FIELD_PREP(TLBIR_NUM_MASK, num);		\
		__ta |= FIELD_PREP(TLBIR_SCALE_MASK, scale);		\
		__ta |= FIELD_PREP(TLBIR_TG_MASK, get_trans_granule());	\
		__ta |= FIELD_PREP(TLBIR_ASID_MASK, asid);		\
		__ta;							\
	})

/* These macros are used by the TLBI RANGE feature. */
#define __TLBI_RANGE_PAGES(num, scale)	\
	((unsigned long)((num) + 1) << (5 * (scale) + 1))
#define MAX_TLBI_RANGE_PAGES		__TLBI_RANGE_PAGES(31, 3)

/*
 * Generate 'num' values from -1 to 31 with -1 rejected by the
 * __flush_tlb_range() loop below. Its return value is only
 * significant for a maximum of MAX_TLBI_RANGE_PAGES pages. If
 * 'pages' is more than that, you must iterate over the overall
 * range.
 */
#define __TLBI_RANGE_NUM(pages, scale)					\
	({								\
		int __pages = min((pages),				\
				  __TLBI_RANGE_PAGES(31, (scale)));	\
		(__pages >> (5 * (scale) + 1)) - 1;			\
	})

/*
 *	TLB Invalidation
 *	================
 *
 * 	This header file implements the low-level TLB invalidation routines
 *	(sometimes referred to as "flushing" in the kernel) for arm64.
 *
 *	Every invalidation operation uses the following template:
 *
 *	DSB ISHST	// Ensure prior page-table updates have completed
 *	TLBI ...	// Invalidate the TLB
 *	DSB ISH		// Ensure the TLB invalidation has completed
 *      if (invalidated kernel mappings)
 *		ISB	// Discard any instructions fetched from the old mapping
 *
 *
 *	The following functions form part of the "core" TLB invalidation API,
 *	as documented in Documentation/core-api/cachetlb.rst:
 *
 *	flush_tlb_all()
 *		Invalidate the entire TLB (kernel + user) on all CPUs
 *
 *	flush_tlb_mm(mm)
 *		Invalidate an entire user address space on all CPUs.
 *		The 'mm' argument identifies the ASID to invalidate.
 *
 *	flush_tlb_range(vma, start, end)
 *		Invalidate the virtual-address range '[start, end)' on all
 *		CPUs for the user address space corresponding to 'vma->mm'.
 *		Note that this operation also invalidates any walk-cache
 *		entries associated with translations for the specified address
 *		range.
 *
 *	flush_tlb_kernel_range(start, end)
 *		Same as flush_tlb_range(..., start, end), but applies to
 * 		kernel mappings rather than a particular user address space.
 *		Whilst not explicitly documented, this function is used when
 *		unmapping pages from vmalloc/io space.
 *
 *	flush_tlb_page(vma, addr)
 *		Invalidate a single user mapping for address 'addr' in the
 *		address space corresponding to 'vma->mm'.  Note that this
 *		operation only invalidates a single, last-level page-table
 *		entry and therefore does not affect any walk-caches.
 *
 *
 *	Next, we have some undocumented invalidation routines that you probably
 *	don't want to call unless you know what you're doing:
 *
 *	local_flush_tlb_all()
 *		Same as flush_tlb_all(), but only applies to the calling CPU.
 *
 *	__flush_tlb_kernel_pgtable(addr)
 *		Invalidate a single kernel mapping for address 'addr' on all
 *		CPUs, ensuring that any walk-cache entries associated with the
 *		translation are also invalidated.
 *
 *	__flush_tlb_range(vma, start, end, stride, last_level, tlb_level)
 *		Invalidate the virtual-address range '[start, end)' on all
 *		CPUs for the user address space corresponding to 'vma->mm'.
 *		The invalidation operations are issued at a granularity
 *		determined by 'stride' and only affect any walk-cache entries
 *		if 'last_level' is equal to false. tlb_level is the level at
 *		which the invalidation must take place. If the level is wrong,
 *		no invalidation may take place. In the case where the level
 *		cannot be easily determined, the value TLBI_TTL_UNKNOWN will
 *		perform a non-hinted invalidation.
 *
 *
 *	Finally, take a look at asm/tlb.h to see how tlb_flush() is implemented
 *	on top of these routines, since that is our interface to the mmu_gather
 *	API as used by munmap() and friends.
 */
static inline void local_flush_tlb_all(void)
{
	dsb(nshst);
	__tlbi(vmalle1);
	dsb(nsh);
	isb();
}

static inline void flush_tlb_all(void)
{
	dsb(ishst);
	__tlbi(vmalle1is);
	dsb(ish);
	isb();
}

static inline void flush_tlb_mm(struct mm_struct *mm)
{
	unsigned long asid;

	dsb(ishst);
	asid = __TLBI_VADDR(0, ASID(mm));
	__tlbi(aside1is, asid);
	__tlbi_user(aside1is, asid);
	dsb(ish);
	mmu_notifier_arch_invalidate_secondary_tlbs(mm, 0, -1UL);
}

static inline void __flush_tlb_page_nosync(struct mm_struct *mm,
					   unsigned long uaddr)
{
	unsigned long addr;

	dsb(ishst);
	addr = __TLBI_VADDR(uaddr, ASID(mm));
	__tlbi(vale1is, addr);
	__tlbi_user(vale1is, addr);
	mmu_notifier_arch_invalidate_secondary_tlbs(mm, uaddr & PAGE_MASK,
						(uaddr & PAGE_MASK) + PAGE_SIZE);
}

static inline void flush_tlb_page_nosync(struct vm_area_struct *vma,
					 unsigned long uaddr)
{
	return __flush_tlb_page_nosync(vma->vm_mm, uaddr);
}

static inline void flush_tlb_page(struct vm_area_struct *vma,
				  unsigned long uaddr)
{
	flush_tlb_page_nosync(vma, uaddr);
	dsb(ish);
}

static inline bool arch_tlbbatch_should_defer(struct mm_struct *mm)
{
	/*
	 * TLB flush deferral is not required on systems which are affected by
	 * ARM64_WORKAROUND_REPEAT_TLBI, as __tlbi()/__tlbi_user() implementation
	 * will have two consecutive TLBI instructions with a dsb(ish) in between
	 * defeating the purpose (i.e save overall 'dsb ish' cost).
	 */
	if (alternative_has_cap_unlikely(ARM64_WORKAROUND_REPEAT_TLBI))
		return false;

	return true;
}

/*
 * To support TLB batched flush for multiple pages unmapping, we only send
 * the TLBI for each page in arch_tlbbatch_add_pending() and wait for the
 * completion at the end in arch_tlbbatch_flush(). Since we've already issued
 * TLBI for each page so only a DSB is needed to synchronise its effect on the
 * other CPUs.
 *
 * This will save the time waiting on DSB comparing issuing a TLBI;DSB sequence
 * for each page.
 */
static inline void arch_tlbbatch_flush(struct arch_tlbflush_unmap_batch *batch)
{
	dsb(ish);
}

/*
 * This is meant to avoid soft lock-ups on large TLB flushing ranges and not
 * necessarily a performance improvement.
 */
#define MAX_DVM_OPS	PTRS_PER_PTE

/*
 * __flush_tlb_range_op - Perform TLBI operation upon a range
 *
 * @op:	TLBI instruction that operates on a range (has 'r' prefix)
 * @start:	The start address of the range
 * @pages:	Range as the number of pages from 'start'
 * @stride:	Flush granularity
 * @asid:	The ASID of the task (0 for IPA instructions)
 * @tlb_level:	Translation Table level hint, if known
 * @tlbi_user:	If 'true', call an additional __tlbi_user()
 *              (typically for user ASIDs). 'flase' for IPA instructions
 * @lpa2:	If 'true', the lpa2 scheme is used as set out below
 *
 * When the CPU does not support TLB range operations, flush the TLB
 * entries one by one at the granularity of 'stride'. If the TLB
 * range ops are supported, then:
 *
 * 1. If FEAT_LPA2 is in use, the start address of a range operation must be
 *    64KB aligned, so flush pages one by one until the alignment is reached
 *    using the non-range operations. This step is skipped if LPA2 is not in
 *    use.
 *
 * 2. The minimum range granularity is decided by 'scale', so multiple range
 *    TLBI operations may be required. Start from scale = 3, flush the largest
 *    possible number of pages ((num+1)*2^(5*scale+1)) that fit into the
 *    requested range, then decrement scale and continue until one or zero pages
 *    are left. We must start from highest scale to ensure 64KB start alignment
 *    is maintained in the LPA2 case.
 *
 * 3. If there is 1 page remaining, flush it through non-range operations. Range
 *    operations can only span an even number of pages. We save this for last to
 *    ensure 64KB start alignment is maintained for the LPA2 case.
 */
#define __flush_tlb_range_op(op, start, pages, stride,			\
				asid, tlb_level, tlbi_user, lpa2)	\
do {									\
	typeof(start) __flush_start = start;				\
	typeof(pages) __flush_pages = pages;				\
	int num = 0;							\
	int scale = 3;							\
	int shift = lpa2 ? 16 : PAGE_SHIFT;				\
	unsigned long addr;						\
									\
	while (__flush_pages > 0) {					\
		if (!system_supports_tlb_range() ||			\
		    __flush_pages == 1 ||				\
		    (lpa2 && __flush_start != ALIGN(__flush_start, SZ_64K))) {	\
			addr = __TLBI_VADDR(__flush_start, asid);	\
			__tlbi_level(op, addr, tlb_level);		\
			if (tlbi_user)					\
				__tlbi_user_level(op, addr, tlb_level);	\
			__flush_start += stride;			\
			__flush_pages -= stride >> PAGE_SHIFT;		\
			continue;					\
		}							\
									\
		num = __TLBI_RANGE_NUM(__flush_pages, scale);		\
		if (num >= 0) {						\
			addr = __TLBI_VADDR_RANGE(__flush_start >> shift, asid, \
						scale, num, tlb_level);	\
			__tlbi(r##op, addr);				\
			if (tlbi_user)					\
				__tlbi_user(r##op, addr);		\
			__flush_start += __TLBI_RANGE_PAGES(num, scale) << PAGE_SHIFT; \
			__flush_pages -= __TLBI_RANGE_PAGES(num, scale);\
		}							\
		scale--;						\
	}								\
} while (0)

#define __flush_s2_tlb_range_op(op, start, pages, stride, tlb_level) \
	__flush_tlb_range_op(op, start, pages, stride, 0, tlb_level, false, kvm_lpa2_is_enabled());

static inline bool __flush_tlb_range_limit_excess(unsigned long start,
		unsigned long end, unsigned long pages, unsigned long stride)
{
	/*
	 * When the system does not support TLB range based flush
	 * operation, (MAX_DVM_OPS - 1) pages can be handled. But
	 * with TLB range based operation, MAX_TLBI_RANGE_PAGES
	 * pages can be handled.
	 */
	if ((!system_supports_tlb_range() &&
	     (end - start) >= (MAX_DVM_OPS * stride)) ||
	    pages > MAX_TLBI_RANGE_PAGES)
		return true;

	return false;
}

static inline void __flush_tlb_range_nosync(struct mm_struct *mm,
				     unsigned long start, unsigned long end,
				     unsigned long stride, bool last_level,
				     int tlb_level)
{
	unsigned long asid, pages;

	start = round_down(start, stride);
	end = round_up(end, stride);
	pages = (end - start) >> PAGE_SHIFT;

	if (__flush_tlb_range_limit_excess(start, end, pages, stride)) {
		flush_tlb_mm(mm);
		return;
	}

	dsb(ishst);
	asid = ASID(mm);

	if (last_level)
		__flush_tlb_range_op(vale1is, start, pages, stride, asid,
				     tlb_level, true, lpa2_is_enabled());
	else
		__flush_tlb_range_op(vae1is, start, pages, stride, asid,
				     tlb_level, true, lpa2_is_enabled());

	mmu_notifier_arch_invalidate_secondary_tlbs(mm, start, end);
}

static inline void __flush_tlb_range(struct vm_area_struct *vma,
				     unsigned long start, unsigned long end,
				     unsigned long stride, bool last_level,
				     int tlb_level)
{
	__flush_tlb_range_nosync(vma->vm_mm, start, end, stride,
				 last_level, tlb_level);
	dsb(ish);
}

static inline void flush_tlb_range(struct vm_area_struct *vma,
				   unsigned long start, unsigned long end)
{
	/*
	 * We cannot use leaf-only invalidation here, since we may be invalidating
	 * table entries as part of collapsing hugepages or moving page tables.
	 * Set the tlb_level to TLBI_TTL_UNKNOWN because we can not get enough
	 * information here.
	 */
	__flush_tlb_range(vma, start, end, PAGE_SIZE, false, TLBI_TTL_UNKNOWN);
}

static inline void flush_tlb_kernel_range(unsigned long start, unsigned long end)
{
	const unsigned long stride = PAGE_SIZE;
	unsigned long pages;

	start = round_down(start, stride);
	end = round_up(end, stride);
	pages = (end - start) >> PAGE_SHIFT;

	if (__flush_tlb_range_limit_excess(start, end, pages, stride)) {
		flush_tlb_all();
		return;
	}

	dsb(ishst);
	__flush_tlb_range_op(vaale1is, start, pages, stride, 0,
			     TLBI_TTL_UNKNOWN, false, lpa2_is_enabled());
	dsb(ish);
	isb();
}

/*
 * Used to invalidate the TLB (walk caches) corresponding to intermediate page
 * table levels (pgd/pud/pmd).
 */
static inline void __flush_tlb_kernel_pgtable(unsigned long kaddr)
{
	unsigned long addr = __TLBI_VADDR(kaddr, 0);

	dsb(ishst);
	__tlbi(vaae1is, addr);
	dsb(ish);
	isb();
}

static inline void arch_tlbbatch_add_pending(struct arch_tlbflush_unmap_batch *batch,
		struct mm_struct *mm, unsigned long start, unsigned long end)
{
	__flush_tlb_range_nosync(mm, start, end, PAGE_SIZE, true, 3);
}
#endif

#endif
