#ifndef VNODE_INFO_H
#define VNODE_INFO_H

/*
    vnode_info, vnode_info_parameter is only for internal
    use between parse_vnuma_config and vnuma_write_to_xs, caller allocate vnode_info_para
    before calling parse_vnuma_config, parse_vnuma_config fill it up and later the info is
    used by vnuma_write_to_xs to write vnuma info in xenstore.
*/

struct vnode_info{
        int pnode;
        unsigned long target;
        unsigned long capacity;
        int* vcpu;
        int num_vcpus;
};

struct vnode_info_parameter{
        int num_nodes;
        struct vnode_info* vnode_infos;
};

#endif  /* VNODE_INFO_H */
