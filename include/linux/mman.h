/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_MMAN_H
#define _LINUX_MMAN_H

#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/percpu_counter.h>

#include <linux/atomic.h>
#include <uapi/linux/mman.h>

/*
 * Arrange for legacy / undefined architecture specific flags to be
 * ignored by mmap handling code.
 */
#ifndef MAP_32BIT
#define MAP_32BIT 0
#endif
#ifndef MAP_ABOVE4G
#define MAP_ABOVE4G 0
#endif
#ifndef MAP_HUGE_2MB
#define MAP_HUGE_2MB 0
#endif
#ifndef MAP_HUGE_1GB
#define MAP_HUGE_1GB 0
#endif
#ifndef MAP_UNINITIALIZED
#define MAP_UNINITIALIZED 0
#endif
#ifndef MAP_SYNC
#define MAP_SYNC 0
#endif

/*
 * The historical set of flags that all mmap implementations implicitly
 * support when a ->mmap_validate() op is not provided in file_operations.
 *
 * MAP_EXECUTABLE and MAP_DENYWRITE are completely ignored throughout the
 * kernel.
 */
#define LEGACY_MAP_MASK (MAP_SHARED \
		| MAP_PRIVATE \
		| MAP_FIXED \
		| MAP_ANONYMOUS \
		| MAP_DENYWRITE \
		| MAP_EXECUTABLE \
		| MAP_UNINITIALIZED \
		| MAP_GROWSDOWN \
		| MAP_LOCKED \
		| MAP_NORESERVE \
		| MAP_POPULATE \
		| MAP_NONBLOCK \
		| MAP_STACK \
		| MAP_HUGETLB \
		| MAP_32BIT \
		| MAP_ABOVE4G \
		| MAP_HUGE_2MB \
		| MAP_HUGE_1GB)

extern int sysctl_overcommit_memory;
extern struct percpu_counter vm_committed_as;

#ifdef CONFIG_SMP
extern s32 vm_committed_as_batch;
extern void mm_compute_batch(int overcommit_policy);
#else
#define vm_committed_as_batch 0
static inline void mm_compute_batch(int overcommit_policy)
{
}
#endif

unsigned long vm_memory_committed(void);

static inline void vm_acct_memory(long pages)
{
	percpu_counter_add_batch(&vm_committed_as, pages, vm_committed_as_batch);
}

static inline void vm_unacct_memory(long pages)
{
	vm_acct_memory(-pages);
}

/*
 * Allow architectures to handle additional protection and flag bits. The
 * overriding macros must be defined in the arch-specific asm/mman.h file.
 */

#ifndef arch_calc_vm_prot_bits
#define arch_calc_vm_prot_bits(prot, pkey) 0
#endif

#ifndef arch_calc_vm_flag_bits
#define arch_calc_vm_flag_bits(file, flags) 0
#endif

#ifndef arch_validate_prot
/*
 * This is called from mprotect().  PROT_GROWSDOWN and PROT_GROWSUP have
 * already been masked out.
 *
 * Returns true if the prot flags are valid
 */
static inline bool arch_validate_prot(unsigned long prot, unsigned long addr)
{
	return (prot & ~(PROT_READ | PROT_WRITE | PROT_EXEC | PROT_SEM)) == 0;
}
#define arch_validate_prot arch_validate_prot
#endif

#ifndef arch_validate_flags
/*
 * This is called from mmap() and mprotect() with the updated vma->vm_flags.
 *
 * Returns true if the VM_* flags are valid.
 */
static inline bool arch_validate_flags(unsigned long flags)
{
	return true;
}
#define arch_validate_flags arch_validate_flags
#endif

/*
 * Optimisation macro.  It is equivalent to:
 *      (x & bit1) ? bit2 : 0
 * but this version is faster.
 * ("bit1" and "bit2" must be single bits)
 */
#define _calc_vm_trans(x, bit1, bit2) \
  ((!(bit1) || !(bit2)) ? 0 : \
  ((bit1) <= (bit2) ? ((x) & (bit1)) * ((bit2) / (bit1)) \
   : ((x) & (bit1)) / ((bit1) / (bit2))))

/*
 * Combine the mmap "prot" argument into "vm_flags" used internally.
 */
static inline vm_flags_t
calc_vm_prot_bits(unsigned long prot, unsigned long pkey)
{
	return _calc_vm_trans(prot, PROT_READ,  VM_READ ) |
	       _calc_vm_trans(prot, PROT_WRITE, VM_WRITE) |
	       _calc_vm_trans(prot, PROT_EXEC,  VM_EXEC) |
	       arch_calc_vm_prot_bits(prot, pkey);
}

/*
 * Combine the mmap "flags" argument into "vm_flags" used internally.
 */
static inline vm_flags_t
calc_vm_flag_bits(struct file *file, unsigned long flags)
{
	return _calc_vm_trans(flags, MAP_GROWSDOWN,  VM_GROWSDOWN ) |
	       _calc_vm_trans(flags, MAP_LOCKED,     VM_LOCKED    ) |
	       _calc_vm_trans(flags, MAP_SYNC,	     VM_SYNC      ) |
#ifdef CONFIG_TRANSPARENT_HUGEPAGE
	       _calc_vm_trans(flags, MAP_STACK,	     VM_NOHUGEPAGE) |
#endif
	       arch_calc_vm_flag_bits(file, flags);
}

unsigned long vm_commit_limit(void);

#ifndef arch_memory_deny_write_exec_supported
static inline bool arch_memory_deny_write_exec_supported(void)
{
	return true;
}
#define arch_memory_deny_write_exec_supported arch_memory_deny_write_exec_supported
#endif

/*
 * Denies creating a writable executable mapping or gaining executable permissions.
 *
 * This denies the following:
 *
 * 	a)	mmap(PROT_WRITE | PROT_EXEC)
 *
 *	b)	mmap(PROT_WRITE)
 *		mprotect(PROT_EXEC)
 *
 *	c)	mmap(PROT_WRITE)
 *		mprotect(PROT_READ)
 *		mprotect(PROT_EXEC)
 *
 * But allows the following:
 *
 *	d)	mmap(PROT_READ | PROT_EXEC)
 *		mmap(PROT_READ | PROT_EXEC | PROT_BTI)
 *
 * This is only applicable if the user has set the Memory-Deny-Write-Execute
 * (MDWE) protection mask for the current process.
 *
 * @old specifies the VMA flags the VMA originally possessed, and @new the ones
 * we propose to set.
 *
 * Return: false if proposed change is OK, true if not ok and should be denied.
 */
static inline bool map_deny_write_exec(unsigned long old, unsigned long new)
{
	/* If MDWE is disabled, we have nothing to deny. */
	if (!test_bit(MMF_HAS_MDWE, &current->mm->flags))
		return false;

	/* If the new VMA is not executable, we have nothing to deny. */
	if (!(new & VM_EXEC))
		return false;

	/* Under MDWE we do not accept newly writably executable VMAs... */
	if (new & VM_WRITE)
		return true;

	/* ...nor previously non-executable VMAs becoming executable. */
	if (!(old & VM_EXEC))
		return true;

	return false;
}

#endif /* _LINUX_MMAN_H */
