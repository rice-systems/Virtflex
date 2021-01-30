/******************************************************************************
 * memory.c
 *
 * Code to handle memory-related requests.
 *
 * Copyright (c) 2003-2004, B Dragovic
 * Copyright (c) 2003-2005, K A Fraser
 */

#include <xen/types.h>
#include <xen/lib.h>
#include <xen/mm.h>
#include <xen/perfc.h>
#include <xen/sched.h>
#include <xen/event.h>
#include <xen/paging.h>
#include <xen/iocap.h>
#include <xen/guest_access.h>
#include <xen/hypercall.h>
#include <xen/errno.h>
#include <xen/tmem.h>
#include <xen/tmem_xen.h>
#include <xen/numa.h>
#include <xen/mem_access.h>
#include <xen/trace.h>
#include <asm/current.h>
#include <asm/hardirq.h>
#include <asm/p2m.h>
#include <public/memory.h>
#include <xsm/xsm.h>

#ifdef CONFIG_X86
#include <asm/guest.h>
#endif

struct memop_args {
    /* INPUT */
    struct domain *domain;     /* Domain to be affected. */
    XEN_GUEST_HANDLE(xen_pfn_t) extent_list; /* List of extent base addrs. */
    unsigned int nr_extents;   /* Number of extents to allocate or free. */
    unsigned int extent_order; /* Size of each extent. */
    unsigned int memflags;     /* Allocation flags. */

    /* INPUT/OUTPUT */
    unsigned int nr_done;    /* Number of extents processed so far. */
    int          preempted;  /* Was the hypercall preempted? */
};

#ifndef CONFIG_CTLDOM_MAX_ORDER
#define CONFIG_CTLDOM_MAX_ORDER CONFIG_PAGEALLOC_MAX_ORDER
#endif
#ifndef CONFIG_PTDOM_MAX_ORDER
#define CONFIG_PTDOM_MAX_ORDER CONFIG_HWDOM_MAX_ORDER
#endif

static unsigned int __read_mostly domu_max_order = CONFIG_DOMU_MAX_ORDER;
static unsigned int __read_mostly ctldom_max_order = CONFIG_CTLDOM_MAX_ORDER;
static unsigned int __read_mostly hwdom_max_order = CONFIG_HWDOM_MAX_ORDER;
#ifdef HAS_PASSTHROUGH
static unsigned int __read_mostly ptdom_max_order = CONFIG_PTDOM_MAX_ORDER;
#endif

static int __init parse_max_order(const char *s)
{
    if ( *s != ',' )
        domu_max_order = simple_strtoul(s, &s, 0);
    if ( *s == ',' && *++s != ',' )
        ctldom_max_order = simple_strtoul(s, &s, 0);
    if ( *s == ',' && *++s != ',' )
        hwdom_max_order = simple_strtoul(s, &s, 0);
#ifdef HAS_PASSTHROUGH
    if ( *s == ',' && *++s != ',' )
        ptdom_max_order = simple_strtoul(s, &s, 0);
#endif

    return *s ? -EINVAL : 0;
}
custom_param("memop-max-order", parse_max_order);

static unsigned int max_order(const struct domain *d)
{
    unsigned int order = domu_max_order+3;

#ifdef HAS_PASSTHROUGH
    if ( cache_flush_permitted(d) && order < ptdom_max_order )
        order = ptdom_max_order;
#endif

    if ( is_control_domain(d) && order < ctldom_max_order )
        order = ctldom_max_order;

    if ( is_hardware_domain(d) && order < hwdom_max_order )
        order = hwdom_max_order;

    return min(order, MAX_ORDER + 0U);
}

/* Helper to copy a typesafe MFN to guest */
static inline
unsigned long __copy_mfn_to_guest_offset(XEN_GUEST_HANDLE(xen_pfn_t) hnd,
                                         size_t off, mfn_t mfn)
 {
    xen_pfn_t mfn_ = mfn_x(mfn);

    return __copy_to_guest_offset(hnd, off, &mfn_, 1);
}

static void increase_reservation(struct memop_args *a)
{
    struct page_info *page;
    unsigned long i;
    struct domain *d = a->domain;

    if ( !guest_handle_is_null(a->extent_list) &&
         !guest_handle_subrange_okay(a->extent_list, a->nr_done,
                                     a->nr_extents-1) )
        return;

    if ( a->extent_order > max_order(current->domain) )
        return;

    for ( i = a->nr_done; i < a->nr_extents; i++ )
    {
        if ( i != a->nr_done && hypercall_preempt_check() )
        {
            a->preempted = 1;
            goto out;
        }

        page = alloc_domheap_pages(d, a->extent_order, a->memflags);
        if ( unlikely(page == NULL) ) 
        {
            gdprintk(XENLOG_INFO, "Could not allocate order=%d extent: "
                    "id=%d memflags=%x (%ld of %d)\n",
                     a->extent_order, d->domain_id, a->memflags,
                     i, a->nr_extents);
            goto out;
        }

        /* Inform the domain of the new page's machine address. */ 
        if ( !paging_mode_translate(d) &&
             !guest_handle_is_null(a->extent_list) )
        {
            mfn_t mfn = page_to_mfn(page);

            if ( unlikely(__copy_mfn_to_guest_offset(a->extent_list, i, mfn)) )
                goto out;
        }
    }

 out:
    a->nr_done = i;
}

static void populate_physmap(struct memop_args *a)
{
    struct page_info *page;
    unsigned int i, j, k, node;
    xen_pfn_t gpfn;
    struct domain *d = a->domain, *curr_d = current->domain;
    bool need_tlbflush = false;
    uint32_t tlbflush_timestamp = 0;

    static uint64_t hcall_count = 0;
    static uint64_t non_contig_hcalls = 0;
    static uint64_t totoal_miss_page = 0;
    bool non_contig;

    static uint64_t total_time = 0;
    static uint64_t setup_time = 0;
    static uint64_t alloc_time = 0;
    static uint64_t nid_time = 0;
    static uint64_t other_time_1 = 0;
    static uint64_t other_time_2 = 0;
    uint64_t alloc_start_time = 0;
    uint64_t start_time = rdtsc();
    uint64_t setup_start_time = 0;
    uint64_t nid_start_time = 0;
    uint64_t other_start_time_1 = 0;
    uint64_t other_start_time_2 = 0;

    char mfn_array[512];

    //static unsigned int func_cnt = 0;
    //printk("I'm here at populate_physmap, a->extent_order:: %d\n", a->extent_order);
    //func_cnt++;

    if ( !guest_handle_subrange_okay(a->extent_list, a->nr_done,
                                     a->nr_extents-1) )
        return;

    if ( a->extent_order > (a->memflags & MEMF_populate_on_demand ? MAX_ORDER :
                            max_order(curr_d)) )
        return;

    if ( unlikely(!d->creation_finished) )
    {
        /*
         * With MEMF_no_tlbflush set, alloc_heap_pages() will ignore
         * TLB-flushes. After VM creation, this is a security issue (it can
         * make pages accessible to guest B, when guest A may still have a
         * cached mapping to them). So we do this only during domain creation,
         * when the domain itself has not yet been unpaused for the first
         * time.
         */
        a->memflags |= MEMF_no_tlbflush;
        /*
         * With MEMF_no_icache_flush, alloc_heap_pages() will skip
         * performing icache flushes. We do it only before domain
         * creation as once the domain is running there is a danger of
         * executing instructions from stale caches if icache flush is
         * delayed.
         */
        a->memflags |= MEMF_no_icache_flush;
    }
    
    //if (a->nr_extents != 512 )
	//    printk("I'm here in populate_physmap, a->nr_extents is not 512, %u instead\n",a->nr_extents);
    for (i = 0; i< 512; i++) 
	 mfn_array[i] = 0;   

    nid_start_time = rdtsc();

    for ( i = a->nr_done; i < a->nr_extents; i++ )
    {
        mfn_t mfn;

        if ( i != a->nr_done && hypercall_preempt_check() )
        {
            a->preempted = 1;
            goto out;
        }
	
	other_start_time_1 = rdtsc();
        if ( unlikely(__copy_from_guest_offset(&gpfn, a->extent_list, i, 1)) )
            goto out;
	other_time_1 += rdtsc() - other_start_time_1;

        if ( a->memflags & MEMF_populate_on_demand )
        {
	    //printk("I'm here at populate_physmap, PoD is disabled\n");
            /* Disallow populating PoD pages on oneself. */
            if ( d == curr_d )
                goto out;

            if ( guest_physmap_mark_populate_on_demand(d, gpfn,
                                                       a->extent_order) < 0 )
                goto out;
        }
        else
        {
            if ( is_domain_direct_mapped(d) )
            {
		//printk("I'm here at populate_physmap, domain_direct_mapped.\n");
                mfn = _mfn(gpfn);

                for ( j = 0; j < (1U << a->extent_order); j++,
                      mfn = mfn_add(mfn, 1) )
                {
                    if ( !mfn_valid(mfn) )
                    {
                        gdprintk(XENLOG_INFO, "Invalid mfn %#"PRI_mfn"\n",
                                 mfn_x(mfn));
                        goto out;
                    }

                    page = mfn_to_page(mfn);
                    if ( !get_page(page, d) )
                    {
                        gdprintk(XENLOG_INFO,
                                 "mfn %#"PRI_mfn" doesn't belong to d%d\n",
                                  mfn_x(mfn), d->domain_id);
                        goto out;
                    }
                    put_page(page);
                }

                mfn = _mfn(gpfn);
            }
            else
            {
		//printk("I'm here at populate_physmap about to call alloc_domheap_pages\n");
    		other_start_time_2 = rdtsc();
		alloc_start_time = rdtsc();	
                page = alloc_domheap_pages(d, a->extent_order, a->memflags);
		alloc_time += rdtsc() - alloc_start_time;

                if ( unlikely(!page) )
                {
                    if ( !tmem_enabled() || a->extent_order )
                        gdprintk(XENLOG_INFO,
                                 "Could not allocate order=%u extent: id=%d memflags=%#x (%u of %u)\n",
                                 a->extent_order, d->domain_id, a->memflags,
                                 i, a->nr_extents);
                    goto out;
                }

		if (a->memflags & MEMF_exact_node){
			node = phys_to_nid(page_to_maddr(page));
			if (node != MEMF_get_node(a->memflags))
				printk("I'm here at alloc_domheap_pages"
				", cannot alloc page from node%d, get "
				"node%d instead\n",\
			       	MEMF_get_node(a->memflags), node);
		}

    
    		if ( unlikely(a->memflags & MEMF_no_tlbflush) )
                {
                    for ( j = 0; j < (1U << a->extent_order); j++ )
                        accumulate_tlbflush(&need_tlbflush, &page[j],
                                            &tlbflush_timestamp);
                }

                mfn = page_to_mfn(page);
		// extract the superpage offset from mfn
		//printk("I'm here at populate_physmap, mfn: %lu\n", mfn);
		mfn_array[mfn%512] = 1;
    		other_time_2 += rdtsc() - other_start_time_2;
	    }
	    
	    setup_start_time = rdtsc();
            guest_physmap_add_page(d, _gfn(gpfn), mfn, a->extent_order);
	    setup_time += rdtsc() - setup_start_time;
		
            if ( !paging_mode_translate(d) )
            {
                for ( j = 0; j < (1U << a->extent_order); j++ )
                    set_gpfn_from_mfn(mfn_x(mfn_add(mfn, j)), gpfn + j);

                /* Inform the domain of the new page's machine address. */ 
                if ( unlikely(__copy_mfn_to_guest_offset(a->extent_list, i,
                                                         mfn)) )
                    goto out;
            }
        }
    }
    nid_time += rdtsc() - nid_start_time;

    if (a->nr_extents == 512){
	hcall_count++;
	non_contig = false;
    	for (k=0; k<512; k++){
    		if (mfn_array[k] == 0){
			totoal_miss_page++;
			non_contig = true;
    	    	}
    	}
	if (non_contig){
    		non_contig_hcalls++;
    	    	printk("I'm here at populate_physmap, this call does not alloc contiguous 2m region\n");
	}
    	printk("I'm here at populate_physmap, there are %lu hcalls that allocates 512 pages and %lu of them are not contiguous, total missing page:%lu\n", hcall_count, non_contig_hcalls, totoal_miss_page);
    }

out:
    if ( need_tlbflush )
        filtered_flush_tlb_mask(tlbflush_timestamp);

    if ( a->memflags & MEMF_no_icache_flush )
        invalidate_icache();

    a->nr_done = i;
    total_time += rdtsc() - start_time;
    //printk("I'm here at populate_physpage, total time: %lu, target_time: %lu, alloc_time:%lu, nid_time:%lu, other_time_1:%lu, other_time_2:%lu\n", total_time, setup_time, alloc_time, nid_time, other_time_1, other_time_2);
}

static void populate_physmap_2(struct memop_args *a)
{
    struct page_info *page;
    unsigned int i, j, node;
    //unsigned int i, j, k, node;
    xen_pfn_t gpfn;
    struct domain *d = a->domain, *curr_d = current->domain;
    bool need_tlbflush = false;
    uint32_t tlbflush_timestamp = 0;

    //static uint64_t hcall_count = 0;
    //static uint64_t non_contig_hcalls = 0;
    //static uint64_t totoal_miss_page = 0;
    //bool non_contig;

    static uint64_t total_time = 0;
    static uint64_t setup_time = 0;
    static uint64_t alloc_time = 0;
    static uint64_t nid_time = 0;
    static uint64_t other_time_1 = 0;
    static uint64_t other_time_2 = 0;
    uint64_t alloc_start_time = 0;
    uint64_t start_time = rdtsc();
    uint64_t setup_start_time = 0;
    uint64_t nid_start_time = 0;
    uint64_t other_start_time_1 = 0;
    uint64_t other_start_time_2 = 0;

    //char mfn_array[512];
    //static unsigned int func_cnt = 0;
    printk("I'm here at populate_physmap_2, a->extent_order:: %d\n", a->extent_order);
    //func_cnt++;

    if ( !guest_handle_subrange_okay(a->extent_list, a->nr_done,
                                     a->nr_extents-1) )
    {
	    printk("I'm here in populate_physmap_2, I'm out 1\n");
	    return;
    }

    if ( a->extent_order > (a->memflags & MEMF_populate_on_demand ? MAX_ORDER :
                            max_order(curr_d)) )
    {   printk("I'm here in populate_physmap_2, I'm out 2\n");
        return;
    }

    if ( unlikely(!d->creation_finished) )
    {
        /*
         * With MEMF_no_tlbflush set, alloc_heap_pages() will ignore
         * TLB-flushes. After VM creation, this is a security issue (it can
         * make pages accessible to guest B, when guest A may still have a
         * cached mapping to them). So we do this only during domain creation,
         * when the domain itself has not yet been unpaused for the first
         * time.
         */
        a->memflags |= MEMF_no_tlbflush;
        /*
         * With MEMF_no_icache_flush, alloc_heap_pages() will skip
         * performing icache flushes. We do it only before domain
         * creation as once the domain is running there is a danger of
         * executing instructions from stale caches if icache flush is
         * delayed.
         */
        a->memflags |= MEMF_no_icache_flush;
    }
    
	printk("I'm here in populate_physmap_2, a->nr_extents is %u\n",a->nr_extents);
    //for (i = 0; i< 512; i++) 
    //	 mfn_array[i] = 0;   

    for ( i = a->nr_done; i < a->nr_extents; i++ )
    {
        mfn_t mfn;
	//printk("I'm here at populate_physmap_2, about to enter loop %d\n", i);
        //if ( i != a->nr_done && hypercall_preempt_check() )
        //{
        //    a->preempted = 1;
	//    printk("I'm here in populate_physmap_2, I'm out 3\n");
        //    goto out;
        //}
	
	other_start_time_1 = rdtsc();
        if ( unlikely(__copy_from_guest_offset(&gpfn, a->extent_list, i, 1)) )
	{
	    	printk("I'm here in populate_physmap_2, I'm out 4\n");
		goto out;
	}
	other_time_1 += rdtsc() - other_start_time_1;

        if ( a->memflags & MEMF_populate_on_demand )
        {
	    //printk("I'm here at populate_physmap, PoD is disabled\n");
            /* Disallow populating PoD pages on oneself. */
            if ( d == curr_d )
	    {	
		printk("I'm here in populate_physmap_2, I'm out 5\n");
                goto out;
	    }

            if ( guest_physmap_mark_populate_on_demand(d, gpfn,
                                                       a->extent_order) < 0 )
	    {
	    	printk("I'm here in populate_physmap_2, I'm out 6\n");
		    goto out;
	    }
        }
        else
        {
            if ( is_domain_direct_mapped(d) )
            {
		//printk("I'm here at populate_physmap, domain_direct_mapped.\n");
                mfn = _mfn(gpfn);

                for ( j = 0; j < (1U << a->extent_order); j++,
                      mfn = mfn_add(mfn, 1) )
                {
                    if ( !mfn_valid(mfn) )
                    {
                        gdprintk(XENLOG_INFO, "Invalid mfn %#"PRI_mfn"\n",
                                 mfn_x(mfn));
                        goto out;
                    }

                    page = mfn_to_page(mfn);
                    if ( !get_page(page, d) )
                    {
                        gdprintk(XENLOG_INFO,
                                 "mfn %#"PRI_mfn" doesn't belong to d%d\n",
                                  mfn_x(mfn), d->domain_id);
                        goto out;
                    }
                    put_page(page);
                }

                mfn = _mfn(gpfn);
            }
            else
            {
		//printk("I'm here at populate_physmap about to call alloc_domheap_pages, order%u\n", a->extent_order);
    		//other_start_time_2 = rdtsc();
		alloc_start_time = rdtsc();	
                page = alloc_domheap_pages(d, a->extent_order, a->memflags);
		alloc_time += rdtsc() - alloc_start_time;

                if ( unlikely(!page) )
                {
                    if ( !tmem_enabled() || a->extent_order )
                        gdprintk(XENLOG_INFO,
                                 "Could not allocate order=%u extent: id=%d memflags=%#x (%u of %u)\n",
                                 a->extent_order, d->domain_id, a->memflags,
                                 i, a->nr_extents);
		    {
			    printk("I'm here in populate_physmap_2, I'm out 7\n");
			    goto out;
		    }
                }

    nid_start_time = rdtsc();
		if (a->memflags & MEMF_exact_node){
			node = phys_to_nid(page_to_maddr(page));
			if (node != MEMF_get_node(a->memflags))
				printk("I'm here at alloc_domheap_pages"
				", cannot alloc page from node%d, get "
				"node%d instead\n",\
			       	MEMF_get_node(a->memflags), node);
		}

    
    		if ( unlikely(a->memflags & MEMF_no_tlbflush) )
                {
                    for ( j = 0; j < (1U << a->extent_order); j++ )
                        accumulate_tlbflush(&need_tlbflush, &page[j],
                                            &tlbflush_timestamp);
                }

                mfn = page_to_mfn(page);
    nid_time += rdtsc() - nid_start_time;
		// extract the superpage offset from mfn
		//printk("I'm here at populate_physmap, mfn: %lu\n", mfn);
		//mfn_array[mfn%512] = 1;
    		//other_time_2 += rdtsc() - other_start_time_2;
	    }
	    
	    setup_start_time = rdtsc();
            guest_physmap_add_page(d, _gfn(gpfn), mfn, a->extent_order);
	    setup_time += rdtsc() - setup_start_time;
    other_start_time_2 = rdtsc();
		
            if ( !paging_mode_translate(d) )
            {
                for ( j = 0; j < (1U << a->extent_order); j++ )
                    set_gpfn_from_mfn(mfn_x(mfn_add(mfn, j)), gpfn + j);

                /* Inform the domain of the new page's machine address. */ 
                if ( unlikely(__copy_mfn_to_guest_offset(a->extent_list, i,
                                                         mfn)) )
		{
			printk("I'm here about to go out\n");
			goto out;
		}
            }
    other_time_2 += rdtsc() - other_start_time_2;
        }
    }

//    if (a->nr_extents == 512){
//	hcall_count++;
//	non_contig = false;
//    	for (k=0; k<512; k++){
//    		if (mfn_array[k] == 0){
//			totoal_miss_page++;
//			non_contig = true;
//    	    	}
//    	}
//	if (non_contig){
//    		non_contig_hcalls++;
//    	    	printk("I'm here at populate_physmap, this call does not alloc contiguous 2m region\n");
//	}
    	//printk("I'm here at populate_physmap, there are %lu hcalls that allocates 512 pages and %lu of them are not contiguous, total missing page:%lu\n", hcall_count, non_contig_hcalls, totoal_miss_page);
//    }

out:
    if ( need_tlbflush )
        filtered_flush_tlb_mask(tlbflush_timestamp);

    if ( a->memflags & MEMF_no_icache_flush )
        invalidate_icache();

    a->nr_done = i;
    total_time += rdtsc() - start_time;
    printk("I'm here at populate_physpage_2, total time: %lu, target_time: %lu, alloc_time:%lu, nid_time:%lu, other_time_1:%lu, other_time_2:%lu\n", total_time, setup_time, alloc_time, nid_time, other_time_1, other_time_2);
}

int guest_remove_page(struct domain *d, unsigned long gmfn)
{
    
   static uint64_t total_time = 0;
   static uint64_t setup_time = 0;
   uint64_t start_time = rdtsc();
   uint64_t setup_start_time = 0;

   static uint64_t alloc_time = 0;
   static uint64_t nid_time = 0;
   static uint64_t other_time_1 = 0;
   static uint64_t other_time_2 = 0;
   uint64_t alloc_start_time = 0;


   uint64_t nid_start_time = 0;
   uint64_t other_start_time_1 = 0;
   uint64_t other_start_time_2 = 0;
    
    struct page_info *page;
#ifdef CONFIG_X86
    p2m_type_t p2mt;
#endif
    mfn_t mfn;

    int rc;
    unsigned long total_pages;
    unsigned int page_order;
    
    //mfn_t mfn2;
    //p2m_type_t p2mt2;
    //unsigned int page_order2;

    total_pages = d->tot_pages;
#ifdef CONFIG_X86
	//printk("I'm here at guest_remove_page, p2m->defer_flush 1: %u\n", p2m_get_hostp2m((d))->defer_flush);

    other_start_time_1 = rdtsc();
    mfn = get_gfn_query_with_order(d, gmfn, &p2mt, &page_order);
    //mfn2 = get_gfn_query_with_order(d, gmfn+1, &p2mt2, &page_order2);
    //printk("In guest_remove_page, gfn:%lu, page_order from get_gfn_query: %u, mfn: %lu\n", gmfn, page_order, mfn);
    //printk("In guest_remove_page, gfn2:%lu, page_order2 from get_gfn_query: %u, mfn2: %lu\n", gmfn+1, page_order2, mfn2);
    other_time_1 += rdtsc() - other_start_time_1;
    
    if ( unlikely(p2mt == p2m_invalid) || unlikely(p2mt == p2m_mmio_dm) )
        return -ENOENT;

    if ( unlikely(p2m_is_paging(p2mt)) )
    {
        printk("I'm here at guest_remove_page, p2m is paging\n");
	    rc = guest_physmap_remove_page(d, _gfn(gmfn), mfn, 0);
        if ( rc )
            goto out_put_gfn;

        put_gfn(d, gmfn);

        /* If the page hasn't yet been paged out, there is an
         * actual page that needs to be released. */
        if ( p2mt == p2m_ram_paging_out )
        {
            ASSERT(mfn_valid(mfn));
            page = mfn_to_page(mfn);
            if ( test_and_clear_bit(_PGC_allocated, &page->count_info) )
                put_page(page);
        }
        p2m_mem_paging_drop_page(d, gmfn, p2mt);

        return 0;
    }
    if ( p2mt == p2m_mmio_direct )
    {
        rc = clear_mmio_p2m_entry(d, gmfn, mfn, PAGE_ORDER_4K);
        goto out_put_gfn;
    }
#else
    mfn = gfn_to_mfn(d, _gfn(gmfn));
#endif
    if ( unlikely(!mfn_valid(mfn)) )
    {
        put_gfn(d, gmfn);
        gdprintk(XENLOG_INFO, "Domain %u page number %lx invalid\n",
                d->domain_id, gmfn);

        return -EINVAL;
    }
            
#ifdef CONFIG_X86
    if ( p2m_is_shared(p2mt) )
    {
        /*
         * Unshare the page, bail out on error. We unshare because we
         * might be the only one using this shared page, and we need to
         * trigger proper cleanup. Once done, this is like any other page.
         */
        printk("I'm here at guest_remove_page, p2m is shared\n");
        rc = mem_sharing_unshare_page(d, gmfn, 0);
        if ( rc )
        {
            (void)mem_sharing_notify_enomem(d, gmfn, 0);
            goto out_put_gfn;
        }
        /* Maybe the mfn changed */
        mfn = get_gfn_query_unlocked(d, gmfn, &p2mt);
        ASSERT(!p2m_is_shared(p2mt));
    }
#endif /* CONFIG_X86 */
	//printk("I'm here at guest_remove_page, p2m->defer_flush 2: %u\n", p2m_get_hostp2m((d))->defer_flush);

    other_start_time_2 = rdtsc();
    page = mfn_to_page(mfn);
    other_time_2 += rdtsc() - other_start_time_2;

    //if( (page->count_info & PGC_count_mask) != 2)
    //	printk("I'm here at guest_remove_page, page->count_info:%lu\n",page->count_info);
    
    if ( unlikely(!get_page(page, d)) )
    {
        put_gfn(d, gmfn);
        gdprintk(XENLOG_INFO, "Bad page free for domain %u\n", d->domain_id);

        return -ENXIO;
    }
	//printk("I'm here at guest_remove_page, p2m->defer_flush 3: %u\n", p2m_get_hostp2m((d))->defer_flush);
    setup_start_time= rdtsc();
    rc = guest_physmap_remove_page(d, _gfn(gmfn), mfn, 0);
    setup_time += rdtsc() - setup_start_time;
	//if (d->tot_pages != total_pages)
	//	printk("I'm here at guest_remove_page, tot_pages changed\n");

	//printk("I'm here at guest_remove_page, p2m->defer_flush 4: %u\n", p2m_get_hostp2m((d))->defer_flush);
    
    
    /*
     * With the lack of an IOMMU on some platforms, domains with DMA-capable
     * device must retrieve the same pfn when the hypercall populate_physmap
     * is called.
     *
     * For this purpose (and to match populate_physmap() behavior), the page
     * is kept allocated.
     */
    //if( (page->count_info & PGC_count_mask) != 3)
    //	printk("I'm here at guest_remove_page, page->count_info 2:%lu\n",page->count_info);
    alloc_start_time = rdtsc();
    if ( !rc && !is_domain_direct_mapped(d) &&
         test_and_clear_bit(_PGC_allocated, &page->count_info) )
        put_page(page);
	
    //if (d->tot_pages != total_pages)
//		printk("I'm here at guest_remove_page, tot_pages changed 1\n");
		
    //if( (page->count_info & PGC_count_mask) != 2)
    //	printk("I'm here at guest_remove_page, page->count_info 3:%lu\n",page->count_info);

    put_page(page);
    alloc_time += rdtsc() - alloc_start_time;
	//if (p2mt->defer_flush){
	//printk("I'm here at guest_remove_page, p2m->defer_flush 5: %u\n", p2m_get_hostp2m((d))->defer_flush);
		
    	//	printk("I'm here at guest_remove_page, page->count_info:%lu\n",page->count_info);
	//}
 out_put_gfn: __maybe_unused
    nid_start_time = rdtsc();
    put_gfn(d, gmfn);
    nid_time += rdtsc() - nid_start_time;
	//if (d->tot_pages != total_pages)
	//	printk("I'm here at guest_remove_page, tot_pages changed 3\n");
    
    total_time += rdtsc() - start_time;
    //if (gmfn%500 == 0)
    //printk("I'm here at guest_remove_page, total time: %lu, target_time: %lu, other_time1: %lu, other_time2: %lu, alloc_time: %lu, nid_time:%lu\n", total_time, setup_time, other_time_1, other_time_2, alloc_time, nid_time );
    /*
     * Filter out -ENOENT return values that aren't a result of an empty p2m
     * entry.
     */
    return rc != -ENOENT ? rc : -EINVAL;
}

int guest_remove_page_with_order(struct domain *d, unsigned long gmfn, unsigned int order, unsigned int* p2m_order)
{
	//TODO: change guest_remove_page to use larger order
	// main taskes from guest_remove_page: 
	// 1. check p2m_is_paging
	// 2. check p2mt == p2m_mmio_direct
	// 3. check mfn_valid(mfn)
	// 4. check p2m_is_shared(p2mt)
	// 5. page = mfn_to_page(mfn);
	// 6. guest_physmap_remove_page iterates through each 4k pages
	// 7. if !is_domain_direct_mapped, put_page(page);
	// 8. put_page(page);
	// 9. put_gfn(d, gmfn);
   static uint64_t total_time = 0;
   static uint64_t setup_time = 0;
   uint64_t start_time = rdtsc();
   uint64_t setup_start_time = 0;

   static uint64_t alloc_time = 0;
   static uint64_t nid_time = 0;
   static uint64_t other_time_1 = 0;
   static uint64_t other_time_2 = 0;
   uint64_t alloc_start_time = 0;


   uint64_t nid_start_time = 0;
   uint64_t other_start_time_1 = 0;
   uint64_t other_start_time_2 = 0;
    
    struct page_info *page;
#ifdef CONFIG_X86
    p2m_type_t p2mt;
#endif
    mfn_t mfn;

    int rc;
    unsigned long total_pages;
    unsigned int page_order;
    //unsigned int ref_cnt_4k;
    
    total_pages = d->tot_pages;
#ifdef CONFIG_X86

    other_start_time_1 = rdtsc();
    mfn = get_gfn_query_with_order(d, gmfn, &p2mt, &page_order);
    other_time_1 += rdtsc() - other_start_time_1;
    *p2m_order = page_order;
    
    //printk("In guest_remove_page_w_roder, gfn:%lu, page_order from get_gfn_query: %u, mfn: %lu\n", gmfn, page_order, mfn);
    
    if (page_order < order){
    	order = page_order;
    }


    
    if ( unlikely(p2mt == p2m_invalid) || unlikely(p2mt == p2m_mmio_dm) )
        return -ENOENT;

    if ( unlikely(p2m_is_paging(p2mt)) )
    {
        printk("In guest_remove_page, p2m is paging, potential ERROR\n");
        return 0;
    }
    if ( p2mt == p2m_mmio_direct )
    {
	printk("In guest_remove_page, p2m is p2m_mmio_direct, potential ERROR\n");
        rc = clear_mmio_p2m_entry(d, gmfn, mfn, page_order);
        goto out_put_gfn;
    }
#else
    mfn = gfn_to_mfn(d, _gfn(gmfn));
#endif
    if ( unlikely(!mfn_valid(mfn)) )
    {
        put_gfn(d, gmfn);
        gdprintk(XENLOG_INFO, "Domain %u page number %lx invalid\n",
                d->domain_id, gmfn);

        return -EINVAL;
    }
            
#ifdef CONFIG_X86
    if ( p2m_is_shared(p2mt) )
    {
        /*
         * Unshare the page, bail out on error. We unshare because we
         * might be the only one using this shared page, and we need to
         * trigger proper cleanup. Once done, this is like any other page.
         */
        printk("I'm here at guest_remove_page, p2m is shared, potential ERROR\n");
    }
#endif /* CONFIG_X86 */

    other_start_time_2 = rdtsc();
    page = mfn_to_page(mfn);
    other_time_2 += rdtsc() - other_start_time_2;
    

    if ( unlikely(!get_page(page, d)) )
    {
        put_gfn(d, gmfn);
        gdprintk(XENLOG_INFO, "Bad page free for domain %u\n", d->domain_id);

        return -ENXIO;
    }
    //printk("In guest_remove_page_w_roder, stamp1 ref count of first page %lu\n", ((page->count_info) & PGC_count_mask));
    //if(order > 0)
    //	printk("In guest_remove_page_w_roder, stamp1 ref count of secondt page %lu\n", (((page+1)->count_info) & PGC_count_mask));

    
    setup_start_time= rdtsc();
    rc = guest_physmap_remove_page(d, _gfn(gmfn), mfn, order);
    setup_time += rdtsc() - setup_start_time;
    
    
    //printk("In guest_remove_page_w_roder, stamp2 ref count of first page %lu\n", ((page->count_info) & PGC_count_mask));
    //if(order > 0)
    //	printk("In guest_remove_page_w_roder, stamp2 ref count of secondt page %lu\n", (((page+1)->count_info) & PGC_count_mask));
    
    /*
     * With the lack of an IOMMU on some platforms, domains with DMA-capable
     * device must retrieve the same pfn when the hypercall populate_physmap
     * is called.
     *
     * For this purpose (and to match populate_physmap() behavior), the page
     * is kept allocated.
     */
    alloc_start_time = rdtsc();
    
    put_page(page);

    /* make sure didn't free the first page in the last put_page func */
    ASSERT( ((page->count_info) & PGC_count_mask) != 0 );

    if ( !rc && !is_domain_direct_mapped(d) ){ 
        put_page_with_order(page, order);
    }
	
		
    alloc_time += rdtsc() - alloc_start_time;
 

out_put_gfn: __maybe_unused
    nid_start_time = rdtsc();
    put_gfn(d, gmfn);
    nid_time += rdtsc() - nid_start_time;
    
    total_time += rdtsc() - start_time;
    if (gmfn%500 == 0)
    printk("I'm here at guest_remove_page, total time: %lu, target_time: %lu, other_time1: %lu, other_time2: %lu, alloc_time: %lu, nid_time:%lu\n", total_time, setup_time, other_time_1, other_time_2, alloc_time, nid_time );
    /*
     * Filter out -ENOENT return values that aren't a result of an empty p2m
     * entry.
     */
    return rc != -ENOENT ? rc : -EINVAL;
    
}

static void decrease_reservation(struct memop_args *a)
{
    unsigned long i, j;
    xen_pfn_t gmfn;
    //static uint64_t total_time = 0;
    //static uint64_t setup_time = 0;
    //static uint64_t copy_time = 0;
    //static uint64_t nid_time = 0;
    //static uint64_t other_time_1 = 0;
    //static uint64_t other_time_2 = 0;
    //uint64_t copy_start_time = 0;
    //uint64_t start_time = rdtsc();
    //uint64_t setup_start_time = 0;
    //uint64_t nid_start_time = 0;
    //uint64_t other_start_time_1 = 0;
    //uint64_t other_start_time_2 = 0;


        //printk("I'm in decrease_reservation, nr_extents: %u\n", a->nr_extents);

    if ( !guest_handle_subrange_okay(a->extent_list, a->nr_done,
                                     a->nr_extents-1) ||
         a->extent_order > max_order(current->domain) ){
        printk("I'm in decrease_reservation, return because max_order: %u\n", max_order(current->domain));
	    return;
    }

    for ( i = a->nr_done; i < a->nr_extents; i++ )
    {
        unsigned long pod_done;

        if ( i != a->nr_done && hypercall_preempt_check() )
        {
            a->preempted = 1;
            goto out;
        }
	//copy_start_time = rdtsc(); 
        if ( unlikely(__copy_from_guest_offset(&gmfn, a->extent_list, i, 1)) )
            goto out;
	//copy_time += rdtsc() - copy_start_time;

        if ( tb_init_done )
        {
            struct {
                u64 gfn;
                int d:16,order:16;
            } t;

            t.gfn = gmfn;
            t.d = a->domain->domain_id;
            t.order = a->extent_order;
        
            __trace_var(TRC_MEM_DECREASE_RESERVATION, 0, sizeof(t), &t);
        }

        /* See if populate-on-demand wants to handle this */
        pod_done = is_hvm_domain(a->domain) ?
                   p2m_pod_decrease_reservation(a->domain, _gfn(gmfn),
                                                a->extent_order) : 0;

        /*
         * Look for pages not handled by p2m_pod_decrease_reservation().
         *
         * guest_remove_page() will return -ENOENT for pages which have already
         * been removed by p2m_pod_decrease_reservation(); so expect to see
         * exactly pod_done failures.  Any more means that there were invalid
         * entries before p2m_pod_decrease_reservation() was called.
         */
	for ( j = 0; j + pod_done < (1UL << a->extent_order); j++ )
        {
            //setup_start_time = rdtsc();
            switch ( guest_remove_page(a->domain, gmfn + j) )
            {
            case 0:
                break;
            case -ENOENT:
                if ( !pod_done )
        	printk("I'm in decrease_reservation, out 1\n");
                    goto out;
                --pod_done;
                break;
            default:
        	printk("I'm in decrease_reservation, out 2\n");
                goto out;
            }
	    //setup_time += rdtsc() - setup_start_time; 
        }
    }

 out:
    a->nr_done = i;
    //total_time += rdtsc() - start_time;
    //printk("I'm here at decrease_reservation, total time: %lu, target_time: %lu, copy_time:%lu, nid_time:%lu, other_time_3:%lu, other_time_2:%lu\n", total_time, setup_time, copy_time, nid_time, other_time_1, other_time_2);
}

static void decrease_reservation_2(struct memop_args *a)
{
	//p2m_type_t p2mt;
	//mfn_t mfn;
	//struct page_info *page;
    unsigned long i;
    unsigned long j;
    unsigned int p2m_order;
    xen_pfn_t gmfn;
    int ret=0;
    static uint64_t total_time = 0;
    static uint64_t setup_time = 0;
    static uint64_t copy_time = 0;
    static uint64_t nid_time = 0;
    static uint64_t other_time_1 = 0;
    static uint64_t other_time_2 = 0;
    uint64_t copy_start_time = 0;
    uint64_t start_time = rdtsc();
    
    //uint64_t setup_start_time = 0;
    //uint64_t nid_start_time = 0;
    //uint64_t other_start_time_1 = 0;
    //uint64_t other_start_time_2 = 0;

        //printk("I'm in decrease_reservation_2, nr_extents: %u, order:%d\n", a->nr_extents, a->extent_order);


    if ( !guest_handle_subrange_okay(a->extent_list, a->nr_done,
                                     a->nr_extents-1) ||
         a->extent_order > max_order(current->domain) ){
        printk("I'm in decrease_reservation_2, return because max_order: %u\n", max_order(current->domain));
        return;
    }
        //printk("I'm in decrease_reservation_2, d->tot_pages:%d, d->max_pages:%d\n", a->domain->tot_pages, a->domain->max_pages);

    for ( i = a->nr_done; i < a->nr_extents; i++ )
    {
        unsigned long pod_done;

	copy_start_time = rdtsc(); 
        if ( unlikely(__copy_from_guest_offset(&gmfn, a->extent_list, i, 1)) ){
         	printk("I'm here at decrease_reservation_2, copy_from_guest error, out, i=%lu\n",i);
	     	goto out;
	}

	copy_time += rdtsc() - copy_start_time;

        if ( tb_init_done )
        {
            struct {
                u64 gfn;
                int d:16,order:16;
            } t;

            t.gfn = gmfn;
            t.d = a->domain->domain_id;
            t.order = a->extent_order;
        
            __trace_var(TRC_MEM_DECREASE_RESERVATION, 0, sizeof(t), &t);
        }

        /* See if populate-on-demand wants to handle this */
        pod_done = is_hvm_domain(a->domain) ?
                   p2m_pod_decrease_reservation(a->domain, _gfn(gmfn),
                                                a->extent_order) : 0;

        /*
         * Look for pages not handled by p2m_pod_decrease_reservation().
         *
         * guest_remove_page() will return -ENOENT for pages which have already
         * been removed by p2m_pod_decrease_reservation(); so expect to see
         * exactly pod_done failures.  Any more means that there were invalid
         * entries before p2m_pod_decrease_reservation() was called.
         */


	//p2m_get_hostp2m(a->domain)->defer_flush = ((1UL << a->extent_order) - 1) *2;
	p2m_get_hostp2m(a->domain)->defer_flush = 2;
	ret = guest_remove_page_with_order(a->domain, gmfn, 
			a->extent_order, &p2m_order);
	if (ret)
		printk("In decrease_reservation_2, first guest_remove_page ret %d\n", ret);	
	/* handle the case where p2m entry order P is smaller than balloon order B,
	 * fall back to remove one P order page at a time */
	//printk("In decrease_reservation_2, p2m order: %u, order: %u\n", p2m_order, a->extent_order);	

	if (p2m_order < a->extent_order){
		unsigned int remainder_order = a->extent_order - p2m_order;
		printk("In decrease_reservation_2, p2m order: %u, smaller than requested order: %u\n", p2m_order, a->extent_order);	
		p2m_get_hostp2m(a->domain)->defer_flush = 
			((1UL << remainder_order) - 1) *2;

		for (j = 1; j < (1UL << remainder_order); j++){
			ret = guest_remove_page_with_order(a->domain, 
					gmfn + j*(1UL << p2m_order), 
					p2m_order, &p2m_order);
        		if (ret)
                		printk("In decrease_reservation_2, second guest_remove_page ret %d\n", ret);
		}	
	}
    }

 out:
    //printk("I'm in decrease_reservation_2 out, d->tot_pages:%d, d->max_pages:%d\n", a->domain->tot_pages, a->domain->max_pages);
    a->nr_done = i;
    total_time += rdtsc() - start_time;
    if (a->nr_done == a->nr_extents)
    	printk("I'm here at decrease_reservation_2, total time: %lu, target_time: %lu, copy_time:%lu, nid_time:%lu, other_time_3:%lu, other_time_2:%lu, a->done:%u\n", total_time, setup_time, copy_time, nid_time, other_time_1, other_time_2, a->nr_done);
}

static int migrate_page_range(struct domain *domain, struct xen_memory_range_migration* migration)
{
	return guest_migrate_page_range(domain, migration->start_addr, migration->end_addr, (int)migration->move_to_node);	
}

static bool propagate_node(unsigned int xmf, unsigned int *memflags)
{
    const struct domain *currd = current->domain;

    BUILD_BUG_ON(XENMEMF_get_node(0) != NUMA_NO_NODE);
    BUILD_BUG_ON(MEMF_get_node(0) != NUMA_NO_NODE);

    if ( XENMEMF_get_node(xmf) == NUMA_NO_NODE )
        return true;

    if ( is_hardware_domain(currd) || is_control_domain(currd) )
    {
        if ( XENMEMF_get_node(xmf) >= MAX_NUMNODES )
            return false;

        *memflags |= MEMF_node(XENMEMF_get_node(xmf));
        if ( xmf & XENMEMF_exact_node_request )
            *memflags |= MEMF_exact_node;
    }
    else if ( xmf & XENMEMF_exact_node_request )
        return false;

    return true;
}

static long memory_exchange(XEN_GUEST_HANDLE_PARAM(xen_memory_exchange_t) arg)
{
    struct xen_memory_exchange exch;
    PAGE_LIST_HEAD(in_chunk_list);
    PAGE_LIST_HEAD(out_chunk_list);
    unsigned long in_chunk_order, out_chunk_order;
    xen_pfn_t     gpfn, gmfn;
    mfn_t         mfn;
    unsigned long i, j, k;
    unsigned int  memflags = 0;
    long          rc = 0;
    struct domain *d;
    struct page_info *page;

    if ( copy_from_guest(&exch, arg, 1) )
        return -EFAULT;

    if ( max(exch.in.extent_order, exch.out.extent_order) >
         max_order(current->domain) )
    {
        rc = -EPERM;
        goto fail_early;
    }

    /* Various sanity checks. */
    if ( (exch.nr_exchanged > exch.in.nr_extents) ||
         /* Input and output domain identifiers match? */
         (exch.in.domid != exch.out.domid) ||
         /* Sizes of input and output lists do not overflow a long? */
         ((~0UL >> exch.in.extent_order) < exch.in.nr_extents) ||
         ((~0UL >> exch.out.extent_order) < exch.out.nr_extents) ||
         /* Sizes of input and output lists match? */
         ((exch.in.nr_extents << exch.in.extent_order) !=
          (exch.out.nr_extents << exch.out.extent_order)) )
    {
        rc = -EINVAL;
        goto fail_early;
    }

    if ( !guest_handle_subrange_okay(exch.in.extent_start, exch.nr_exchanged,
                                     exch.in.nr_extents - 1) )
    {
        rc = -EFAULT;
        goto fail_early;
    }

    if ( exch.in.extent_order <= exch.out.extent_order )
    {
        in_chunk_order  = exch.out.extent_order - exch.in.extent_order;
        out_chunk_order = 0;

        if ( !guest_handle_subrange_okay(exch.out.extent_start,
                                         exch.nr_exchanged >> in_chunk_order,
                                         exch.out.nr_extents - 1) )
        {
            rc = -EFAULT;
            goto fail_early;
        }
    }
    else
    {
        in_chunk_order  = 0;
        out_chunk_order = exch.in.extent_order - exch.out.extent_order;

        if ( !guest_handle_subrange_okay(exch.out.extent_start,
                                         exch.nr_exchanged << out_chunk_order,
                                         exch.out.nr_extents - 1) )
        {
            rc = -EFAULT;
            goto fail_early;
        }
    }

    if ( unlikely(!propagate_node(exch.out.mem_flags, &memflags)) )
    {
        rc = -EINVAL;
        goto fail_early;
    }

    d = rcu_lock_domain_by_any_id(exch.in.domid);
    if ( d == NULL )
    {
        rc = -ESRCH;
        goto fail_early;
    }

    rc = xsm_memory_exchange(XSM_TARGET, d);
    if ( rc )
    {
        rcu_unlock_domain(d);
        goto fail_early;
    }

    memflags |= MEMF_bits(domain_clamp_alloc_bitsize(
        d,
        XENMEMF_get_address_bits(exch.out.mem_flags) ? :
        (BITS_PER_LONG+PAGE_SHIFT)));

    for ( i = (exch.nr_exchanged >> in_chunk_order);
          i < (exch.in.nr_extents >> in_chunk_order);
          i++ )
    {
        if ( i != (exch.nr_exchanged >> in_chunk_order) &&
             hypercall_preempt_check() )
        {
            exch.nr_exchanged = i << in_chunk_order;
            rcu_unlock_domain(d);
            if ( __copy_field_to_guest(arg, &exch, nr_exchanged) )
                return -EFAULT;
            return hypercall_create_continuation(
                __HYPERVISOR_memory_op, "lh", XENMEM_exchange, arg);
        }

        /* Steal a chunk's worth of input pages from the domain. */
        for ( j = 0; j < (1UL << in_chunk_order); j++ )
        {
            if ( unlikely(__copy_from_guest_offset(
                &gmfn, exch.in.extent_start, (i<<in_chunk_order)+j, 1)) )
            {
                rc = -EFAULT;
                goto fail;
            }

            for ( k = 0; k < (1UL << exch.in.extent_order); k++ )
            {
#ifdef CONFIG_X86
                p2m_type_t p2mt;

                /* Shared pages cannot be exchanged */
                mfn = get_gfn_unshare(d, gmfn + k, &p2mt);
                if ( p2m_is_shared(p2mt) )
                {
                    put_gfn(d, gmfn + k);
                    rc = -ENOMEM;
                    goto fail; 
                }
#else /* !CONFIG_X86 */
                mfn = gfn_to_mfn(d, _gfn(gmfn + k));
#endif
                if ( unlikely(!mfn_valid(mfn)) )
                {
                    put_gfn(d, gmfn + k);
                    rc = -EINVAL;
                    goto fail;
                }

                page = mfn_to_page(mfn);

                rc = steal_page(d, page, MEMF_no_refcount);
                if ( unlikely(rc) )
                {
                    put_gfn(d, gmfn + k);
                    goto fail;
                }

                page_list_add(page, &in_chunk_list);
                put_gfn(d, gmfn + k);
            }
        }

        /* Allocate a chunk's worth of anonymous output pages. */
        for ( j = 0; j < (1UL << out_chunk_order); j++ )
        {
            page = alloc_domheap_pages(d, exch.out.extent_order,
                                       MEMF_no_owner | memflags);
            if ( unlikely(page == NULL) )
            {
                rc = -ENOMEM;
                goto fail;
            }

            page_list_add(page, &out_chunk_list);
        }

        /*
         * Success! Beyond this point we cannot fail for this chunk.
         */

        /* Destroy final reference to each input page. */
        while ( (page = page_list_remove_head(&in_chunk_list)) )
        {
            unsigned long gfn;

            if ( !test_and_clear_bit(_PGC_allocated, &page->count_info) )
                BUG();
            mfn = page_to_mfn(page);
            gfn = mfn_to_gmfn(d, mfn_x(mfn));
            /* Pages were unshared above */
            BUG_ON(SHARED_M2P(gfn));
            if ( guest_physmap_remove_page(d, _gfn(gfn), mfn, 0) )
                domain_crash(d);
            put_page(page);
        }

        /* Assign each output page to the domain. */
        for ( j = 0; (page = page_list_remove_head(&out_chunk_list)); ++j )
        {
            if ( assign_pages(d, page, exch.out.extent_order,
                              MEMF_no_refcount) )
            {
                unsigned long dec_count;
                bool_t drop_dom_ref;

                /*
                 * Pages in in_chunk_list is stolen without
                 * decreasing the tot_pages. If the domain is dying when
                 * assign pages, we need decrease the count. For those pages
                 * that has been assigned, it should be covered by
                 * domain_relinquish_resources().
                 */
                dec_count = (((1UL << exch.in.extent_order) *
                              (1UL << in_chunk_order)) -
                             (j * (1UL << exch.out.extent_order)));

                spin_lock(&d->page_alloc_lock);
                drop_dom_ref = (dec_count &&
                                !domain_adjust_tot_pages(d, -dec_count));
                spin_unlock(&d->page_alloc_lock);

                if ( drop_dom_ref )
                    put_domain(d);

                free_domheap_pages(page, exch.out.extent_order);
                goto dying;
            }

            if ( __copy_from_guest_offset(&gpfn, exch.out.extent_start,
                                          (i << out_chunk_order) + j, 1) )
            {
                rc = -EFAULT;
                continue;
            }

            mfn = page_to_mfn(page);
            guest_physmap_add_page(d, _gfn(gpfn), mfn,
                                   exch.out.extent_order);

            if ( !paging_mode_translate(d) )
            {
                for ( k = 0; k < (1UL << exch.out.extent_order); k++ )
                    set_gpfn_from_mfn(mfn_x(mfn_add(mfn, k)), gpfn + k);
                if ( __copy_mfn_to_guest_offset(exch.out.extent_start,
                                                (i << out_chunk_order) + j,
                                                mfn) )
                    rc = -EFAULT;
            }
        }
        BUG_ON( !(d->is_dying) && (j != (1UL << out_chunk_order)) );

        if ( rc )
            goto fail;
    }

    exch.nr_exchanged = exch.in.nr_extents;
    if ( __copy_field_to_guest(arg, &exch, nr_exchanged) )
        rc = -EFAULT;
    rcu_unlock_domain(d);
    return rc;

    /*
     * Failed a chunk! Free any partial chunk work. Tell caller how many
     * chunks succeeded.
     */
 fail:
    /* Reassign any input pages we managed to steal. */
    while ( (page = page_list_remove_head(&in_chunk_list)) )
        if ( assign_pages(d, page, 0, MEMF_no_refcount) )
        {
            BUG_ON(!d->is_dying);
            if ( test_and_clear_bit(_PGC_allocated, &page->count_info) )
                put_page(page);
        }

 dying:
    rcu_unlock_domain(d);
    /* Free any output pages we managed to allocate. */
    while ( (page = page_list_remove_head(&out_chunk_list)) )
        free_domheap_pages(page, exch.out.extent_order);

    exch.nr_exchanged = i << in_chunk_order;

 fail_early:
    if ( __copy_field_to_guest(arg, &exch, nr_exchanged) )
        rc = -EFAULT;
    return rc;
}

int xenmem_add_to_physmap(struct domain *d, struct xen_add_to_physmap *xatp,
                          unsigned int start)
{
    unsigned int done = 0;
    long rc = 0;
    union xen_add_to_physmap_batch_extra extra;

    if ( xatp->space != XENMAPSPACE_gmfn_foreign )
        extra.res0 = 0;
    else
        extra.foreign_domid = DOMID_INVALID;

    if ( xatp->space != XENMAPSPACE_gmfn_range )
        return xenmem_add_to_physmap_one(d, xatp->space, extra,
                                         xatp->idx, _gfn(xatp->gpfn));

    if ( xatp->size < start )
        return -EILSEQ;

    xatp->idx += start;
    xatp->gpfn += start;
    xatp->size -= start;

#ifdef CONFIG_HAS_PASSTHROUGH
    if ( need_iommu(d) )
        this_cpu(iommu_dont_flush_iotlb) = 1;
#endif

    while ( xatp->size > done )
    {
        rc = xenmem_add_to_physmap_one(d, XENMAPSPACE_gmfn, extra,
                                       xatp->idx, _gfn(xatp->gpfn));
        if ( rc < 0 )
            break;

        xatp->idx++;
        xatp->gpfn++;

        /* Check for continuation if it's not the last iteration. */
        if ( xatp->size > ++done && hypercall_preempt_check() )
        {
            rc = start + done;
            break;
        }
    }

#ifdef CONFIG_HAS_PASSTHROUGH
    if ( need_iommu(d) )
    {
        int ret;

        this_cpu(iommu_dont_flush_iotlb) = 0;

        ret = iommu_iotlb_flush(d, xatp->idx - done, done);
        if ( unlikely(ret) && rc >= 0 )
            rc = ret;

        ret = iommu_iotlb_flush(d, xatp->gpfn - done, done);
        if ( unlikely(ret) && rc >= 0 )
            rc = ret;
    }
#endif

    return rc;
}

static int xenmem_add_to_physmap_batch(struct domain *d,
                                       struct xen_add_to_physmap_batch *xatpb,
                                       unsigned int extent)
{
    if ( xatpb->size < extent )
        return -EILSEQ;

    if ( !guest_handle_subrange_okay(xatpb->idxs, extent, xatpb->size - 1) ||
         !guest_handle_subrange_okay(xatpb->gpfns, extent, xatpb->size - 1) ||
         !guest_handle_subrange_okay(xatpb->errs, extent, xatpb->size - 1) )
        return -EFAULT;

    while ( xatpb->size > extent )
    {
        xen_ulong_t idx;
        xen_pfn_t gpfn;
        int rc;

        if ( unlikely(__copy_from_guest_offset(&idx, xatpb->idxs,
                                               extent, 1)) ||
             unlikely(__copy_from_guest_offset(&gpfn, xatpb->gpfns,
                                               extent, 1)) )
            return -EFAULT;

        rc = xenmem_add_to_physmap_one(d, xatpb->space,
                                       xatpb->u,
                                       idx, _gfn(gpfn));

        if ( unlikely(__copy_to_guest_offset(xatpb->errs, extent, &rc, 1)) )
            return -EFAULT;

        /* Check for continuation if it's not the last iteration. */
        if ( xatpb->size > ++extent && hypercall_preempt_check() )
            return extent;
    }

    return 0;
}

static int construct_memop_from_reservation(
               const struct xen_memory_reservation *r,
               struct memop_args *a)
{
    unsigned int address_bits;

    a->extent_list  = r->extent_start;
    a->nr_extents   = r->nr_extents;
    a->extent_order = r->extent_order;
    a->memflags     = 0;

    address_bits = XENMEMF_get_address_bits(r->mem_flags);
    if ( (address_bits != 0) &&
         (address_bits < (get_order_from_pages(max_page) + PAGE_SHIFT)) )
    {
        if ( address_bits <= PAGE_SHIFT )
            return -EINVAL;
        a->memflags = MEMF_bits(address_bits);
    }

    if ( r->mem_flags & XENMEMF_vnode )
    {
        nodeid_t vnode, pnode;
        struct domain *d = a->domain;

        read_lock(&d->vnuma_rwlock);
        if ( d->vnuma )
        {
            vnode = XENMEMF_get_node(r->mem_flags);
            if ( vnode >= d->vnuma->nr_vnodes )
            {
                read_unlock(&d->vnuma_rwlock);
                return -EINVAL;
            }

            pnode = d->vnuma->vnode_to_pnode[vnode];
            if ( pnode != NUMA_NO_NODE )
            {
                a->memflags |= MEMF_node(pnode);
                if ( r->mem_flags & XENMEMF_exact_node_request )
                    a->memflags |= MEMF_exact_node;
            }
        }
        read_unlock(&d->vnuma_rwlock);
    }
    else if ( unlikely(!propagate_node(r->mem_flags, &a->memflags)) )
        return -EINVAL;

    return 0;
}

#ifdef CONFIG_HAS_PASSTHROUGH
struct get_reserved_device_memory {
    struct xen_reserved_device_memory_map map;
    unsigned int used_entries;
};

static int get_reserved_device_memory(xen_pfn_t start, xen_ulong_t nr,
                                      u32 id, void *ctxt)
{
    struct get_reserved_device_memory *grdm = ctxt;
    u32 sbdf = PCI_SBDF3(grdm->map.dev.pci.seg, grdm->map.dev.pci.bus,
                         grdm->map.dev.pci.devfn);

    if ( !(grdm->map.flags & XENMEM_RDM_ALL) && (sbdf != id) )
        return 0;

    if ( grdm->used_entries < grdm->map.nr_entries )
    {
        struct xen_reserved_device_memory rdm = {
            .start_pfn = start, .nr_pages = nr
        };

        if ( __copy_to_guest_offset(grdm->map.buffer, grdm->used_entries,
                                    &rdm, 1) )
            return -EFAULT;
    }

    ++grdm->used_entries;

    return 1;
}
#endif

static long xatp_permission_check(struct domain *d, unsigned int space)
{
    /*
     * XENMAPSPACE_dev_mmio mapping is only supported for hardware Domain
     * to map this kind of space to itself.
     */
    if ( (space == XENMAPSPACE_dev_mmio) &&
         (!is_hardware_domain(current->domain) || (d != current->domain)) )
        return -EACCES;

    return xsm_add_to_physmap(XSM_TARGET, current->domain, d);
}

static int acquire_resource(
    XEN_GUEST_HANDLE_PARAM(xen_mem_acquire_resource_t) arg)
{
    struct domain *d, *currd = current->domain;
    xen_mem_acquire_resource_t xmar;
    /*
     * The mfn_list and gfn_list (below) arrays are ok on stack for the
     * moment since they are small, but if they need to grow in future
     * use-cases then per-CPU arrays or heap allocations may be required.
     */
    xen_pfn_t mfn_list[2];
    int rc;

    if ( copy_from_guest(&xmar, arg, 1) )
        return -EFAULT;

    if ( xmar.flags != 0 )
        return -EINVAL;

    if ( guest_handle_is_null(xmar.frame_list) )
    {
        if ( xmar.nr_frames )
            return -EINVAL;

        xmar.nr_frames = ARRAY_SIZE(mfn_list);

        if ( __copy_field_to_guest(arg, &xmar, nr_frames) )
            return -EFAULT;

        return 0;
    }

    if ( xmar.nr_frames > ARRAY_SIZE(mfn_list) )
        return -E2BIG;

    rc = rcu_lock_remote_domain_by_id(xmar.domid, &d);
    if ( rc )
        return rc;

    rc = xsm_domain_resource_map(XSM_DM_PRIV, d);
    if ( rc )
        goto out;

    switch ( xmar.type )
    {
    default:
        rc = arch_acquire_resource(d, xmar.type, xmar.id, xmar.frame,
                                   xmar.nr_frames, mfn_list, &xmar.flags);
        break;
    }

    if ( rc )
        goto out;

    if ( !paging_mode_translate(currd) )
    {
        if ( copy_to_guest(xmar.frame_list, mfn_list, xmar.nr_frames) )
            rc = -EFAULT;
    }
    else
    {
        xen_pfn_t gfn_list[ARRAY_SIZE(mfn_list)];
        unsigned int i;

        if ( copy_from_guest(gfn_list, xmar.frame_list, xmar.nr_frames) )
            rc = -EFAULT;

        for ( i = 0; !rc && i < xmar.nr_frames; i++ )
        {
            rc = set_foreign_p2m_entry(currd, gfn_list[i],
                                       _mfn(mfn_list[i]));
            /* rc should be -EIO for any iteration other than the first */
            if ( rc && i )
                rc = -EIO;
        }
    }

    if ( xmar.flags != 0 &&
         __copy_field_to_guest(arg, &xmar, flags) )
        rc = -EFAULT;

 out:
    rcu_unlock_domain(d);

    return rc;
}

long do_memory_op(unsigned long cmd, XEN_GUEST_HANDLE_PARAM(void) arg)
{
    struct domain *d, *curr_d = current->domain;
    long rc;
    struct xen_memory_reservation reservation;
    struct xen_memory_range_migration migration;
    struct memop_args args;
    domid_t domid;
    unsigned long start_extent = cmd >> MEMOP_EXTENT_SHIFT;
    int op = cmd & MEMOP_CMD_MASK;

    switch ( op )
    {
    case XENMEM_migrate_page_range:
	    if ( copy_from_guest(&migration, arg, 1) )
            	return start_extent;
            d = rcu_lock_domain_by_any_id(migration.domid);
            if ( d == NULL )
            	return start_extent;
	    migrate_page_range(d, &migration);
            rcu_unlock_domain(d);
	    printk("XENMEM_migrate_page_range being called, start_addr: %lu, end_addr:%lu\n", migration.start_addr, migration.end_addr);
	    rc = 0;
    break;
    case XENMEM_increase_reservation:
    case XENMEM_decrease_reservation_2:
    case XENMEM_populate_physmap_2:
    case XENMEM_decrease_reservation:
    case XENMEM_populate_physmap:
        if ( copy_from_guest(&reservation, arg, 1) )
            return start_extent;

        /* Is size too large for us to encode a continuation? */
        if ( reservation.nr_extents > (UINT_MAX >> MEMOP_EXTENT_SHIFT) )
            return start_extent;

        if ( unlikely(start_extent >= reservation.nr_extents) )
            return start_extent;

        d = rcu_lock_domain_by_any_id(reservation.domid);
        if ( d == NULL )
            return start_extent;
        args.domain = d;

        if ( construct_memop_from_reservation(&reservation, &args) )
        {
            rcu_unlock_domain(d);
            return start_extent;
        }

        args.nr_done   = start_extent;
        args.preempted = 0;

        if ( (op == XENMEM_populate_physmap || op == XENMEM_populate_physmap_2)
             && (reservation.mem_flags & XENMEMF_populate_on_demand) )
            args.memflags |= MEMF_populate_on_demand;

        if ( xsm_memory_adjust_reservation(XSM_TARGET, curr_d, d) )
        {
            rcu_unlock_domain(d);
            return start_extent;
        }

#ifdef CONFIG_X86
        if ( pv_shim && op != XENMEM_decrease_reservation && op != XENMEM_decrease_reservation_2  && !args.preempted )
            /* Avoid calling pv_shim_online_memory when preempted. */
            pv_shim_online_memory(args.nr_extents, args.extent_order);
#endif

        switch ( op )
        {
        case XENMEM_increase_reservation:
            increase_reservation(&args);
            break;
        case XENMEM_decrease_reservation:
            decrease_reservation(&args);
            break;
	case XENMEM_decrease_reservation_2:
	    //printk("I'm about to call XENMEM_decrease_reservation_2, d->tot_pages:%d, d->max_pages:%d\n", d->tot_pages, d->max_pages);
	    decrease_reservation_2(&args);
	    //printk("I'm called XENMEM_decrease_reservation_2, d->tot_pages:%d, d->max_pages:%d\n", d->tot_pages, d->max_pages);
	    break;
	case XENMEM_populate_physmap_2:
	    populate_physmap_2(&args);
	    //printk("I'm called XENMEM_populate_physmap_2, d->tot_pages:%d, d->max_pages:%d\n", d->tot_pages, d->max_pages);
	    break;
        default: /* XENMEM_populate_physmap */
            //printk("I'm in do_mem_op, MEMF_get_node(memflags): %d, NUMA_NO_NODE: %d, memflags & MEMF_exact_node: %d\n", MEMF_get_node(args.memflags), NUMA_NO_NODE, args.memflags & MEMF_exact_node);
	    populate_physmap(&args);
            break;
        }

        rcu_unlock_domain(d);

        rc = args.nr_done;

        if ( args.preempted )
            return hypercall_create_continuation(
                __HYPERVISOR_memory_op, "lh",
                op | (rc << MEMOP_EXTENT_SHIFT), arg);

#ifdef CONFIG_X86
        if ( pv_shim && (op == XENMEM_decrease_reservation || op == XENMEM_decrease_reservation_2) )
            /*
             * Only call pv_shim_offline_memory when the hypercall has
             * finished. Note that nr_done is used to cope in case the
             * hypercall has failed and only part of the extents where
             * processed.
             */
            pv_shim_offline_memory(args.nr_extents, args.nr_done);
#endif

        break;

    case XENMEM_exchange:
        if ( unlikely(start_extent) )
            return -EINVAL;

        rc = memory_exchange(guest_handle_cast(arg, xen_memory_exchange_t));
        break;

    case XENMEM_maximum_ram_page:
        if ( unlikely(start_extent) )
            return -EINVAL;

        rc = max_page;
        break;

    case XENMEM_current_reservation:
    case XENMEM_maximum_reservation:
    case XENMEM_maximum_gpfn:
        if ( unlikely(start_extent) )
            return -EINVAL;

        if ( copy_from_guest(&domid, arg, 1) )
            return -EFAULT;

        d = rcu_lock_domain_by_any_id(domid);
        if ( d == NULL )
            return -ESRCH;

        rc = xsm_memory_stat_reservation(XSM_TARGET, curr_d, d);
        if ( rc )
        {
            rcu_unlock_domain(d);
            return rc;
        }

        switch ( op )
        {
        case XENMEM_current_reservation:
            rc = d->tot_pages;
            break;
        case XENMEM_maximum_reservation:
            rc = d->max_pages;
            break;
        default:
            ASSERT(op == XENMEM_maximum_gpfn);
            rc = domain_get_maximum_gpfn(d);
            break;
        }

        rcu_unlock_domain(d);

        break;

    case XENMEM_add_to_physmap:
    {
        struct xen_add_to_physmap xatp;

        BUILD_BUG_ON((typeof(xatp.size))-1 > (UINT_MAX >> MEMOP_EXTENT_SHIFT));

        /* Check for malicious or buggy input. */
        if ( start_extent != (typeof(xatp.size))start_extent )
            return -EDOM;

        if ( copy_from_guest(&xatp, arg, 1) )
            return -EFAULT;

        /* Foreign mapping is only possible via add_to_physmap_batch. */
        if ( xatp.space == XENMAPSPACE_gmfn_foreign )
            return -ENOSYS;

        d = rcu_lock_domain_by_any_id(xatp.domid);
        if ( d == NULL )
            return -ESRCH;

        rc = xatp_permission_check(d, xatp.space);
        if ( rc )
        {
            rcu_unlock_domain(d);
            return rc;
        }

        rc = xenmem_add_to_physmap(d, &xatp, start_extent);

        rcu_unlock_domain(d);

        if ( xatp.space == XENMAPSPACE_gmfn_range && rc > 0 )
            rc = hypercall_create_continuation(
                     __HYPERVISOR_memory_op, "lh",
                     op | (rc << MEMOP_EXTENT_SHIFT), arg);

        return rc;
    }

    case XENMEM_add_to_physmap_batch:
    {
        struct xen_add_to_physmap_batch xatpb;

        BUILD_BUG_ON((typeof(xatpb.size))-1 >
                     (UINT_MAX >> MEMOP_EXTENT_SHIFT));

        /* Check for malicious or buggy input. */
        if ( start_extent != (typeof(xatpb.size))start_extent )
            return -EDOM;

        if ( copy_from_guest(&xatpb, arg, 1) )
            return -EFAULT;

        /* This mapspace is unsupported for this hypercall. */
        if ( xatpb.space == XENMAPSPACE_gmfn_range )
            return -EOPNOTSUPP;

        d = rcu_lock_domain_by_any_id(xatpb.domid);
        if ( d == NULL )
            return -ESRCH;

        rc = xatp_permission_check(d, xatpb.space);
        if ( rc )
        {
            rcu_unlock_domain(d);
            return rc;
        }

        rc = xenmem_add_to_physmap_batch(d, &xatpb, start_extent);

        rcu_unlock_domain(d);

        if ( rc > 0 )
            rc = hypercall_create_continuation(
                    __HYPERVISOR_memory_op, "lh",
                    op | (rc << MEMOP_EXTENT_SHIFT), arg);

        return rc;
    }

    case XENMEM_remove_from_physmap:
    {
        struct xen_remove_from_physmap xrfp;
        struct page_info *page;

        if ( unlikely(start_extent) )
            return -EINVAL;

        if ( copy_from_guest(&xrfp, arg, 1) )
            return -EFAULT;

        d = rcu_lock_domain_by_any_id(xrfp.domid);
        if ( d == NULL )
            return -ESRCH;

        rc = xsm_remove_from_physmap(XSM_TARGET, curr_d, d);
        if ( rc )
        {
            rcu_unlock_domain(d);
            return rc;
        }

        page = get_page_from_gfn(d, xrfp.gpfn, NULL, P2M_ALLOC);
        if ( page )
        {
            rc = guest_physmap_remove_page(d, _gfn(xrfp.gpfn),
                                           page_to_mfn(page), 0);
            put_page(page);
        }
        else
            rc = -ENOENT;

        rcu_unlock_domain(d);

        break;
    }

    case XENMEM_access_op:
        rc = mem_access_memop(cmd, guest_handle_cast(arg, xen_mem_access_op_t));
        break;

    case XENMEM_claim_pages:
        if ( unlikely(start_extent) )
            return -EINVAL;

        if ( copy_from_guest(&reservation, arg, 1) )
            return -EFAULT;

        if ( !guest_handle_is_null(reservation.extent_start) )
            return -EINVAL;

        if ( reservation.extent_order != 0 )
            return -EINVAL;

        if ( reservation.mem_flags != 0 )
            return -EINVAL;

        d = rcu_lock_domain_by_id(reservation.domid);
        if ( d == NULL )
            return -EINVAL;

        rc = xsm_claim_pages(XSM_PRIV, d);

        if ( !rc )
            rc = domain_set_outstanding_pages(d, reservation.nr_extents);

        rcu_unlock_domain(d);

        break;

    case XENMEM_get_vnumainfo:
    {
        struct xen_vnuma_topology_info topology;
        unsigned int dom_vnodes, dom_vranges, dom_vcpus;
        struct vnuma_info tmp;
	
	//printk("DEBUG: received a XENMEM_get_vnumainfo hypercall\n");

        if ( unlikely(start_extent) )
            return -EINVAL;

        /*
         * Guest passes nr_vnodes, number of regions and nr_vcpus thus
         * we know how much memory guest has allocated.
         */
        if ( copy_from_guest(&topology, arg, 1 ))
            return -EFAULT;

        if ( topology.pad != 0 )
            return -EINVAL;

        if ( (d = rcu_lock_domain_by_any_id(topology.domid)) == NULL )
            return -ESRCH;

        rc = xsm_get_vnumainfo(XSM_TARGET, d);
        if ( rc )
        {
            rcu_unlock_domain(d);
            return rc;
        }

        read_lock(&d->vnuma_rwlock);

        if ( d->vnuma == NULL )
        {
            read_unlock(&d->vnuma_rwlock);
            rcu_unlock_domain(d);
            return -EOPNOTSUPP;
        }

        dom_vnodes = d->vnuma->nr_vnodes;
        dom_vranges = d->vnuma->nr_vmemranges;
        dom_vcpus = d->max_vcpus;

        /*
         * Copied from guest values may differ from domain vnuma config.
         * Check here guest parameters make sure we dont overflow.
         * Additionaly check padding.
         */
        if ( topology.nr_vnodes < dom_vnodes      ||
             topology.nr_vcpus < dom_vcpus        ||
             topology.nr_vmemranges < dom_vranges )
        {
	    //printk("DEBUG: XENMEM_get_vnumainfo hypercall 1.5, topo.nr_vnodes: %d, dom_vnodes %d; \t topo.nr_vcpus: %d, dom_vcpus: %d \t topo.nr_vranges: %d, dom_vrange: %d\n", topology.nr_vnodes, dom_vnodes, topology.nr_vcpus, dom_vcpus, topology.nr_vmemranges, dom_vranges);
            read_unlock(&d->vnuma_rwlock);
            rcu_unlock_domain(d);

            topology.nr_vnodes = dom_vnodes;
            topology.nr_vcpus = dom_vcpus;
            topology.nr_vmemranges = dom_vranges;

            /* Copy back needed values. */
            return __copy_to_guest(arg, &topology, 1) ? -EFAULT : -ENOBUFS;
        }

        read_unlock(&d->vnuma_rwlock);

        tmp.vdistance = xmalloc_array(unsigned int, dom_vnodes * dom_vnodes);
        tmp.vmemrange = xmalloc_array(xen_vmemrange_t, dom_vranges);
        tmp.vcpu_to_vnode = xmalloc_array(unsigned int, dom_vcpus);

        if ( tmp.vdistance == NULL ||
             tmp.vmemrange == NULL ||
             tmp.vcpu_to_vnode == NULL )
        {
            rc = -ENOMEM;
            goto vnumainfo_out;
        }

        /*
         * Check if vnuma info has changed and if the allocated arrays
         * are not big enough.
         */
        read_lock(&d->vnuma_rwlock);

        if ( dom_vnodes < d->vnuma->nr_vnodes ||
             dom_vranges < d->vnuma->nr_vmemranges ||
             dom_vcpus < d->max_vcpus )
        {
            read_unlock(&d->vnuma_rwlock);
            rc = -EAGAIN;
            goto vnumainfo_out;
        }

        dom_vnodes = d->vnuma->nr_vnodes;
        dom_vranges = d->vnuma->nr_vmemranges;
        dom_vcpus = d->max_vcpus;

        memcpy(tmp.vmemrange, d->vnuma->vmemrange,
               sizeof(*d->vnuma->vmemrange) * dom_vranges);
        memcpy(tmp.vdistance, d->vnuma->vdistance,
               sizeof(*d->vnuma->vdistance) * dom_vnodes * dom_vnodes);
        memcpy(tmp.vcpu_to_vnode, d->vnuma->vcpu_to_vnode,
               sizeof(*d->vnuma->vcpu_to_vnode) * dom_vcpus);

        read_unlock(&d->vnuma_rwlock);

        rc = -EFAULT;

        if ( copy_to_guest(topology.vmemrange.h, tmp.vmemrange,
                           dom_vranges) != 0 )
            goto vnumainfo_out;

        if ( copy_to_guest(topology.vdistance.h, tmp.vdistance,
                           dom_vnodes * dom_vnodes) != 0 )
            goto vnumainfo_out;

        if ( copy_to_guest(topology.vcpu_to_vnode.h, tmp.vcpu_to_vnode,
                           dom_vcpus) != 0 )
            goto vnumainfo_out;

        topology.nr_vnodes = dom_vnodes;
        topology.nr_vcpus = dom_vcpus;
        topology.nr_vmemranges = dom_vranges;

        rc = __copy_to_guest(arg, &topology, 1) ? -EFAULT : 0;

 vnumainfo_out:
        rcu_unlock_domain(d);

        xfree(tmp.vdistance);
        xfree(tmp.vmemrange);
        xfree(tmp.vcpu_to_vnode);
        break;
    }

#ifdef CONFIG_HAS_PASSTHROUGH
    case XENMEM_reserved_device_memory_map:
    {
        struct get_reserved_device_memory grdm;

        if ( unlikely(start_extent) )
            return -EINVAL;

        if ( copy_from_guest(&grdm.map, arg, 1) ||
             !guest_handle_okay(grdm.map.buffer, grdm.map.nr_entries) )
            return -EFAULT;

        if ( grdm.map.flags & ~XENMEM_RDM_ALL )
            return -EINVAL;

        grdm.used_entries = 0;
        rc = iommu_get_reserved_device_memory(get_reserved_device_memory,
                                              &grdm);

        if ( !rc && grdm.map.nr_entries < grdm.used_entries )
            rc = -ENOBUFS;
        grdm.map.nr_entries = grdm.used_entries;
        if ( __copy_to_guest(arg, &grdm.map, 1) )
            rc = -EFAULT;

        break;
    }
#endif

    case XENMEM_acquire_resource:
        rc = acquire_resource(
            guest_handle_cast(arg, xen_mem_acquire_resource_t));
        break;

    default:
        rc = arch_memory_op(cmd, arg);
        break;
    }

    return rc;
}

void clear_domain_page(mfn_t mfn)
{
    void *ptr = map_domain_page(mfn);

    clear_page(ptr);
    unmap_domain_page(ptr);
}

void copy_domain_page(mfn_t dest, mfn_t source)
{
    const void *src = map_domain_page(source);
    void *dst = map_domain_page(dest);

    copy_page(dst, src);
    unmap_domain_page(dst);
    unmap_domain_page(src);
}

void destroy_ring_for_helper(
    void **_va, struct page_info *page)
{
    void *va = *_va;

    if ( va != NULL )
    {
        unmap_domain_page_global(va);
        put_page_and_type(page);
        *_va = NULL;
    }
}

int prepare_ring_for_helper(
    struct domain *d, unsigned long gmfn, struct page_info **_page,
    void **_va)
{
    struct page_info *page;
    p2m_type_t p2mt;
    void *va;

    page = get_page_from_gfn(d, gmfn, &p2mt, P2M_UNSHARE);

#ifdef CONFIG_HAS_MEM_PAGING
    if ( p2m_is_paging(p2mt) )
    {
        if ( page )
            put_page(page);
        p2m_mem_paging_populate(d, gmfn);
        return -ENOENT;
    }
#endif
#ifdef CONFIG_HAS_MEM_SHARING
    if ( p2m_is_shared(p2mt) )
    {
        if ( page )
            put_page(page);
        return -ENOENT;
    }
#endif

    if ( !page )
        return -EINVAL;

    if ( !get_page_type(page, PGT_writable_page) )
    {
        put_page(page);
        return -EINVAL;
    }

    va = __map_domain_page_global(page);
    if ( va == NULL )
    {
        put_page_and_type(page);
        return -ENOMEM;
    }

    *_va = va;
    *_page = page;

    return 0;
}

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
