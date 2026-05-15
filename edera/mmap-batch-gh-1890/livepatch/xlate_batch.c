#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/livepatch.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/pgtable.h>
#include <linux/sched.h>

#include <asm/xen/hypercall.h>
#include <asm/pgtable_types.h>

#include <xen/interface/memory.h>
#include <xen/page.h>
#include <xen/xen-ops.h>
#include <xen/xen.h>

// Linux keeps a separate copy of the xen public headers in its sources.

MODULE_LICENSE("GPL");
MODULE_INFO(livepatch, "Y");

#ifndef IN_KERNEL_BUILD
/*
 * MEMOP_EXTENT_SHIFT is the bit position Xen uses to pack the "resume from
 * here" hint into the hypercall command word. The hypervisor reads it back out
 * via start_extent and skips already-processed entries.
 */
#ifndef MEMOP_EXTENT_SHIFT
#define MEMOP_EXTENT_SHIFT 6
#endif
#endif /* !IN_KERNEL_BUILD */

#define XLATE_BATCH_SIZE 1

#ifdef IN_KERNEL_BUILD
struct xlate_setup {
	xen_ulong_t *idxs;
	xen_pfn_t *gpfns;
	int *errs;
	xen_pfn_t *fgfn;
	unsigned int iter;
};

static void xlate_setup_one(unsigned long gfn, void *data)
{
	struct xlate_setup *s = data;
	s->idxs[s->iter] = *s->fgfn;
	s->gpfns[s->iter] = gfn;
	s->errs[s->iter] = 0;
	s->fgfn++;
	s->iter++;
}
#endif /* IN_KERNEL_BUILD */

struct lp_remap_pfn {
	struct mm_struct *mm;
	struct page **pages;
	pgprot_t prot;
	unsigned long i;
};

static int lp_remap_pfn_fn(pte_t *ptep, unsigned long addr, void *data)
{
	struct lp_remap_pfn *r = data;
	printk(KERN_INFO "%s addr %#lx", __func__, addr);
	pte_t pte =
		pte_mkspecial(pfn_pte(page_to_pfn(r->pages[r->i++]), r->prot));
	set_pte_at(r->mm, addr, ptep, pte);
	return 0;
}

/*
 * vma:   user area created by toolstack to map foreign domain memory into
 * addr:  starting vaddr in vma
 * nr:    count of source pages
 * gfn:   foreign GFNs[nr]
 * domid: foreign domain to map memory from
 * pages: array of struct page referencing 4Ki slot in dom0's GFN space
 */
static int lp_xen_xlate_remap_gfn_array(struct vm_area_struct *vma,
					unsigned long addr, xen_pfn_t *gfn,
					int nr, int *err_ptr, pgprot_t prot,
					unsigned domid, struct page **pages)
{
	printk(KERN_INFO "%s", __func__);

	/*
         * 1. Build h_idxs[] (foreign GFNs from gfn[]) and h_gpfns[] (dom0 GFNs from pages[] via page_to_xen_pfn)
         * 2. Issue the hypercall over those batch entries
         * 3. Call apply_to_page_range over batch pages of address space, with a callback that only writes PTEs
         * 4. Propagate per-entry errors into err_ptr[]
         * 5. Advance addr, offset, decrement nr, cond_resched()
         */

	/* Source GFNs in foreign domain P2M (combined with XENMAPSPACE_gmfn_foreign) */
	xen_ulong_t h_idxs[XLATE_BATCH_SIZE];

	/* Destination GFNs in dom0's P2M. Extracted from pages[] */
	xen_pfn_t h_gpfns[XLATE_BATCH_SIZE];

	/* h_idxs and h_gpfns exist as pairs: we map into dom0's GFN the same MFN for the GFN in foreign domain. */

	int h_errs[XLATE_BATCH_SIZE];
	int mapped = 0;
	int gfn_off = 0; /* index into gfn/err */
	int page_off = 0; /* index into pages */

	BUILD_BUG_ON(XLATE_BATCH_SIZE % XEN_PFN_PER_PAGE != 0);
	BUG_ON(!((vma->vm_flags & (VM_PFNMAP | VM_IO)) == (VM_PFNMAP | VM_IO)));

	printk(KERN_INFO "%s: nr %d", __func__, nr);

	while (nr > 0) {
		int batch = min(XLATE_BATCH_SIZE, nr);
		printk(KERN_INFO "%s: batch %d", __func__, batch);

		/* arm: page size may be larger than xen */
		int batch_pages = DIV_ROUND_UP(batch, XEN_PFN_PER_PAGE);
		printk(KERN_INFO "%s: batch_pages %d", __func__, batch);

		unsigned long batch_range = (unsigned long)batch_pages
					    << PAGE_SHIFT;
		printk(KERN_INFO "%s: batch_range %lu", __func__, batch_range);
		unsigned int start_extent = 0;

		int rc, i;

		/* set up arguments to hyeprcall to edit dom0 P2M */

#if IN_KERNEL_BUILD
		struct xlate_setup s = {
			.idxs = h_idxs,
			.gpfns = h_gpfns,
			.errs = h_errs,
			.fgfn = &gfn[gfn_off],
			.iter = 0,
		};

		xen_for_each_gfn(pages + page_off, (unsigned)batch,
				 xlate_setup_one, &s);
#else /* as livepatch */
		unsigned long xen_pfn = 0;

		/* for each _source_ page in the batch (arm: pages may be larger than 4Ki) */
		for (i = 0; i < batch; i++) {
			/* dom0 struct page -> xen PFN -> GFN */
			if ((i % XEN_PFN_PER_PAGE) == 0) {
				xen_pfn = page_to_xen_pfn(
					pages[page_off + i / XEN_PFN_PER_PAGE]);
				printk(KERN_INFO "%s: i %d xen_pfn %#lx",
				       __func__, i, xen_pfn);
			}
			h_idxs[i] = gfn[gfn_off + i];
			h_gpfns[i] = pfn_to_gfn(xen_pfn++);
			h_errs[i] = 0;
		}
#endif /* as livepatch */

		struct xen_add_to_physmap_range xatp = {
			.domid = DOMID_SELF,
			.foreign_domid = domid,
			.space = XENMAPSPACE_gmfn_foreign,
			.size = batch,
		};

		set_xen_guest_handle(xatp.idxs, h_idxs);
		set_xen_guest_handle(xatp.gpfns, h_gpfns);
		set_xen_guest_handle(xatp.errs, h_errs);

		do {
			rc = HYPERVISOR_memory_op(
				XENMEM_add_to_physmap_range |
					(start_extent << MEMOP_EXTENT_SHIFT),
				&xatp);
			printk(KERN_INFO
			       "%s: XENMEM_add_to_physmap_range rc %d",
			       __func__, rc);
			if (rc > 0)
				start_extent = rc;
		} while (rc > 0);

		printk(KERN_INFO "%s: P2M done", __func__);

		struct lp_remap_pfn r = {
			.mm = vma->vm_mm,
			.pages = &pages[page_off],
			.prot = prot,
			.i = 0,
		};
		apply_to_page_range(vma->vm_mm, addr, batch_range,
				    lp_remap_pfn_fn, &r);

		printk(KERN_INFO "%s: PT done", __func__);

		for (i = 0; i < batch; i++) {
			int err = (rc < 0) ? rc : h_errs[i];
			err_ptr[gfn_off + i] = err;
			if (!err)
				mapped++;
		}

		addr += batch_range;
		gfn_off += batch;
		page_off += batch_pages;
		nr -= batch;
		cond_resched();
	}

	printk(KERN_INFO "%s: mapped %d", __func__, mapped);
	return mapped;
}

static struct klp_func funcs[] = {
	{
		.old_name = "xen_xlate_remap_gfn_array",
		.new_func = lp_xen_xlate_remap_gfn_array,
	},
	{}
};

static struct klp_object objs[] = { {
					    .funcs = funcs,
				    },
				    {} };

static struct klp_patch patch = {
	.mod = THIS_MODULE,
	.objs = objs,
};

static int lp_init(void)
{
	return klp_enable_patch(&patch);
}
static void lp_exit(void)
{
}

module_init(lp_init);
module_exit(lp_exit);
