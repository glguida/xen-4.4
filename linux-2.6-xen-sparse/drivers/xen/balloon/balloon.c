/******************************************************************************
 * balloon.c
 *
 * Xen balloon driver - enables returning/claiming memory to/from Xen.
 *
 * Copyright (c) 2003, B Dragovic
 * Copyright (c) 2003-2004, M Williamson, K Fraser
 * Copyright (c) 2005 Dan M. Smith, IBM Corporation
 * 
 * This file may be distributed separately from the Linux kernel, or
 * incorporated into other software packages, subject to the following license:
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this source file (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use, copy, modify,
 * merge, publish, distribute, sublicense, and/or sell copies of the Software,
 * and to permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include <linux/config.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/errno.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/smp_lock.h>
#include <linux/pagemap.h>
#include <linux/bootmem.h>
#include <linux/highmem.h>
#include <linux/vmalloc.h>
#include <asm-xen/xen_proc.h>
#include <asm-xen/hypervisor.h>
#include <asm-xen/balloon.h>
#include <asm/pgalloc.h>
#include <asm/pgtable.h>
#include <asm/uaccess.h>
#include <asm/tlb.h>
#include <linux/list.h>

#include<asm-xen/xenbus.h>

#define PAGES2KB(_p) ((_p)<<(PAGE_SHIFT-10))

static struct proc_dir_entry *balloon_pde;

static DECLARE_MUTEX(balloon_mutex);
spinlock_t balloon_lock = SPIN_LOCK_UNLOCKED;

/* We aim for 'current allocation' == 'target allocation'. */
static unsigned long current_pages;
static unsigned long target_pages;

/* We may hit the hard limit in Xen. If we do then we remember it. */
static unsigned long hard_limit;

/*
 * Drivers may alter the memory reservation independently, but they must
 * inform the balloon driver so that we can avoid hitting the hard limit.
 */
static unsigned long driver_pages;

/* List of ballooned pages, threaded through the mem_map array. */
static LIST_HEAD(ballooned_pages);
static unsigned long balloon_low, balloon_high;

/* Main work function, always executed in process context. */
static void balloon_process(void *unused);
static DECLARE_WORK(balloon_worker, balloon_process, NULL);
static struct timer_list balloon_timer;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,0)
/* Use the private and mapping fields of struct page as a list. */
#define PAGE_TO_LIST(p) ((struct list_head *)&p->private)
#define LIST_TO_PAGE(l)				\
	(list_entry(((unsigned long *)l), struct page, private))
#define UNLIST_PAGE(p)				\
	do {					\
		list_del(PAGE_TO_LIST(p));	\
		p->mapping = NULL;		\
		p->private = 0;			\
	} while(0)
#else
/* There's a dedicated list field in struct page we can use.    */
#define PAGE_TO_LIST(p) ( &p->list )
#define LIST_TO_PAGE(l) ( list_entry(l, struct page, list) )
#define UNLIST_PAGE(p)  ( list_del(&p->list) )
#define pte_offset_kernel pte_offset
#define pud_t pgd_t
#define pud_offset(d, va) d
#define pud_none(d) 0
#define pud_bad(d) 0
#define subsys_initcall(_fn) __initcall(_fn)
#define pfn_to_page(_pfn) (mem_map + (_pfn))
#endif

#define IPRINTK(fmt, args...) \
	printk(KERN_INFO "xen_mem: " fmt, ##args)
#define WPRINTK(fmt, args...) \
	printk(KERN_WARNING "xen_mem: " fmt, ##args)

/* balloon_append: add the given page to the balloon. */
static void balloon_append(struct page *page)
{
	/* Lowmem is re-populated first, so highmem pages go at list tail. */
	if (PageHighMem(page)) {
		list_add_tail(PAGE_TO_LIST(page), &ballooned_pages);
		balloon_high++;
	} else {
		list_add(PAGE_TO_LIST(page), &ballooned_pages);
		balloon_low++;
	}
}

/* balloon_retrieve: rescue a page from the balloon, if it is not empty. */
static struct page *balloon_retrieve(void)
{
	struct page *page;

	if (list_empty(&ballooned_pages))
		return NULL;

	page = LIST_TO_PAGE(ballooned_pages.next);
	UNLIST_PAGE(page);

	if (PageHighMem(page))
		balloon_high--;
	else
		balloon_low--;

	return page;
}

static void balloon_alarm(unsigned long unused)
{
	schedule_work(&balloon_worker);
}

static unsigned long current_target(void)
{
	unsigned long target = min(target_pages, hard_limit);
	if (target > (current_pages + balloon_low + balloon_high))
		target = current_pages + balloon_low + balloon_high;
	return target;
}

/*
 * We avoid multiple worker processes conflicting via the balloon mutex.
 * We may of course race updates of the target counts (which are protected
 * by the balloon lock), or with changes to the Xen hard limit, but we will
 * recover from these in time.
 */
static void balloon_process(void *unused)
{
	unsigned long *mfn_list, pfn, i, flags;
	struct page   *page;
	long           credit, debt, rc;
	void          *v;

	down(&balloon_mutex);

 retry:
	mfn_list = NULL;

	if ((credit = current_target() - current_pages) > 0) {
		mfn_list = vmalloc(credit * sizeof(*mfn_list));
		if (mfn_list == NULL)
			goto out;

		balloon_lock(flags);
		rc = HYPERVISOR_dom_mem_op(
			MEMOP_increase_reservation, mfn_list, credit, 0);
		balloon_unlock(flags);
		if (rc < credit) {
			/* We hit the Xen hard limit: reprobe. */
			BUG_ON(HYPERVISOR_dom_mem_op(
				MEMOP_decrease_reservation,
				mfn_list, rc, 0) != rc);
			hard_limit = current_pages + rc - driver_pages;
			vfree(mfn_list);
			goto retry;
		}

		for (i = 0; i < credit; i++) {
			page = balloon_retrieve();
			BUG_ON(page == NULL);

			pfn = page - mem_map;
			if (phys_to_machine_mapping[pfn] != INVALID_P2M_ENTRY)
				BUG();

			/* Update P->M and M->P tables. */
			phys_to_machine_mapping[pfn] = mfn_list[i];
			xen_machphys_update(mfn_list[i], pfn);
            
			/* Link back into the page tables if not highmem. */
			if (pfn < max_low_pfn)
				BUG_ON(HYPERVISOR_update_va_mapping(
					(unsigned long)__va(pfn << PAGE_SHIFT),
					pfn_pte_ma(mfn_list[i], PAGE_KERNEL),
					0));

			/* Relinquish the page back to the allocator. */
			ClearPageReserved(page);
			set_page_count(page, 1);
			__free_page(page);
		}

		current_pages += credit;
	} else if (credit < 0) {
		debt = -credit;

		mfn_list = vmalloc(debt * sizeof(*mfn_list));
		if (mfn_list == NULL)
			goto out;

		for (i = 0; i < debt; i++) {
			if ((page = alloc_page(GFP_HIGHUSER)) == NULL) {
				debt = i;
				break;
			}

			pfn = page - mem_map;
			mfn_list[i] = phys_to_machine_mapping[pfn];

			if (!PageHighMem(page)) {
				v = phys_to_virt(pfn << PAGE_SHIFT);
				scrub_pages(v, 1);
				BUG_ON(HYPERVISOR_update_va_mapping(
					(unsigned long)v, __pte_ma(0), 0));
			}
#ifdef CONFIG_XEN_SCRUB_PAGES
			else {
				v = kmap(page);
				scrub_pages(v, 1);
				kunmap(page);
			}
#endif
		}

		/* Ensure that ballooned highmem pages don't have kmaps. */
		kmap_flush_unused();
		flush_tlb_all();

		/* No more mappings: invalidate P2M and add to balloon. */
		for (i = 0; i < debt; i++) {
			pfn = mfn_to_pfn(mfn_list[i]);
			phys_to_machine_mapping[pfn] = INVALID_P2M_ENTRY;
			balloon_append(pfn_to_page(pfn));
		}

		BUG_ON(HYPERVISOR_dom_mem_op(
			MEMOP_decrease_reservation,mfn_list, debt, 0) != debt);

		current_pages -= debt;
	}

 out:
	if (mfn_list != NULL)
		vfree(mfn_list);

	/* Schedule more work if there is some still to be done. */
	if (current_target() != current_pages)
		mod_timer(&balloon_timer, jiffies + HZ);

	up(&balloon_mutex);
}

/* Resets the Xen limit, sets new target, and kicks off processing. */
static void set_new_target(unsigned long target)
{
	/* No need for lock. Not read-modify-write updates. */
	hard_limit   = ~0UL;
	target_pages = target;
	schedule_work(&balloon_worker);
}

static struct xenbus_watch target_watch =
{
	.node = "memory/target"
};

/* React to a change in the target key */
static void watch_target(struct xenbus_watch *watch, const char *node)
{
	unsigned long new_target;
	int err;

	err = xenbus_scanf("memory", "target", "%lu", &new_target);
	if (err != 1) {
		printk(KERN_ERR "Unable to read memory/target\n");
		return;
	} 
        
	set_new_target(new_target >> PAGE_SHIFT);
    
}

/* Setup our watcher
   NB: Assumes xenbus_lock is held!
*/
int balloon_init_watcher(struct notifier_block *notifier,
                         unsigned long event,
                         void *data)
{
	int err;

	BUG_ON(down_trylock(&xenbus_lock) == 0);

	err = register_xenbus_watch(&target_watch);
	if (err)
		printk(KERN_ERR "Failed to set balloon watcher\n");

	return NOTIFY_DONE;
    
}

static int balloon_write(struct file *file, const char __user *buffer,
                         unsigned long count, void *data)
{
	char memstring[64], *endchar;
	unsigned long long target_bytes;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	if (count <= 1)
		return -EBADMSG; /* runt */
	if (count > sizeof(memstring))
		return -EFBIG;   /* too long */

	if (copy_from_user(memstring, buffer, count))
		return -EFAULT;
	memstring[sizeof(memstring)-1] = '\0';

	target_bytes = memparse(memstring, &endchar);
	set_new_target(target_bytes >> PAGE_SHIFT);

	return count;
}

static int balloon_read(char *page, char **start, off_t off,
                        int count, int *eof, void *data)
{
	int len;

	len = sprintf(
		page,
		"Current allocation: %8lu kB\n"
		"Requested target:   %8lu kB\n"
		"Low-mem balloon:    %8lu kB\n"
		"High-mem balloon:   %8lu kB\n"
		"Xen hard limit:     ",
		PAGES2KB(current_pages), PAGES2KB(target_pages), 
		PAGES2KB(balloon_low), PAGES2KB(balloon_high));

	if (hard_limit != ~0UL) {
		len += sprintf(
			page + len, 
			"%8lu kB (inc. %8lu kB driver headroom)\n",
			PAGES2KB(hard_limit), PAGES2KB(driver_pages));
	} else {
		len += sprintf(
			page + len,
			"     ??? kB\n");
	}

	*eof = 1;
	return len;
}

static struct notifier_block xenstore_notifier;

static int __init balloon_init(void)
{
	unsigned long pfn;
	struct page *page;

	IPRINTK("Initialising balloon driver.\n");

	current_pages = min(xen_start_info.nr_pages, max_pfn);
	target_pages  = current_pages;
	balloon_low   = 0;
	balloon_high  = 0;
	driver_pages  = 0UL;
	hard_limit    = ~0UL;

	init_timer(&balloon_timer);
	balloon_timer.data = 0;
	balloon_timer.function = balloon_alarm;
    
	if ((balloon_pde = create_xen_proc_entry("balloon", 0644)) == NULL) {
		WPRINTK("Unable to create /proc/xen/balloon.\n");
		return -1;
	}

	balloon_pde->read_proc  = balloon_read;
	balloon_pde->write_proc = balloon_write;
    
	/* Initialise the balloon with excess memory space. */
	for (pfn = xen_start_info.nr_pages; pfn < max_pfn; pfn++) {
		page = &mem_map[pfn];
		if (!PageReserved(page))
			balloon_append(page);
	}

	target_watch.callback = watch_target;
	xenstore_notifier.notifier_call = balloon_init_watcher;

	register_xenstore_notifier(&xenstore_notifier);
    
	return 0;
}

subsys_initcall(balloon_init);

void balloon_update_driver_allowance(long delta)
{
	unsigned long flags;
	balloon_lock(flags);
	driver_pages += delta; /* non-atomic update */
	balloon_unlock(flags);
}

static int dealloc_pte_fn(
	pte_t *pte, struct page *pte_page, unsigned long addr, void *data)
{
	unsigned long mfn = pte_mfn(*pte);
	set_pte(pte, __pte_ma(0));
	phys_to_machine_mapping[__pa(addr) >> PAGE_SHIFT] =
		INVALID_P2M_ENTRY;
	BUG_ON(HYPERVISOR_dom_mem_op(
		MEMOP_decrease_reservation, &mfn, 1, 0) != 1);
	return 0;
}

struct page *balloon_alloc_empty_page_range(unsigned long nr_pages)
{
	unsigned long vstart, flags;
	unsigned int  order = get_order(nr_pages * PAGE_SIZE);

	vstart = __get_free_pages(GFP_KERNEL, order);
	if (vstart == 0)
		return NULL;

	scrub_pages(vstart, 1 << order);

	balloon_lock(flags);
	BUG_ON(generic_page_range(
		&init_mm, vstart, PAGE_SIZE << order, dealloc_pte_fn, NULL));
	current_pages -= 1UL << order;
	balloon_unlock(flags);

	schedule_work(&balloon_worker);

	flush_tlb_all();

	return virt_to_page(vstart);
}

void balloon_dealloc_empty_page_range(
	struct page *page, unsigned long nr_pages)
{
	unsigned long i, flags;
	unsigned int  order = get_order(nr_pages * PAGE_SIZE);

	balloon_lock(flags);
	for (i = 0; i < (1UL << order); i++)
		balloon_append(page + i);
	balloon_unlock(flags);

	schedule_work(&balloon_worker);
}

EXPORT_SYMBOL(balloon_update_driver_allowance);
EXPORT_SYMBOL(balloon_alloc_empty_page_range);
EXPORT_SYMBOL(balloon_dealloc_empty_page_range);

/*
 * Local variables:
 *  c-file-style: "linux"
 *  indent-tabs-mode: t
 *  c-indent-level: 8
 *  c-basic-offset: 8
 *  tab-width: 8
 * End:
 */
