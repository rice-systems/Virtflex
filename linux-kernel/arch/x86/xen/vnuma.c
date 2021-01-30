#include <linux/err.h>
#include <linux/memblock.h>
#include <xen/interface/xen.h>
#include <xen/interface/memory.h>
#include <asm/xen/interface.h>
#include <asm/xen/hypercall.h>
#include <asm/xen/vnuma.h>

#ifdef CONFIG_NUMA

/* Checks if hypercall is suported */
int xen_vnuma_supported()
{
	return HYPERVISOR_memory_op(XENMEM_get_vnumainfo, NULL) == -ENOSYS ? 0 : 1;
}

int __init xen_numa_init(void)
{
	int rc;
	unsigned int i, j, nr_nodes, cpu, idx, pcpus, nr_vnodes, nr_vmemranges;
	u64 physm, physd, physc;
	unsigned int *vdistance, *cpu_to_node;
	unsigned long mem_size, dist_size, cpu_to_node_size;
	//xen_vmemrange_t* vblock;
	void* vblock;

	/* For now only PV guests are supported */
	//if (!xen_pv_domain())
	//	return rc;
	
	struct xen_vnuma_topology_info vnuma_topo = { .domid = DOMID_SELF };

	rc = HYPERVISOR_memory_op(XENMEM_get_vnumainfo, &vnuma_topo);
	printk("I'm here in xen_numa_init(), \ 
			rc = %d (ENOBUF:%d), vnuma.nr_vnode = %d, \
			nr_vmemranges = %d \n" \
			, rc, ENOBUFS, vnuma_topo.nr_vnodes, vnuma_topo.nr_vmemranges);

	if ( rc != -ENOBUFS )
        	return rc;
	
	//pcpus = num_possible_cpus();
	pcpus = vnuma_topo.nr_vcpus;

	nr_vnodes = vnuma_topo.nr_vnodes;
	nr_vmemranges = vnuma_topo.nr_vmemranges;

	mem_size =  nr_vmemranges * sizeof(xen_vmemrange_t);
	dist_size = nr_vnodes * nr_vnodes * sizeof(*vdistance);
	cpu_to_node_size = pcpus * sizeof(*cpu_to_node);

	physm = memblock_alloc(mem_size, PAGE_SIZE);
	vblock = __va(physm);

	physd = memblock_alloc(dist_size, PAGE_SIZE);
	vdistance  = __va(physd);

	physc = memblock_alloc(cpu_to_node_size, PAGE_SIZE);
	cpu_to_node  = __va(physc);

	if (!physm || !physc || !physd)
		goto vnumaout;
	//set_xen_guest_handle(vnuma_topo.nr_nodes, &nr_nodes);
	set_xen_guest_handle(vnuma_topo.vdistance.h, vdistance);
	set_xen_guest_handle(vnuma_topo.vcpu_to_vnode.h, cpu_to_node);
	set_xen_guest_handle(vnuma_topo.vmemrange.h, vblock);


	//vcpu_to_vnode =
	//	scratch_alloc(sizeof(*vcpu_to_vnode) * pcpus, 0);
	//vdistance = scratch_alloc(sizeof(uint32_t) * vnuma_topo.nr_vnodes *
	//			vnuma_topo.nr_vnodes, 0);
	//vmemrange = scratch_alloc(sizeof(xen_vmemrange_t) *
	//			vnuma_topo.nr_vmemranges, 0);
//
//	set_xen_guest_handle(vnuma_topo.vdistance.h, vdistance);
//	set_xen_guest_handle(vnuma_topo.vcpu_to_vnode.h, vcpu_to_vnode);
//	set_xen_guest_handle(vnuma_topo.vmemrange.h, vmemrange);
//
	rc = HYPERVISOR_memory_op(XENMEM_get_vnumainfo, &vnuma_topo);
	printk("hola, I'm here in xen_numa_init() again, \ 
			rc = %d (ENOBUF:%d), vnuma.nr_vnode = %d, \
			nr_vmemranges = %d, num_possible_cpus:%d\n" \
			, rc, ENOBUFS, vnuma_topo.nr_vnodes, 
			vnuma_topo.nr_vmemranges,
			num_possible_cpus());
	//for (i=0; i < pcpus; i++)
	//printk("This is the %dth vcpu, it's in node %d\n" \ 
	//		, i, *(*uint64_t)vnuma_topo.vcpu_to_vnode);

	if (rc < 0)
		goto vnumaout;
	
	if (vnuma_topo.nr_vnodes == 0) {
		/* will pass to dummy_numa_init */
		goto vnumaout;
	}
	//if (vnuma_topo.nr_vnodes > num_possible_cpus()) {
	if (vnuma_topo.nr_vnodes > vnuma_topo.nr_vcpus) {
		pr_debug("vNUMA: Node without cpu is not supported in this version.\n");
		goto vnumaout;
	}

	/*
	 * NUMA nodes memory ranges are in pfns, constructed and
	 * aligned based on e820 ram domain map.
	 */
	for (i = 0; i < vnuma_topo.nr_vnodes; i++) {
		if (numa_add_memblk(i, ((xen_vmemrange_t*)vblock)[i].start, ((xen_vmemrange_t*)vblock)[i].end))
			/* pass to numa_dummy_init */
			goto vnumaout;
		node_set(i, numa_nodes_parsed);
	}
	setup_nr_node_ids();
	/* Setting the cpu, apicid to node */
	for_each_cpu(cpu, cpu_possible_mask) {
		set_apicid_to_node(cpu, cpu_to_node[cpu]);
		numa_set_node(cpu, cpu_to_node[cpu]);
		printk("Didn't crash yet, MAX_NUMNODES:%d, cpu:%d, cpu_to_node: %d, sizeof(node_mask):%ld\n", MAX_NUMNODES, cpu, cpu_to_node[cpu],node_to_cpumask_map[cpu_to_node[cpu]]->bits);
		// the origianl line in the patch:
		//cpumask_set_cpu(cpu, node_to_cpumask_map[cpu_to_node[cpu]]);
		// expand the origianl line into the following:
		//set_bit(cpu, node_to_cpumask_map[cpu_to_node[cpu]]->bits);
		// expand set_bit even further, the second brach will
		// be executed and that will cause pv guest kernel to
		// crash 
//		long nr = cpu;
//		unsigned long *addr = 
//			node_to_cpumask_map[cpu_to_node[cpu]]->bits;
//		if (IS_IMMEDIATE(nr)) {
//			asm volatile(LOCK_PREFIX "orb %1,%0"
//				: CONST_MASK_ADDR(nr, addr)
//				: "iq" ((u8)CONST_MASK(nr))
//				: "memory");
//		} else {
//			asm volatile("nop");
			//asm volatile("btsq %1,%0" : "+m" (*(u64 *)addr) : "Ir" (nr));
//			asm volatile(LOCK_PREFIX __ASM_SIZE(bts) " %1,%0"
//				: BITOP_ADDR(addr) : "Ir" (nr) : "memory");
//		}

	}
	for (i = 0; i < vnuma_topo.nr_vnodes; i++) {
		for (j = 0; j < vnuma_topo.nr_vnodes; j++) {
			idx = (j * vnuma_topo.nr_vnodes) + i;
			numa_set_distance(i, j, *(vdistance + idx));
		}
	}

vnumaout:
	if (physm)
		memblock_free(__pa(physm), mem_size);
	if (physd)
		memblock_free(__pa(physd), dist_size);
	if (physc)
		memblock_free(__pa(physc), cpu_to_node_size);
	/*
	 * Set the "dummy" node and exit without error so Linux
	 * will not try any NUMA init functions which might break
	 * guests in the future. This will discard all previous
	 * settings.
	 */
	//rc = 1;
	if (rc != 0) {
		for (i = 0; i < MAX_LOCAL_APIC; i++)
			set_apicid_to_node(i, NUMA_NO_NODE);
		nodes_clear(numa_nodes_parsed);
		nodes_clear(node_possible_map);
		nodes_clear(node_online_map);
		node_set(0, numa_nodes_parsed);
		numa_add_memblk(0, 0, PFN_PHYS(max_pfn));
	}
	return 0;
}
#endif
