#include <linux/kernel.h>
#include <linux/syscalls.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <xen/xenbus.h>
#include <linux/mutex.h>

#define string_size 20

static int alloc_copy_user_array(void** k, void* u, size_t size){
	*k = kmalloc(size, GFP_KERNEL);
	if(!(*k))
		return -ENOMEM;
	if(copy_from_user(*k, u, size))
		return -EFAULT;
	return 0;
}

static int write_msr_list(unsigned long* list, unsigned long* val, size_t len){
	u32 lo, hi;
	int i;
	for(i=0; i < len; i++){
		lo = (u32)val[i];
		hi =  val[i] >> 32;
		wrmsr(list[i], lo, hi);
	}
	return 0;
}

static int read_msr_list(unsigned long* list, unsigned long* val, size_t len){
	u32 lo, hi;
	int i;
	for(i=0; i < len; i++){
		rdmsr(list[i], lo, hi);
		val[i] = hi;
		val[i] <<= 32;
		val[i] |= lo;
	}
	return 0;
}

static int write_list_to_xs(unsigned long* val_list, char* dir, char* nodes, size_t len){
	int ret = 0;
	int i;
	size_t size = 200;
	char buf[size];
	for(i=0; i < len; i++){
		snprintf(buf, size, "%lu", val_list[i]);
		ret |= xenbus_write(XBT_NIL, dir, &(nodes[i*string_size]), buf);
	}
	return ret;
}

static void print_array(unsigned long* list, size_t len){
	int i;
	for(i =0; i < len; i++){
		printk("%lx", list[i]);
	}
}


static void print_nodes(char* list, size_t len){
	int i;
	for(i =0; i < len; i++){
		printk("%s", &list[i*string_size]);
	}
}

struct mutex *numa_locks;
/*
interval == 0 --> starting phase
interval <  0 --> ending phase
interval >  0 --> measuring phase
*/
SYSCALL_DEFINE6(report_stat, unsigned long* , msr_config, unsigned long*, config_val,
		unsigned long*, msr_counter, char*, nodes, long, len, long, interval)
{
	int i;
	long ret = 0;
	int owner;
	unsigned long *msr_configk=NULL, *config_valk=NULL, *msr_counterk=NULL, *counter_valk=NULL;
	char* nodesk = NULL;
	char dir[string_size];
    	int thisnid;
	static bool first_time = true;
        struct pglist_data* node_list;
	static int num_nodes = 0;
	DEFINE_MUTEX(init);

	if(len <= 0 )
		return -EINVAL;
	
	if(first_time){
		if(mutex_trylock(&init) == 0)
			return -EBUSY;
		if(first_time){
        		node_list = first_online_pgdat();
        		for (i=0; node_list; node_list = next_online_pgdat(node_list),i++);
        		num_nodes = i;
		
	        	numa_locks = (struct mutex*)kzalloc(num_nodes*sizeof(struct mutex), GFP_KERNEL);
			if(!numa_locks){
				return -ENOMEM;
			}
			for(i=0; i<num_nodes; i++){
				mutex_init(&numa_locks[i]);
			}
			first_time = false;
		}
		mutex_unlock(&init);
    	}
	
	thisnid = cpu_to_node(raw_smp_processor_id());

	// if this vnode doesn't have ownership for this pnode in xenstore, exit
	snprintf(dir, string_size, "numa/node/%d", thisnid);
	ret = xenbus_scanf(XBT_NIL, dir, "pnode_owner", "%d", &owner);
	if(ret <0 || owner != 1){
		return -EPERM;
	}
	ret=0;

	BUG_ON(!numa_locks);
	BUG_ON(thisnid <0 || thisnid >= num_nodes);
	if(mutex_trylock(&numa_locks[thisnid]) == 0 )
		return -EBUSY;

	// testing cases
	if(interval == -3){
		
		if(alloc_copy_user_array((void**)&msr_counterk, (void*)msr_counter, len*sizeof(unsigned long))){
                        ret =  -ENOMEM;
                        goto out;
                }
                if(alloc_copy_user_array((void**)&msr_configk, (void*)msr_config, len*sizeof(unsigned long))){
                        ret = -ENOMEM;
                        goto out;
                }
                if(alloc_copy_user_array((void**)&config_valk, (void*)config_val, len*sizeof(unsigned long))){
                        ret = -ENOMEM;
                        goto out;
                }
		counter_valk = (unsigned long*)kzalloc(len*sizeof(unsigned long), GFP_KERNEL);
		if(!counter_valk){
			ret = -ENOMEM;
                        goto out;
		}
		if(alloc_copy_user_array((void**)&nodesk, (void*)nodes, len*sizeof(char)*string_size)){
                        ret =  -ENOMEM;
                        goto out;
                }
	//unsigned long *msr_configk=NULL, *config_valk=NULL, *msr_counterk=NULL, *counter_valk=NULL;
		printk("msr_configk:");
		print_array(msr_configk, len);
		printk("config_valk:");
		print_array(config_valk, len);
		printk("msr_counterk");
		print_array(msr_counterk, len);
		printk("counter_valk:");
		print_array(counter_valk, len);
		printk("nodesk:");
		print_nodes(nodesk, len);
		for(i = 0; i < len; i++){
			counter_valk[i] = 0xdeadbeefU;
		}
		if(write_msr_list(msr_counterk, counter_valk, len)){
			ret = -EPERM;
			goto out;	
		} 
		for(i = 0; i < len; i++){
			counter_valk[i] = 0x0U;
		}
		if(read_msr_list(msr_counterk, counter_valk, len)){
			ret = -EPERM;
                        goto out;
		}
		printk("counter_valk after rd/wr deadbeef:");
		print_array(counter_valk, len);
		if(write_list_to_xs(counter_valk, dir, nodesk, len)){
	                ret = -EPERM;
                        goto out;
		}

	}
	else if(interval < 0){
		/*ending phase*/
		if(alloc_copy_user_array((void**)&msr_counterk, (void*)msr_counter, len*sizeof(unsigned long))){
			ret =  -ENOMEM;
			goto out;
		}
		if(alloc_copy_user_array((void**)&msr_configk, (void*)msr_config, len*sizeof(unsigned long))){
                        ret = -ENOMEM;
			goto out;
		}
		counter_valk = (unsigned long*)kzalloc(len*sizeof(unsigned long), GFP_KERNEL);
		if(!counter_valk){
			ret = -ENOMEM;
                        goto out;
		}
		
		if(write_msr_list(msr_configk, counter_valk, len)){
			ret = -EPERM;
			goto out;	
		} 
		if(write_msr_list(msr_counterk, counter_valk, len)){
			ret = -EPERM;
                        goto out;
		}

	}
	else if(interval == 0){
		/*starting phase*/
		if(alloc_copy_user_array((void**)&msr_counterk, (void*)msr_counter, len*sizeof(unsigned long))){
                        ret =  -ENOMEM;
                        goto out;
                }
                if(alloc_copy_user_array((void**)&msr_configk, (void*)msr_config, len*sizeof(unsigned long))){
                        ret = -ENOMEM;
                        goto out;
                }
                if(alloc_copy_user_array((void**)&config_valk, (void*)config_val, len*sizeof(unsigned long))){
                        ret = -ENOMEM;
                        goto out;
                }
		counter_valk = (unsigned long*)kzalloc(len*sizeof(unsigned long), GFP_KERNEL);
                if(!counter_valk){
                        ret = -ENOMEM;
                        goto out;
                }

		if(write_msr_list(msr_configk, config_valk, len)){
			ret = -EPERM;
			goto out;	
		} 
		if(write_msr_list(msr_counterk, counter_valk, len)){
			ret = -EPERM;
                        goto out;
		}
		
		
	}
	else{
		/*measuring phase*/
		if(alloc_copy_user_array((void**)&msr_counterk, (void*)msr_counter, len*sizeof(unsigned long))){
                        ret =  -ENOMEM;
                        goto out;
                }
		if(alloc_copy_user_array((void**)&nodesk, (void*)nodes, len*sizeof(char)*string_size)){
                        ret =  -ENOMEM;
                        goto out;
                }
		counter_valk = (unsigned long*)kzalloc(len*sizeof(unsigned long), GFP_KERNEL);
                if(!counter_valk){
                        ret = -ENOMEM;
                        goto out;
                }
		if(write_msr_list(msr_counterk, counter_valk, len)){
			ret = -EPERM;
                        goto out;
		}
		// wait for interval
		msleep(interval);
		if(read_msr_list(msr_counterk, counter_valk, len)){
			ret = -EPERM;
                        goto out;
		}
		// calculate measurement and write the value to xenstore
		for(i=0; i < len; i++){
			// in the case of bw counters, this is bw usage in MB.
			counter_valk[i] = counter_valk[i]*64/1024/1024*interval/1000;
		}
		
		snprintf(dir, string_size, "numa/node/%d", thisnid);

		if(write_list_to_xs(counter_valk, dir, nodesk, len)){
	                ret = -EPERM;
                        goto out;
		}
	
	}

    	//printk("len: %ld, msr_config:", len);
	//for(i = 0; i < len; i++){
	//	printk("%lx", msr_configk[i]);
	//}

out:
	//unsigned long *msr_configk=NULL, *config_valk=NULL, *msr_counterk=NULL, *counter_valk=NULL;
	mutex_unlock(&numa_locks[thisnid]);

	if(msr_configk)
		kfree(msr_configk);
	if(config_valk)
		kfree(config_valk);
	if(msr_counterk)
                kfree(msr_counterk);
        if(counter_valk)
                kfree(counter_valk);
	if(nodesk)
		kfree(nodesk);
    	return ret;
}
