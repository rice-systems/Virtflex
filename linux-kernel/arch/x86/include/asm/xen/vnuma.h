#ifndef _ASM_X86_VNUMA_H
#define _ASM_X86_VNUMA_H

#ifdef CONFIG_XEN
int xen_vnuma_supported(void);
int xen_numa_init(void);
#else
int xen_vnuma_supported(void) { return 0; };
int xen_numa_init(void) { return -1; };
#endif

#endif /* _ASM_X86_VNUMA_H */
