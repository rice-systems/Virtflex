/******************************************************************************
 * Xen balloon driver - enables returning/claiming memory to/from Xen.
 *
 * Copyright (c) 2003, B Dragovic
 * Copyright (c) 2003-2004, M Williamson, K Fraser
 * Copyright (c) 2005 Dan M. Smith, IBM Corporation
 * Copyright (c) 2010 Daniel Kiper
 *
 * Memory hotplug support was written by Daniel Kiper. Work on
 * it was sponsored by Google under Google Summer of Code 2010
 * program. Jeremy Fitzhardinge from Citrix was the mentor for
 * this project.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License version 2
 * as published by the Free Software Foundation; or, when distributed
 * separately from the Linux kernel or incorporated into other
 * software packages, subject to the following license:
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

#define pr_fmt(fmt) "xen:" KBUILD_MODNAME ": " fmt

#include <linux/cpu.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/cred.h>
#include <linux/errno.h>
#include <linux/mm.h>
#include <linux/bootmem.h>
#include <linux/pagemap.h>
#include <linux/highmem.h>
#include <linux/mutex.h>
#include <linux/list.h>
#include <linux/gfp.h>
#include <linux/notifier.h>
#include <linux/memory.h>
#include <linux/memory_hotplug.h>
#include <linux/percpu-defs.h>
#include <linux/slab.h>
#include <linux/sysctl.h>

#include <asm/page.h>
#include <asm/pgalloc.h>
#include <asm/pgtable.h>
#include <asm/tlb.h>

#include <asm/xen/hypervisor.h>
#include <asm/xen/hypercall.h>

#include <xen/xen.h>
#include <xen/xenbus.h>
#include <xen/interface/xen.h>
#include <xen/interface/memory.h>
#include <xen/balloon.h>
#include <xen/features.h>
#include <xen/page.h>

// for proc/topo_change
#include <linux/proc_fs.h>
#include <asm/uaccess.h>
static struct proc_dir_entry *topo_proc_entry;
#define PROCFS_MAX_SIZE		8
#define PROCFS_NAME 		"topo_change"
static unsigned long long topo_version = 0;

//wait_queue_head_t next_touch_waitq;
//int next_touch_waitc;		// next touch wait counter


#define PAGE_SIZE_ADJUST 0 	// control the granularity of ballooning
				// -9 for 4k ballooning, 0 for 2m

static int xen_hotplug_unpopulated;
#define CONFIG_XEN_BALLOON_NUMA

#ifdef CONFIG_XEN_BALLOON_MEMORY_HOTPLUG

static int zero;
static int one = 1;

static struct ctl_table balloon_table[] = {
	{
		.procname	= "hotplug_unpopulated",
		.data		= &xen_hotplug_unpopulated,
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= proc_dointvec_minmax,
		.extra1         = &zero,
		.extra2         = &one,
	},
	{ }
};

static struct ctl_table balloon_root[] = {
	{
		.procname	= "balloon",
		.mode		= 0555,
		.child		= balloon_table,
	},
	{ }
};

static struct ctl_table xen_root[] = {
	{
		.procname	= "xen",
		.mode		= 0555,
		.child		= balloon_root,
	},
	{ }
};

#endif

/*
 * Use one extent per PAGE_SIZE to avoid to break down the page into
 * multiple frame.
 */
#define EXTENT_ORDER (fls(XEN_PFN_PER_PAGE) - 1 + HPAGE_SHIFT + PAGE_SIZE_ADJUST - PAGE_SHIFT)

/*
 * balloon_process() state:
 *
 * BP_DONE: done or nothing to do,
 * BP_WAIT: wait to be rescheduled,
 * BP_EAGAIN: error, go to sleep,
 * BP_ECANCELED: error, balloon operation canceled.
 */

enum bp_state {
	BP_DONE,
	BP_WAIT,
	BP_EAGAIN,
	BP_ECANCELED
};


static DEFINE_MUTEX(balloon_mutex);

struct balloon_stats balloon_stats;
EXPORT_SYMBOL_GPL(balloon_stats);

unsigned int num_nodes = 1; 	// added this variable so that UMA
				// will behave just fine
#ifdef CONFIG_XEN_BALLOON_NUMA
struct balloon_stats* balloon_stats_numa = NULL;
static struct list_head* ballooned_pages_numa = NULL;
#endif

/* We increase/decrease in batches which fit in a page */
static xen_pfn_t frame_list[PAGE_SIZE / sizeof(xen_pfn_t)];


/* List of ballooned pages, threaded through the mem_map array. */
static LIST_HEAD(ballooned_pages);
static DECLARE_WAIT_QUEUE_HEAD(balloon_wq);

/* Main work function, always executed in process context. */
static struct workqueue_struct *balloon_workq;
static void balloon_process(struct work_struct *work);
static DECLARE_DELAYED_WORK(balloon_worker, balloon_process);

/* When ballooning out (allocating memory to return to Xen) we don't really
   want the kernel to try too hard since that can trigger the oom killer. */
#define GFP_BALLOON \
	(GFP_KERNEL | __GFP_NORETRY | __GFP_MOVABLE | __GFP_MEMALLOC | ___GFP_DIRECT_RECLAIM | __GFP_THISNODE)
//	(GFP_HIGHUSER | __GFP_NORETRY | __GFP_MOVABLE | __GFP_MEMALLOC)
//	(GFP_HIGHUSER | __GFP_NOWARN | __GFP_NORETRY | __GFP_NOMEMALLOC)

static void scrub_page(struct page *page)
{
#ifdef CONFIG_XEN_SCRUB_PAGES
	clear_highpage(page);
#endif
}

/* balloon_append: add the given page to the balloon. */
static void __balloon_append(struct page *page, int node, int order)
{
	/* Lowmem is re-populated first, so highmem pages go at list tail. */
	if (PageHighMem(page)) {
#ifdef CONFIG_XEN_BALLOON_NUMA
		list_add_tail(&page->lru, &ballooned_pages_numa[node]);
		page->private = order;
		balloon_stats_numa[node].balloon_high++;
#else
		list_add_tail(&page->lru, &ballooned_pages);
#endif
		balloon_stats.balloon_high++;
	} else {
#ifdef CONFIG_XEN_BALLOON_NUMA
		list_add(&page->lru, &ballooned_pages_numa[node]);
		page->private = order;
		balloon_stats_numa[node].balloon_low++;
#else
		list_add(&page->lru, &ballooned_pages);
#endif
		balloon_stats.balloon_low++;
	}
	wake_up(&balloon_wq);
}

static void balloon_append(struct page *page, int node, int order)
{
	__balloon_append(page, node, order);
}

/* balloon_retrieve: rescue a page from the balloon, if it is not empty. */
static struct page *balloon_retrieve(bool require_lowmem, int node, int* order)
{
	struct page *page;

#ifdef CONFIG_XEN_BALLOON_NUMA
	if (unlikely(!ballooned_pages_numa))
		return NULL;
	if (list_empty(&ballooned_pages_numa[node]))
		return NULL;
#else
	if (list_empty(&ballooned_pages))
		return NULL;
#endif

#ifdef CONFIG_XEN_BALLOON_NUMA
	page = list_entry(ballooned_pages_numa[node].next, struct page, lru);
#else
	page = list_entry(ballooned_pages.next, struct page, lru);
#endif
	if (require_lowmem && PageHighMem(page))
		return NULL;
	list_del(&page->lru);
	*order = page->private;

	if (PageHighMem(page)){
		balloon_stats.balloon_high--;
#ifdef CONFIG_XEN_BALLOON_NUMA
		balloon_stats_numa[node].balloon_high--;
#endif
	}
	else{
		balloon_stats.balloon_low--;
#ifdef CONFIG_XEN_BALLOON_NUMA
		balloon_stats_numa[node].balloon_low--;
#endif
	}

	return page;
}

static struct page *balloon_next_page(struct page *page, int node, int* order)
{
	struct list_head *next = page->lru.next;
	struct page* ret_page;
#ifdef CONFIG_XEN_BALLOON_NUMA
	if (next == &ballooned_pages_numa[node])
		return NULL;
#else
	if (next == &ballooned_pages)
		return NULL;
#endif
	ret_page = list_entry(next, struct page, lru);
	*order = ret_page->private;
	return ret_page;
}

static enum bp_state update_schedule(enum bp_state state)
{
	if (state == BP_WAIT)
		return BP_WAIT;

	if (state == BP_ECANCELED)
		return BP_ECANCELED;

	if (state == BP_DONE) {
		balloon_stats.schedule_delay = 1;
		balloon_stats.retry_count = 1;
		return BP_DONE;
	}

	++balloon_stats.retry_count;

	if (balloon_stats.max_retry_count != RETRY_UNLIMITED &&
			balloon_stats.retry_count > balloon_stats.max_retry_count) {
		balloon_stats.schedule_delay = 1;
		balloon_stats.retry_count = 1;
		return BP_ECANCELED;
	}

	balloon_stats.schedule_delay <<= 1;

	if (balloon_stats.schedule_delay > balloon_stats.max_schedule_delay)
		balloon_stats.schedule_delay = balloon_stats.max_schedule_delay;

	return BP_EAGAIN;
}

#ifdef CONFIG_XEN_BALLOON_MEMORY_HOTPLUG
static void release_memory_resource(struct resource *resource)
{
	if (!resource)
		return;

	/*
	 * No need to reset region to identity mapped since we now
	 * know that no I/O can be in this region
	 */
	release_resource(resource);
	kfree(resource);
}

/*
 * Host memory not allocated to dom0. We can use this range for hotplug-based
 * ballooning.
 *
 * It's a type-less resource. Setting IORESOURCE_MEM will make resource
 * management algorithms (arch_remove_reservations()) look into guest e820,
 * which we don't want.
 */
static struct resource hostmem_resource = {
	.name   = "Host RAM",
};

void __attribute__((weak)) __init arch_xen_balloon_init(struct resource *res)
{}

static struct resource *additional_memory_resource(phys_addr_t size)
{
	struct resource *res, *res_hostmem;
	int ret = -ENOMEM;

	res = kzalloc(sizeof(*res), GFP_KERNEL);
	if (!res)
		return NULL;

	res->name = "System RAM";
	res->flags = IORESOURCE_SYSTEM_RAM | IORESOURCE_BUSY;

	res_hostmem = kzalloc(sizeof(*res), GFP_KERNEL);
	if (res_hostmem) {
		/* Try to grab a range from hostmem */
		res_hostmem->name = "Host memory";
		ret = allocate_resource(&hostmem_resource, res_hostmem,
					size, 0, -1,
					PAGES_PER_SECTION * PAGE_SIZE, NULL, NULL);
	}

	if (!ret) {
		/*
		 * Insert this resource into iomem. Because hostmem_resource
		 * tracks portion of guest e820 marked as UNUSABLE noone else
		 * should try to use it.
		 */
		res->start = res_hostmem->start;
		res->end = res_hostmem->end;
		ret = insert_resource(&iomem_resource, res);
		if (ret < 0) {
			pr_err("Can't insert iomem_resource [%llx - %llx]\n",
				res->start, res->end);
			release_memory_resource(res_hostmem);
			res_hostmem = NULL;
			res->start = res->end = 0;
		}
	}

	if (ret) {
		ret = allocate_resource(&iomem_resource, res,
					size, 0, -1,
					PAGES_PER_SECTION * PAGE_SIZE, NULL, NULL);
		if (ret < 0) {
			pr_err("Cannot allocate new System RAM resource\n");
			kfree(res);
			return NULL;
		}
	}

#ifdef CONFIG_SPARSEMEM
	{
		unsigned long limit = 1UL << (MAX_PHYSMEM_BITS - PAGE_SHIFT);
		unsigned long pfn = res->start >> PAGE_SHIFT;

		if (pfn > limit) {
			pr_err("New System RAM resource outside addressable RAM (%lu > %lu)\n",
			       pfn, limit);
			release_memory_resource(res);
			release_memory_resource(res_hostmem);
			return NULL;
		}
	}
#endif

	return res;
}

static enum bp_state reserve_additional_memory(void)
{
	long credit;
	struct resource *resource;
	int nid, rc;
	unsigned long balloon_hotplug;

	credit = balloon_stats.target_pages + balloon_stats.target_unpopulated
		- balloon_stats.total_pages;

	/*
	 * Already hotplugged enough pages?  Wait for them to be
	 * onlined.
	 */
	if (credit <= 0)
		return BP_WAIT;

	balloon_hotplug = round_up(credit, PAGES_PER_SECTION);

	resource = additional_memory_resource(balloon_hotplug * PAGE_SIZE);
	if (!resource)
		goto err;

	nid = memory_add_physaddr_to_nid(resource->start);

#ifdef CONFIG_XEN_HAVE_PVMMU
	/*
	 * We don't support PV MMU when Linux and Xen is using
	 * different page granularity.
	 */
	BUILD_BUG_ON(XEN_PAGE_SIZE != PAGE_SIZE);

        /*
         * add_memory() will build page tables for the new memory so
         * the p2m must contain invalid entries so the correct
         * non-present PTEs will be written.
         *
         * If a failure occurs, the original (identity) p2m entries
         * are not restored since this region is now known not to
         * conflict with any devices.
         */ 
	if (!xen_feature(XENFEAT_auto_translated_physmap)) {
		unsigned long pfn, i;

		pfn = PFN_DOWN(resource->start);
		for (i = 0; i < balloon_hotplug; i++) {
			if (!set_phys_to_machine(pfn + i, INVALID_P2M_ENTRY)) {
				pr_warn("set_phys_to_machine() failed, no memory added\n");
				goto err;
			}
                }
	}
#endif

	/*
	 * add_memory_resource() will call online_pages() which in its turn
	 * will call xen_online_page() callback causing deadlock if we don't
	 * release balloon_mutex here. Unlocking here is safe because the
	 * callers drop the mutex before trying again.
	 */
	mutex_unlock(&balloon_mutex);
	rc = add_memory_resource(nid, resource, memhp_auto_online);
	mutex_lock(&balloon_mutex);

	if (rc) {
		pr_warn("Cannot add additional memory (%i)\n", rc);
		goto err;
	}

	balloon_stats.total_pages += balloon_hotplug;

	return BP_WAIT;
  err:
	release_memory_resource(resource);
	return BP_ECANCELED;
}

static void xen_online_page(struct page *page)
{
	__online_page_set_limits(page);

	mutex_lock(&balloon_mutex);

	//__balloon_append(page);
	__balloon_append(page, 0, 0);

	mutex_unlock(&balloon_mutex);
}

static int xen_memory_notifier(struct notifier_block *nb, unsigned long val, void *v)
{
	if (val == MEM_ONLINE)
		queue_delayed_work(balloon_workq, &balloon_worker, 0);
		//schedule_delayed_work(&balloon_worker, 0);

	return NOTIFY_OK;
}

static struct notifier_block xen_memory_nb = {
	.notifier_call = xen_memory_notifier,
	.priority = 0
};
#else
static enum bp_state reserve_additional_memory(void)
{
	balloon_stats.target_pages = balloon_stats.current_pages;
	return BP_ECANCELED;
}
#endif /* CONFIG_XEN_BALLOON_MEMORY_HOTPLUG */

static long current_credit(int node)
{
#ifdef CONFIG_XEN_BALLOON_NUMA
	return balloon_stats_numa[node].target_pages - balloon_stats_numa[node].current_pages;
#else
	return balloon_stats.target_pages - balloon_stats.current_pages;
#endif
}


static bool balloon_is_inflated(int node)
{
#ifdef CONFIG_XEN_BALLOON_NUMA
	return balloon_stats_numa[node].balloon_low || balloon_stats_numa[node].balloon_high;
#else
	return balloon_stats.balloon_low || balloon_stats.balloon_high;
#endif
}

static enum bp_state increase_reservation(unsigned long nr_pages, int node)
{
	int rc;
	unsigned long i,j;
	int order;
	static unsigned long hypercnt = 0;
	struct page   *page, *tmp;
	struct xen_memory_reservation reservation = {
		.address_bits = 0,
		.extent_order = EXTENT_ORDER,
		.domid        = DOMID_SELF
	};
	// set the memflags for reservation
	reservation.address_bits |= XENMEMF_vnode;
	reservation.address_bits |= XENMEMF_exact_node(node);

	//printk("I'm here at increase_reservation, node:%d,"
	//		"nr_pages: %ld\n", node, nr_pages);
	if (nr_pages > ARRAY_SIZE(frame_list))
		nr_pages = ARRAY_SIZE(frame_list);
#ifdef CONFIG_XEN_BALLOON_NUMA
	page = list_first_entry_or_null(&ballooned_pages_numa[node], struct page, lru);
#else
	page = list_first_entry_or_null(&ballooned_pages, struct page, lru);
#endif
	for (i = 0; i < nr_pages;i++) {
		if (!page) {
			nr_pages = i;
			break;
		}

		/* XENMEM_populate_physmap requires a PFN based on Xen
		 * granularity.
		 */
		//if(i % 100 == 0)
		//printk("node: %d, struct page physical address is: %lu,\n", node, __pa(page));
		frame_list[i] = page_to_xen_pfn(page);
		page = balloon_next_page(page, node, &order);
		//for (j = 0; j < (1U << order); j++, page++){
		//	if (likely(i < nr_pages)){
		//		frame_list[i] = page_to_xen_pfn(page);
		//	}
		//	else{
		//		break;
		//	} 
		//}
		
	}
	//printk("I'm here at increase_reservation after, node:%d,"
	//		"nr_pages: %ld\n", node, nr_pages);

	set_xen_guest_handle(reservation.extent_start, frame_list);
	reservation.nr_extents = nr_pages;
	rc = HYPERVISOR_memory_op(XENMEM_populate_physmap_2, &reservation);
	hypercnt=hypercnt+1;
	//printk("I'm here at increase_reservation, hypercnt=%lu\n", hypercnt);
	if (rc <= 0){
		printk("I'm here at increase_reservation,"
			"XENMEM_populate_physmap failed after %d pages,"
			"retry\n", rc);
		return BP_EAGAIN;
	}

	for (i = 0; i < rc; i++) {
		page = balloon_retrieve(false, node, &order);
		BUG_ON(page == NULL);

#ifdef CONFIG_XEN_HAVE_PVMMU
		/*
		 * We don't support PV MMU when Linux and Xen is using
		 * different page granularity.
		 */
		BUILD_BUG_ON(XEN_PAGE_SIZE != PAGE_SIZE);
		tmp = page;
		for (j = 0; j < (1U << EXTENT_ORDER); page++,j++){
		if (!xen_feature(XENFEAT_auto_translated_physmap)) {
			unsigned long pfn = page_to_pfn(page);
			set_phys_to_machine(pfn, frame_list[i]+j);

			/* Link back into the page tables if not highmem. */
			if (!PageHighMem(page)) {
				int ret;
				ret = HYPERVISOR_update_va_mapping(
						(unsigned long)__va(pfn << PAGE_SHIFT),
						mfn_pte(frame_list[i]+j, PAGE_KERNEL),
						0);
				BUG_ON(ret);
			}
		}
		}
		page = tmp;
#endif

		/* Relinquish the page back to the allocator. */
		//free_reserved_page(page);
		for (j = 0; j < (1U << EXTENT_ORDER); j++, page++){
                	free_reserved_page(page);
		}
	}

	balloon_stats.current_pages += rc;
#ifdef CONFIG_XEN_BALLOON_NUMA
	balloon_stats_numa[node].current_pages += rc;
#endif

	return BP_DONE;
}

// return the largest power remaining in op

static unsigned long largest_power_remain(unsigned long op, int* order){
	unsigned long power = 1;
	*order = 0;
	for (; 2*power <= op; power*=2,(*order)++);
	printk("power%lu, order%d\n", power, *order);
	return op - power;
}

static enum bp_state decrease_reservation(unsigned long nr_pages, gfp_t gfp, int node)
{
	enum bp_state state = BP_DONE;
	unsigned long i, j, tmp_nr_pages, pages_covered=0;
	struct page *page, *tmp, *tmp_2;
	int ret, nid, order;
	struct xen_memory_reservation reservation = {
		.address_bits = 0,
		.extent_order = EXTENT_ORDER,
		.domid        = DOMID_SELF
	};
	LIST_HEAD(pages);

	//printk("I'm here at decrease_reservation, nr_pages before: %lu\n", nr_pages);

	if (nr_pages > ARRAY_SIZE(frame_list)){
		nr_pages = ARRAY_SIZE(frame_list);
	}

	//printk("I'm here at decrease_reservation, nr_pages after: %lu, order: %u\n", nr_pages, reservation.extent_order);
	

	for (i = 0; i < nr_pages; i++) {
	     if(node == -1){
		page = alloc_pages_node(0, gfp, 0);
		if (page == NULL) {
                     nr_pages = i;
                     state = BP_EAGAIN;
		     break;
		}
		//printk("I'm here at decrease_reservation, node == -1\n");
		adjust_managed_page_count(page, -1);
                scrub_page(page);
		list_add(&page->lru, &pages);
		continue;
	     }
	     //if (i == 256){
		//printk("I'm here in decrease_reservation, about to hlt\n");
	     	//__asm__("hlt");
	     //}
	     page = alloc_pages_node(node, gfp, EXTENT_ORDER);
             if (likely(page)){
                     nid = page_to_nid(page);
                     if (nid != node){
                             printk("I'm here at alloc_pages_node"
                             ", could not alloc pages from node "
                             "%d get %d instead\n", node, nid);
			    //for (j = 0; j < (1U << EXTENT_ORDER); page++,j++){
			     __free_pages(page, EXTENT_ORDER);
			     //}	
                             nr_pages = i;
                             state = BP_EAGAIN;
                             break;
                     }
             }
             if (page == NULL) {
                     nr_pages = i;
                     state = BP_EAGAIN;
                     printk("I'm here at decrease reservation,"
                             "alloc failed after %ld pages\n", i);
                     break;
             }
		//printk("Page Scrub: before =========");
		for (j = 0; j < (1U << EXTENT_ORDER); page++,j++){
	                adjust_managed_page_count(page, -1);
                	scrub_page(page);
			if (j == 0 ) // the first page of the batch
				list_add(&page->lru, &pages);
		}	
		//printk("Page Scrub: after ==========");
     }
	
	//printk("I'm here at decrease_reservation, nr_pages after 2: %lu\n", nr_pages);
	/*
	 * Ensure that ballooned highmem pages don't have kmaps.
	 *
	 * Do this before changing the p2m as kmap_flush_unused()
	 * reads PTEs to obtain pages (and hence needs the original
	 * p2m entry).
	 */
	kmap_flush_unused();

	/*
	 * Setup the frame, update direct mapping, invalidate P2M,
	 * and add to balloon.
	 */
	i = 0;
	list_for_each_entry_safe(page, tmp, &pages, lru) {
		/* XENMEM_decrease_reservation requires a GFN */
		frame_list[i++] = xen_page_to_gfn(page);
		//if(i == 5){
		//printk("I'm here at decrease reservation, frame_list[%d]:%lu\n", i-1, frame_list[i-1]);
		//}

#ifdef CONFIG_XEN_HAVE_PVMMU
		/*
		 * We don't support PV MMU when Linux and Xen is using
		 * different page granularity.
		 */
		BUILD_BUG_ON(XEN_PAGE_SIZE != PAGE_SIZE);
		//printk("I'm here at decrease reservation, PVMMU code is called\n");
		if (node == -1)
			order = 0;
		else
			order = EXTENT_ORDER;	
	
		tmp_2 = page;
		for (j = 0; j < (1U << order); page++,j++){

		if (!xen_feature(XENFEAT_auto_translated_physmap)) {
			unsigned long pfn = page_to_pfn(page);
			//printk("I'm here at decrease reservation, PVMMU code is called pfn: %lu\n", pfn);
			if (!PageHighMem(page)) {
				ret = HYPERVISOR_update_va_mapping(
						(unsigned long)__va(pfn << PAGE_SHIFT),
						__pte_ma(0), 0);
				if(ret)
					printk("I'm here at decrease reservation, PVMMU code is called ret: %d\n", ret);
				BUG_ON(ret);
			}
			__set_phys_to_machine(pfn, INVALID_P2M_ENTRY);
		}
		}
		page = tmp_2;
#endif
		list_del(&page->lru);
		if (unlikely(node == -1))
			balloon_append(page, 0, 0);
		else
			balloon_append(page, node, 0);
	}

	flush_tlb_all();

	if (node == -1)
		reservation.extent_order = 0;
	set_xen_guest_handle(reservation.extent_start, frame_list);
	reservation.nr_extents   = nr_pages;
	//printk("I'm here about to call decrease_reservation_2\n");
	ret = HYPERVISOR_memory_op(XENMEM_decrease_reservation_2, &reservation);
	//printk("I'm here called decrease_reservation_2, nr_pages:%lu, ret:%d\n",nr_pages, ret);
	BUG_ON(ret != nr_pages);

	balloon_stats.current_pages -= nr_pages;
#ifdef CONFIG_XEN_BALLOON_NUMA
	balloon_stats_numa[node].current_pages -= nr_pages;
#endif
	return state;
}

/*
 * As this is a work item it is guaranteed to run as a single instance only.
 * We may of course race updates of the target counts (which are protected
 * by the balloon lock), or with changes to the Xen hard limit, but we will
 * recover from these in time.
 */

/*
 * Made some changes to the NUMAlized balloon driver, the goal 
 * is that the balloon driver can respond to target change for each node. As for
 * implementation, the balloon driver works on two phases, the scanning
 * phase and the waiting phase. In the scanning phase, the balloon driver scans
 * each node in sequence, each balloon_process call allows the balloon driver
 * to work on one node at a time. At the end of each balloon_process call, the balloon
 * driver would schedule itself to work on the next node with the preference of running
 * on CPUs from that node. If the balloon driver is done working for all the nodes, it
 * will check if there is any node need rescanning by checking the again_node value. If
 * the answer is yes, then the balloon driver would reschedule a rescan starting from 
 * again_node after a short idle time period. 
 * */

static void balloon_process(struct work_struct *work)
{

	static bool normal_wakeup = true;
	static bool* again_nodes;
	static int* nodes_action; 
	// 0-> no action; 1 -> increase reservation; -1 -> decrease reservation

	static int fired = false; //weather the node_to_cpu struct is being filled or not
	enum bp_state state = BP_DONE; // node-local state 
	long credit = 0; // node-local credit
	int i, ret, j, node;
	int topo_change = 0;

	printk("Balloon_process starts\n");
	
	//printk("start_info.store_mfn: %lu, start_info.console.mfn %lu\n", xen_start_info->store_mfn, xen_start_info->console.domU.mfn);
	if (!fired){
		again_nodes = (bool *)kmalloc(num_nodes*sizeof(bool),GFP_KERNEL);
		nodes_action = (int *)kmalloc(num_nodes*sizeof(int),GFP_KERNEL);
		for(i=0; i < num_nodes; i++){
			again_nodes[i] = false;
			nodes_action[i] = 0;
		}
		fired=true;
	}
	
	if(normal_wakeup){
		//printk("I'm here in balloon_process, normal wakeup, cpu: %d\n", smp_processor_id());
	}
	else{
		//printk("I'm here in balloon_process, again wakeup, cpu: %d\n", smp_processor_id());
	}
                
  	ret = xenbus_scanf(XBT_NIL, "numa",\
                                "topo_change", "%d", &i);
	/* if this is a topology change instead of regular ballooning */
	if (ret >= 0){
		topo_change = i;
		printk("In balloon_process, get /numa/topo_change value: %d\n", i);
	}
	if (ret < 0 ){
		printk("In balloon_process, error during xenbus_scanf /numa/topo_change\n");
		topo_change = 0;	
	}
	/* If it's a node remove, then wait until next_touch is finished*/
	if (i == 2){
		printk("I'm here in balloon_process, topo_change indicators remove nodes\n");
		topo_version++;
		//wait_event_timeout(next_touch_waitq, next_touch_waitc <= 0, 5*HZ); // wait for 5 seconds before remove nodes
	}

	for(i = 0; i < num_nodes; i++){
		//if(!normal_wakeup && !again_nodes[i])
		//	continue;
		state = BP_DONE;
		nodes_action[i] = 0;
                printk("credit handling loop, node %d, topo_change: %d, current_credit(node)> 0 %d\n", i, topo_change, current_credit(i) >0);

		/* move the fake node to the right place before do any ballooning */
		if (topo_change && current_credit(i) >0 ){
                	printk("here at the kernel migration code\n");
			struct pglist_data* node_list;
                        node = i;
                        node_list = first_online_pgdat();
                        for (j=0; j<node; node_list = next_online_pgdat(node_list),j++);
                        struct xen_memory_range_migration migration = {
                                .start_addr = 0,
                                .end_addr = 0,
				.move_to_node = 0,
                                .domid        = DOMID_SELF
                        };
			migration.move_to_node = node;
                        migration.start_addr = node_list->node_start_pfn;
                        migration.end_addr = node_list->node_start_pfn +
                                        node_list->node_spanned_pages - 1;
			// temporary comments out GMM
                        //printk("before ballooning, called XENMEM_migrate_page_range, node %d, node_spanned_pages: %lu, node_present_pages %lu\n", node, node_list->node_spanned_pages, node_list->node_present_pages);
                        //HYPERVISOR_memory_op(XENMEM_migrate_page_range, &migration);
		}
		
		/* enter the loop of balloonning */	
		do {
			mutex_lock(&balloon_mutex);

			credit = current_credit(i);

			if (credit > 0) {
				if (balloon_is_inflated(i))
					state = increase_reservation(credit, i);
				else
					state = reserve_additional_memory();
				nodes_action[i] = 1;
			}
			else if (credit < 0){
				state = decrease_reservation(-credit, GFP_BALLOON, i);
				nodes_action[i] = -1;
			}
			state = update_schedule(state);

			mutex_unlock(&balloon_mutex);

			cond_resched();

		} while (credit && state == BP_DONE);
		
		again_nodes[i] = (state == BP_EAGAIN && state != BP_ECANCELED);
		printk("I'm here in balloon_process, finished up working on node:%d, credit: %d, again_node: %d\n",i, credit, again_nodes[i]);
	}
	
	for(i=0; i< num_nodes; i++){
		if(again_nodes[i] == true){
			normal_wakeup = false;
			printk("I'm here in balloon_process, node %d need to be scheduled again\n",i);
			break;
			//schedule_delayed_work(&balloon_worker, balloon_stats.schedule_delay * HZ);
		}
	}
	if (i == num_nodes){
		printk("I'm here in balloon_process, next wakeup is a normal wakeup\n");
		normal_wakeup = true;
	}	

	if (!normal_wakeup)
		queue_delayed_work(balloon_workq, &balloon_worker, balloon_stats.schedule_delay * HZ);
	else
	{
                ret = xenbus_scanf(XBT_NIL, "numa",\
                                "topo_change", "%d", &i);
		// if the key is read successfully and topo_change==1
		if (ret == 1 && i >0) {
			
			for(i=0; i<=num_nodes; i++){
				if (nodes_action[i] != -1)
					continue;
				struct pglist_data* node_list;
				node = i;
				node_list = first_online_pgdat();
				for (j=0; j<node; node_list = next_online_pgdat(node_list),j++);
				struct xen_memory_range_migration migration = {
					.start_addr = 0,
					.end_addr = 0,
					.move_to_node = -1,
					.domid        = DOMID_SELF
				};
				migration.start_addr = node_list->node_start_pfn;
				migration.end_addr = node_list->node_start_pfn +
						node_list->node_spanned_pages - 1;
				// temporary comment out GMM			
				//printk("after ballooning, called XENMEM_migrate_page_range,node_spanned_pages: %lu, node_present_pages %lu\n", node_list->node_spanned_pages, node_list->node_present_pages);
				//HYPERVISOR_memory_op(XENMEM_migrate_page_range, &migration);
			}
			ret = xenbus_write(XBT_NIL, "numa",\
                		"topo_change", "0");
			//ret = 0;
        		if (ret < 0)
                		printk("I'm here at balloon_process, error when writing topo_change=0 to xenstore, err:%d\n", ret);
			else{
				printk("I'm here at balloon_process, seems to handle topo change successfully\n");
				/* notify application, for the remove node case, topo_version is already increased before */
				if (topo_change != -1)
					topo_version++;
			}
		}
	}

}

/* Resets the Xen limit, sets new target, and kicks off processing. */
void balloon_set_new_target(unsigned long target, int node)
{
	int i;
	/* No need for lock. Not read-modify-write updates. */
	printk("I'm here at balloon_set_new_target, target: %lu,"
		       "node:%d, current cpu: %d\n", target, node, smp_processor_id());
#ifdef CONFIG_XEN_BALLOON_NUMA
	if (node >= num_nodes){
		printk("I'm here at set_new_target, node>num_nodes\n");
		return;
	}
	else if(node < 0){
		// set the total target and average on each node
		balloon_stats.target_pages = target;
		for (i = 0; i < num_nodes; i++){
			balloon_stats_numa[i].target_pages =\
				 target/num_nodes;
			printk("I'm here at balloon_set_new_target,"
				"node %d, target: %lu\n",i, \
				balloon_stats_numa[i].target_pages);
		}
	}
	else
		balloon_stats_numa[node].target_pages = target;
#else
	balloon_stats.target_pages = target;
#endif
	queue_delayed_work(balloon_workq, &balloon_worker, 0);
	//schedule_delayed_work(&balloon_worker, 0);
}
EXPORT_SYMBOL_GPL(balloon_set_new_target);

static int add_ballooned_pages(int nr_pages)
{
	enum bp_state st;
	//printk("I'm here at add_ballooned_pages, nr_pages: %d\n", nr_pages);

	if (xen_hotplug_unpopulated) {
		st = reserve_additional_memory();
		printk("I'm here at add_ballooned_pages,"
			       "reserve_additional_memory is called\n");
		if (st != BP_ECANCELED) {
		printk("I'm here at add_ballooned_pages,"
			       "reserve_additional_memory is "
			       "successfully called\n");
			mutex_unlock(&balloon_mutex);
			wait_event(balloon_wq,
				   !list_empty(&ballooned_pages));
			mutex_lock(&balloon_mutex);
			return 0;
		}
	}

	//st = decrease_reservation(nr_pages, GFP_USER);
	st = decrease_reservation(nr_pages, GFP_USER, -1);
	if (st != BP_DONE)
		return -ENOMEM;

	return 0;
}

#ifdef CONFIG_XEN_BALLOON_NUMA
void balloon_init_numa(void);
#endif

/**
 * alloc_xenballooned_pages - get pages that have been ballooned out
 * @nr_pages: Number of pages to get
 * @pages: pages returned
 * @return 0 on success, error otherwise
 */
int alloc_xenballooned_pages(int nr_pages, struct page **pages)
{
	int pgno = 0;
	struct page *page;
	int order;
	int ret;

	printk("I'm here at alloc_xenballooned_pages"
			", nr_pages: %d\n", nr_pages);
	mutex_lock(&balloon_mutex);
#ifdef CONFIG_XEN_BALLOON_NUMA
	if (unlikely(!balloon_stats_numa))
		balloon_init_numa();
#endif
	balloon_stats.target_unpopulated += nr_pages;

	while (pgno < nr_pages) {
		//page = balloon_retrieve(true);
		page = balloon_retrieve(true, 0, &order);
		if (page) {
			pages[pgno++] = page;
#ifdef CONFIG_XEN_HAVE_PVMMU
			/*
			 * We don't support PV MMU when Linux and Xen is using
			 * different page granularity.
			 */
			BUILD_BUG_ON(XEN_PAGE_SIZE != PAGE_SIZE);

			if (!xen_feature(XENFEAT_auto_translated_physmap)) {
				ret = xen_alloc_p2m_entry(page_to_pfn(page));
				if (ret < 0)
					goto out_undo;
			}
#endif
		} else {
			ret = add_ballooned_pages(nr_pages - pgno);
			if (ret < 0)
				goto out_undo;
		}
	}
	mutex_unlock(&balloon_mutex);
	return 0;
 out_undo:
	mutex_unlock(&balloon_mutex);
	free_xenballooned_pages(pgno, pages);
	return ret;
}
EXPORT_SYMBOL(alloc_xenballooned_pages);

/**
 * free_xenballooned_pages - return pages retrieved with get_ballooned_pages
 * @nr_pages: Number of pages
 * @pages: pages to return
 */
void free_xenballooned_pages(int nr_pages, struct page **pages)
{
	int i;
	
	printk("I'm here at free_xenballooned_pages\n");
	mutex_lock(&balloon_mutex);

	for (i = 0; i < nr_pages; i++) {
		if (pages[i])
			//balloon_append(pages[i]);
			balloon_append(pages[i], 0, 0);
	}

	balloon_stats.target_unpopulated -= nr_pages;

	/* The balloon may be too large now. Shrink it if needed. */
	//if (current_credit())
	if (current_credit(0))
		queue_delayed_work(balloon_workq, &balloon_worker, 0);
		//schedule_delayed_work(&balloon_worker, 0);

	mutex_unlock(&balloon_mutex);
}
EXPORT_SYMBOL(free_xenballooned_pages);

#ifdef CONFIG_XEN_PV
static void __init balloon_add_region(unsigned long start_pfn,
				      unsigned long pages)
{
	unsigned long pfn, extra_pfn_end;
	struct page *page;

	/*
	 * If the amount of usable memory has been limited (e.g., with
	 * the 'mem' command line parameter), don't add pages beyond
	 * this limit.
	 */
	extra_pfn_end = min(max_pfn, start_pfn + pages);

	for (pfn = start_pfn; pfn < extra_pfn_end; pfn++) {
		page = pfn_to_page(pfn);
		/* totalram_pages and totalhigh_pages do not
		   include the boot-time balloon extension, so
		   don't subtract from it. */
		//__balloon_append(page);
		__balloon_append(page, 0, 0);
	}

	balloon_stats.total_pages += extra_pfn_end - start_pfn;
}
#endif

//int 
//procfile_read(char *buffer,
//	      char **buffer_location,
//	      off_t offset, int buffer_length, int *eof, void *data)
ssize_t
procfile_read(struct file *filp,char *buf,size_t count,loff_t *offp)
{
	//printk(KERN_INFO "procfile_read (/proc/%s) called, topo_version: %llu\n", PROCFS_NAME, topo_version);
	//memcpy(buffer, &topo_version, sizeof(unsigned long long));
	copy_to_user(buf,&topo_version, sizeof(unsigned long long));
	return sizeof(unsigned long long);
};

static const struct file_operations proc_file_fops = {
 .read  = procfile_read,
};

#ifdef CONFIG_XEN_BALLOON_NUMA
void balloon_init_numa(void)
{
	int i;
	struct pglist_data* node_list;

	node_list = first_online_pgdat();
	for (i=0; node_list; node_list = \
			next_online_pgdat(node_list),i++);
	printk("I'm here, number of online nodes: %d\n", i);
	num_nodes = i;
	
	balloon_stats_numa = (struct balloon_stats*)kmalloc(
			sizeof(struct balloon_stats)*num_nodes, 
			GFP_KERNEL);
	ballooned_pages_numa = (struct list_head*)kmalloc(
			sizeof(struct list_head)*num_nodes, 
			GFP_KERNEL);
	if (!balloon_stats_numa || !ballooned_pages_numa)
		goto error;
	
	// do LIST_HEAD_INIT at runtime
	for(i = 0; i < num_nodes; i++){
		ballooned_pages_numa[i].next=&ballooned_pages_numa[i];
		ballooned_pages_numa[i].prev=&ballooned_pages_numa[i];
	}
	
	for(i = 0; i < num_nodes; i++){
		unsigned long divisor = num_nodes*(1U << (HPAGE_SHIFT + PAGE_SIZE_ADJUST- PAGE_SHIFT));
		balloon_stats_numa[i].current_pages = get_num_physpages() / divisor;
		if (get_num_physpages() % divisor){
			balloon_stats_numa[i].current_pages+=1;
		}
		//balloon_stats_numa[i].current_pages = get_num_physpages()/(num_nodes * (1U << (HPAGE_SHIFT + PAGE_SIZE_ADJUST- PAGE_SHIFT)));
		balloon_stats_numa[i].target_pages  = balloon_stats_numa[i].current_pages;
		balloon_stats_numa[i].balloon_low   = 0;
		balloon_stats_numa[i].balloon_high  = 0;
		balloon_stats_numa[i].total_pages   = balloon_stats.current_pages;

		balloon_stats_numa[i].schedule_delay = 1;
		balloon_stats_numa[i].max_schedule_delay = 32;
		balloon_stats_numa[i].retry_count = 1;
		//balloon_stats_numa[i].max_retry_count = RETRY_UNLIMITED;
		balloon_stats_numa[i].max_retry_count = 3;
	}
	
	/* create wq struct for balloon_driver */
	balloon_workq = alloc_workqueue("balloon_wq", WQ_HIGHPRI | WQ_UNBOUND, 1);
	//init_waitqueue_head(&next_touch_waitq);
	cpumask_var_t balloon_mask;
	if (!alloc_cpumask_var(&balloon_mask, GFP_KERNEL)){
		printk("Error when allocate cpumask in balloon_init\n");
	}
	int tmp_cpu;
	for_each_possible_cpu(tmp_cpu){
		cpumask_test_and_set_cpu(tmp_cpu, balloon_mask);
	}
	printk("balloon_mask weight: %u\n", cpumask_weight(balloon_mask));	
	struct workqueue_attrs * balloon_wq_attrs = alloc_workqueue_attrs(GFP_KERNEL);
	if(!balloon_wq_attrs){
		printk("Error when allocate wq_attrs in balloon_init\n");
	}
	balloon_wq_attrs->nice = -20;
	balloon_wq_attrs->cpumask = balloon_mask;
	balloon_wq_attrs->no_numa = 0;
	if(apply_workqueue_attrs(balloon_workq,balloon_wq_attrs) != 0){
		printk("Error when setting attr for balloon_workq\n");
	}
	//free_cpumask_var(balloon_mask);
	free_workqueue_attrs(balloon_wq_attrs);


	// create /proc/topo_change
	printk("Creating /proc/topo_change\n");
	topo_proc_entry = proc_create(PROCFS_NAME, 0644, NULL, &proc_file_fops);
	if (topo_proc_entry == NULL) {
		remove_proc_entry(PROCFS_NAME, NULL);
		printk(KERN_ALERT "Error: Could not initialize /proc/%s\n",
			PROCFS_NAME);
	}
	
	return;
error:
	printk("Cannot alloc memory for balloon_init_numa\n");
	BUG();
}
#endif

static int __init balloon_init(void)
{
	if (!xen_domain())
		return -ENODEV;

	pr_info("Initialising balloon driver\n");
	printk("I'm here at balloon_init\n");

#ifdef CONFIG_XEN_PV
	balloon_stats.current_pages = xen_pv_domain()
		? min(xen_start_info->nr_pages - xen_released_pages, max_pfn)
		: get_num_physpages();
#else
	balloon_stats.current_pages = get_num_physpages();
#endif
	balloon_stats.target_pages  = balloon_stats.current_pages;
	balloon_stats.balloon_low   = 0;
	balloon_stats.balloon_high  = 0;
	balloon_stats.total_pages   = balloon_stats.current_pages;

	balloon_stats.schedule_delay = 1;
	balloon_stats.max_schedule_delay = 32;
	balloon_stats.retry_count = 1;
	//balloon_stats.max_retry_count = RETRY_UNLIMITED;
	balloon_stats.max_retry_count = 3;
	
#ifdef CONFIG_XEN_BALLOON_NUMA
	if (!balloon_stats_numa)
		balloon_init_numa();
#endif

#ifdef CONFIG_XEN_BALLOON_MEMORY_HOTPLUG
	printk("I'm here at balloon_init, "
			"BALLOON_MEMORY_HOTPLUG defined\n");
	set_online_page_callback(&xen_online_page);
	register_memory_notifier(&xen_memory_nb);
	register_sysctl_table(xen_root);

	arch_xen_balloon_init(&hostmem_resource);
#endif

#ifdef CONFIG_XEN_PV
	{
		int i;

		/*
		 * Initialize the balloon with pages from the extra memory
		 * regions (see arch/x86/xen/setup.c).
		 */
		for (i = 0; i < XEN_EXTRA_MEM_MAX_REGIONS; i++)
			if (xen_extra_mem[i].n_pfns)
				balloon_add_region(xen_extra_mem[i].start_pfn,
						   xen_extra_mem[i].n_pfns);
	}
#endif

	/* Init the xen-balloon driver. */
	xen_balloon_init();

	return 0;
}
subsys_initcall(balloon_init);
