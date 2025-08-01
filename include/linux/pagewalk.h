/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_PAGEWALK_H
#define _LINUX_PAGEWALK_H

#include <linux/mm.h>

struct mm_walk;

/* Locking requirement during a page walk. */
enum page_walk_lock {
	/* mmap_lock should be locked for read to stabilize the vma tree */
	PGWALK_RDLOCK = 0,
	/* vma will be write-locked during the walk */
	PGWALK_WRLOCK = 1,
	/* vma is expected to be already write-locked during the walk */
	PGWALK_WRLOCK_VERIFY = 2,
	/* vma is expected to be already read-locked during the walk */
	PGWALK_VMA_RDLOCK_VERIFY = 3,
};

/**
 * struct mm_walk_ops - callbacks for walk_page_range
 * @pgd_entry:		if set, called for each non-empty PGD (top-level) entry
 * @p4d_entry:		if set, called for each non-empty P4D entry
 * @pud_entry:		if set, called for each non-empty PUD entry
 * @pmd_entry:		if set, called for each non-empty PMD entry
 *			this handler is required to be able to handle
 *			pmd_trans_huge() pmds.  They may simply choose to
 *			split_huge_page() instead of handling it explicitly.
 * @pte_entry:		if set, called for each PTE (lowest-level) entry
 *			including empty ones, except if @install_pte is set.
 *			If @install_pte is set, @pte_entry is called only for
 *			existing PTEs.
 * @pte_hole:		if set, called for each hole at all levels,
 *			depth is -1 if not known, 0:PGD, 1:P4D, 2:PUD, 3:PMD.
 *			Any folded depths (where PTRS_PER_P?D is equal to 1)
 *			are skipped. If @install_pte is specified, this will
 *			not trigger for any populated ranges.
 * @hugetlb_entry:	if set, called for each hugetlb entry. This hook
 *			function is called with the vma lock held, in order to
 *			protect against a concurrent freeing of the pte_t* or
 *			the ptl. In some cases, the hook function needs to drop
 *			and retake the vma lock in order to avoid deadlocks
 *			while calling other functions. In such cases the hook
 *			function must either refrain from accessing the pte or
 *			ptl after dropping the vma lock, or else revalidate
 *			those items after re-acquiring the vma lock and before
 *			accessing them.
 * @test_walk:		caller specific callback function to determine whether
 *			we walk over the current vma or not. Returning 0 means
 *			"do page table walk over the current vma", returning
 *			a negative value means "abort current page table walk
 *			right now" and returning 1 means "skip the current vma"
 *			Note that this callback is not called when the caller
 *			passes in a single VMA as for walk_page_vma().
 * @pre_vma:            if set, called before starting walk on a non-null vma.
 * @post_vma:           if set, called after a walk on a non-null vma, provided
 *                      that @pre_vma and the vma walk succeeded.
 * @install_pte:        if set, missing page table entries are installed and
 *                      thus all levels are always walked in the specified
 *                      range. This callback is then invoked at the PTE level
 *                      (having split any THP pages prior), providing the PTE to
 *                      install. If allocations fail, the walk is aborted. This
 *                      operation is only available for userland memory. Not
 *                      usable for hugetlb ranges.
 *
 * p?d_entry callbacks are called even if those levels are folded on a
 * particular architecture/configuration.
 */
struct mm_walk_ops {
	int (*pgd_entry)(pgd_t *pgd, unsigned long addr,
			 unsigned long next, struct mm_walk *walk);
	int (*p4d_entry)(p4d_t *p4d, unsigned long addr,
			 unsigned long next, struct mm_walk *walk);
	int (*pud_entry)(pud_t *pud, unsigned long addr,
			 unsigned long next, struct mm_walk *walk);
	int (*pmd_entry)(pmd_t *pmd, unsigned long addr,
			 unsigned long next, struct mm_walk *walk);
	int (*pte_entry)(pte_t *pte, unsigned long addr,
			 unsigned long next, struct mm_walk *walk);
	int (*pte_hole)(unsigned long addr, unsigned long next,
			int depth, struct mm_walk *walk);
	int (*hugetlb_entry)(pte_t *pte, unsigned long hmask,
			     unsigned long addr, unsigned long next,
			     struct mm_walk *walk);
	int (*test_walk)(unsigned long addr, unsigned long next,
			struct mm_walk *walk);
	int (*pre_vma)(unsigned long start, unsigned long end,
		       struct mm_walk *walk);
	void (*post_vma)(struct mm_walk *walk);
	int (*install_pte)(unsigned long addr, unsigned long next,
			   pte_t *ptep, struct mm_walk *walk);
	enum page_walk_lock walk_lock;
};

/*
 * Action for pud_entry / pmd_entry callbacks.
 * ACTION_SUBTREE is the default
 */
enum page_walk_action {
	/* Descend to next level, splitting huge pages if needed and possible */
	ACTION_SUBTREE = 0,
	/* Continue to next entry at this level (ignoring any subtree) */
	ACTION_CONTINUE = 1,
	/* Call again for this entry */
	ACTION_AGAIN = 2
};

/**
 * struct mm_walk - walk_page_range data
 * @ops:	operation to call during the walk
 * @mm:		mm_struct representing the target process of page table walk
 * @pgd:	pointer to PGD; only valid with no_vma (otherwise set to NULL)
 * @vma:	vma currently walked (NULL if walking outside vmas)
 * @action:	next action to perform (see enum page_walk_action)
 * @no_vma:	walk ignoring vmas (vma will always be NULL)
 * @private:	private data for callbacks' usage
 *
 * (see the comment on walk_page_range() for more details)
 */
struct mm_walk {
	const struct mm_walk_ops *ops;
	struct mm_struct *mm;
	pgd_t *pgd;
	struct vm_area_struct *vma;
	enum page_walk_action action;
	bool no_vma;
	void *private;
};

int walk_page_range(struct mm_struct *mm, unsigned long start,
		unsigned long end, const struct mm_walk_ops *ops,
		void *private);
int walk_kernel_page_table_range(unsigned long start,
		unsigned long end, const struct mm_walk_ops *ops,
		pgd_t *pgd, void *private);
int walk_page_range_vma(struct vm_area_struct *vma, unsigned long start,
			unsigned long end, const struct mm_walk_ops *ops,
			void *private);
int walk_page_vma(struct vm_area_struct *vma, const struct mm_walk_ops *ops,
		void *private);
int walk_page_mapping(struct address_space *mapping, pgoff_t first_index,
		      pgoff_t nr, const struct mm_walk_ops *ops,
		      void *private);

typedef int __bitwise folio_walk_flags_t;

/*
 * Walk migration entries as well. Careful: a large folio might get split
 * concurrently.
 */
#define FW_MIGRATION			((__force folio_walk_flags_t)BIT(0))

/* Walk shared zeropages (small + huge) as well. */
#define FW_ZEROPAGE			((__force folio_walk_flags_t)BIT(1))

enum folio_walk_level {
	FW_LEVEL_PTE,
	FW_LEVEL_PMD,
	FW_LEVEL_PUD,
};

/**
 * struct folio_walk - folio_walk_start() / folio_walk_end() data
 * @page:	exact folio page referenced (if applicable)
 * @level:	page table level identifying the entry type
 * @pte:	pointer to the page table entry (FW_LEVEL_PTE).
 * @pmd:	pointer to the page table entry (FW_LEVEL_PMD).
 * @pud:	pointer to the page table entry (FW_LEVEL_PUD).
 * @ptl:	pointer to the page table lock.
 *
 * (see folio_walk_start() documentation for more details)
 */
struct folio_walk {
	/* public */
	struct page *page;
	enum folio_walk_level level;
	union {
		pte_t *ptep;
		pud_t *pudp;
		pmd_t *pmdp;
	};
	union {
		pte_t pte;
		pud_t pud;
		pmd_t pmd;
	};
	/* private */
	struct vm_area_struct *vma;
	spinlock_t *ptl;
};

struct folio *folio_walk_start(struct folio_walk *fw,
		struct vm_area_struct *vma, unsigned long addr,
		folio_walk_flags_t flags);

#define folio_walk_end(__fw, __vma) do { \
	spin_unlock((__fw)->ptl); \
	if (likely((__fw)->level == FW_LEVEL_PTE)) \
		pte_unmap((__fw)->ptep); \
	vma_pgtable_walk_end(__vma); \
} while (0)

#endif /* _LINUX_PAGEWALK_H */
