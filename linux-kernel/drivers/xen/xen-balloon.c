/******************************************************************************
 * Xen balloon driver - enables returning/claiming memory to/from Xen.
 *
 * Copyright (c) 2003, B Dragovic
 * Copyright (c) 2003-2004, M Williamson, K Fraser
 * Copyright (c) 2005 Dan M. Smith, IBM Corporation
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

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/mm_types.h>
#include <linux/init.h>
#include <linux/capability.h>

#include <xen/xen.h>
#include <xen/interface/xen.h>
#include <xen/balloon.h>
#include <xen/xenbus.h>
#include <xen/features.h>
#include <xen/page.h>

#define PAGE_SIZE_ADJUST 0 	// control the granularity of ballooning
				// -9 for 4k ballooning, 0 for 2m

#define PAGES2KB(_p) ((_p)<<(HPAGE_SHIFT + PAGE_SIZE_ADJUST-10))

#define BALLOON_CLASS_NAME "xen_memory"

static struct device balloon_dev;

static int register_balloon(struct device *dev);

/* React to a change in the target key */
static void watch_target(struct xenbus_watch *watch,
			 const char *path, const char *token)
{
	unsigned long long new_target, static_max;
	int err;
	static bool watch_fired;
	static long target_diff;

	printk("I'm here at watch_target\n");
	err = xenbus_scanf(XBT_NIL, "memory", "target", "%llu", &new_target);
	if (err != 1) {
		/* This is ok (for domain0 at least) - so just return */
		return;
	}

	/* The given memory/target value is in KiB, so it needs converting to
	 * pages. PAGE_SHIFT converts bytes to pages, hence PAGE_SHIFT - 10.
	 */
	new_target >>= HPAGE_SHIFT + PAGE_SIZE_ADJUST - 10;

	if (!watch_fired) {
		watch_fired = true;
		err = xenbus_scanf(XBT_NIL, "memory", "static-max", \
				"%llu", &static_max);
		if (err != 1)
			static_max = new_target;
		else
			static_max >>= HPAGE_SHIFT + PAGE_SIZE_ADJUST - 10;
		target_diff = (xen_pv_domain() || xen_initial_domain()) ? 0
				: static_max - balloon_stats.target_pages;
	}

	balloon_set_new_target(new_target - target_diff, -1);

}
static struct xenbus_watch target_watch = {
	.node = "memory/target",
	.callback = watch_target,
};

static void watch_node_target(struct xenbus_watch *watch,
                         const char *path, const char *token)
{
        unsigned long long node_target;
        int err;
        int i;
        char buf[20];
	struct pglist_data* node_list;
	static int num_nodes;
	static bool fired = false;

	// get the number of online nodes when first run
	if (!fired){
        	node_list = first_online_pgdat();
        	for (i=0; node_list; node_list = \
                        next_online_pgdat(node_list),i++);
		num_nodes = i;
		fired = true;
	}
        printk("I'm here at watch_node_target, num_nodes=%d\n", num_nodes);

        //set target for each nodes sequentially, from 0
        //i=0;
        //while(1){
        //        bstr_printf(buf, 20, "node_target%d", &i);
        //        err = xenbus_scanf(XBT_NIL, "memory/node",\
        //                        buf, "%llu", &node_target);
        //        if (err != 1) {
        //                printk("I'm here at watch_node_target, "
        //                    "finished setting target for %d nodes\n", i);
        //                break;
        //        }
        //        printk("I'm here at watch_node_target, %s target: "
        //                       "%llu\n",buf, node_target);
        //        node_target >>= PAGE_SHIFT - 10;
        //        balloon_set_new_target(node_target, i);
        //        i++;
        //}
	
        for (i=0; i < num_nodes; i++){
                bstr_printf(buf, 20, "%d/target", &i);
                err = xenbus_scanf(XBT_NIL, "numa/node",\
                                buf, "%llu", &node_target);
		if (err >= 0){
			// one value is successfully scanned
                	printk("I'm here at watch_node_target, %s target: "
                               "%llu\n",buf, node_target);
                	node_target >>= HPAGE_SHIFT + PAGE_SIZE_ADJUST - 10;
                	balloon_set_new_target(node_target, i);
		}
		else{
			printk("In watch_node_target, xenbus_scanf error, error code %d\n", err);
		}
        }
}

static struct xenbus_watch node_target_watch = {
        .node = "numa/node",
        .callback = watch_node_target,
};

static int balloon_init_watcher(struct notifier_block *notifier,
				unsigned long event,
				void *data)
{
	int err;

	printk("I'm here in balloon_init_watcher\n");
	err = register_xenbus_watch(&target_watch);
	if (err)
		pr_err("Failed to set balloon watcher\n");
	err = register_xenbus_watch(&node_target_watch);
	if (err){
		pr_err("Failed to set balloon watcher(node)\n");
		printk("I'm in balloon_init_watcher, node_target_watch "
				"registration failed\n");
	}

	return NOTIFY_DONE;
}

static struct notifier_block xenstore_notifier = {
	.notifier_call = balloon_init_watcher,
};

void xen_balloon_init(void)
{
	register_balloon(&balloon_dev);

	register_xen_selfballooning(&balloon_dev);

	register_xenstore_notifier(&xenstore_notifier);
}
EXPORT_SYMBOL_GPL(xen_balloon_init);

#define BALLOON_SHOW(name, format, args...)				\
	static ssize_t show_##name(struct device *dev,			\
				   struct device_attribute *attr,	\
				   char *buf)				\
	{								\
		return sprintf(buf, format, ##args);			\
	}								\
	static DEVICE_ATTR(name, S_IRUGO, show_##name, NULL)

BALLOON_SHOW(current_kb, "%lu\n", PAGES2KB(balloon_stats.current_pages));
BALLOON_SHOW(low_kb, "%lu\n", PAGES2KB(balloon_stats.balloon_low));
BALLOON_SHOW(high_kb, "%lu\n", PAGES2KB(balloon_stats.balloon_high));

static DEVICE_ULONG_ATTR(schedule_delay, 0444, balloon_stats.schedule_delay);
static DEVICE_ULONG_ATTR(max_schedule_delay, 0644, balloon_stats.max_schedule_delay);
static DEVICE_ULONG_ATTR(retry_count, 0444, balloon_stats.retry_count);
static DEVICE_ULONG_ATTR(max_retry_count, 0644, balloon_stats.max_retry_count);

static ssize_t show_target_kb(struct device *dev, struct device_attribute *attr,
			      char *buf)
{
	return sprintf(buf, "%lu\n", PAGES2KB(balloon_stats.target_pages));
}

static ssize_t store_target_kb(struct device *dev,
			       struct device_attribute *attr,
			       const char *buf,
			       size_t count)
{
	char *endchar;
	unsigned long long target_bytes;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	target_bytes = simple_strtoull(buf, &endchar, 0) * 1024;

	balloon_set_new_target(target_bytes >> (HPAGE_SHIFT + PAGE_SIZE_ADJUST), -1);

	return count;
}

static DEVICE_ATTR(target_kb, S_IRUGO | S_IWUSR,
		   show_target_kb, store_target_kb);


static ssize_t show_target(struct device *dev, struct device_attribute *attr,
			      char *buf)
{
	return sprintf(buf, "%llu\n",
		       (unsigned long long)balloon_stats.target_pages
		       << (HPAGE_SHIFT + PAGE_SIZE_ADJUST));
}

static ssize_t store_target(struct device *dev,
			    struct device_attribute *attr,
			    const char *buf,
			    size_t count)
{
	char *endchar;
	unsigned long long target_bytes;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	target_bytes = memparse(buf, &endchar);

	balloon_set_new_target(target_bytes >> (HPAGE_SHIFT + PAGE_SIZE_ADJUST), -1);

	return count;
}

static DEVICE_ATTR(target, S_IRUGO | S_IWUSR,
		   show_target, store_target);


static struct attribute *balloon_attrs[] = {
	&dev_attr_target_kb.attr,
	&dev_attr_target.attr,
	&dev_attr_schedule_delay.attr.attr,
	&dev_attr_max_schedule_delay.attr.attr,
	&dev_attr_retry_count.attr.attr,
	&dev_attr_max_retry_count.attr.attr,
	NULL
};

static const struct attribute_group balloon_group = {
	.attrs = balloon_attrs
};

static struct attribute *balloon_info_attrs[] = {
	&dev_attr_current_kb.attr,
	&dev_attr_low_kb.attr,
	&dev_attr_high_kb.attr,
	NULL
};

static const struct attribute_group balloon_info_group = {
	.name = "info",
	.attrs = balloon_info_attrs
};

static const struct attribute_group *balloon_groups[] = {
	&balloon_group,
	&balloon_info_group,
	NULL
};

static struct bus_type balloon_subsys = {
	.name = BALLOON_CLASS_NAME,
	.dev_name = BALLOON_CLASS_NAME,
};

static int register_balloon(struct device *dev)
{
	int error;

	error = subsys_system_register(&balloon_subsys, NULL);
	if (error)
		return error;

	dev->id = 0;
	dev->bus = &balloon_subsys;
	dev->groups = balloon_groups;

	error = device_register(dev);
	if (error) {
		bus_unregister(&balloon_subsys);
		return error;
	}

	return 0;
}
