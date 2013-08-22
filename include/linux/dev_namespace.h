/*
 * include/linux/dev_namespace.h
 *
 * Copyright (c) 2011-2013 Cellrox Ltd. Certain portions are copyrighted by
 * Columbia University. This program is free software licensed under the GNU
 * General Public License Version 2 (GPL 2). You can distribute it and/or
 * modify it under the terms of the GPL 2.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GPL 2 license for more details.
 * The full GPL 2 License is included in this distribution in the file called
 * COPYING
 *
 * Cellrox can be contacted at oss@cellrox.com
 */

#ifndef _LINUX_DEV_NS_H
#define _LINUX_DEV_NS_H

#include <linux/pid_namespace.h>
#include <linux/nsproxy.h>
#include <linux/notifier.h>
#include <linux/hardirq.h>
#include <linux/err.h>

#ifdef __KERNEL__

#define DEV_NS_TAG_LEN 4
#define DEV_NS_DESC_MAX 16

struct dev_namespace;
struct dev_ns_info;

struct dev_namespace {

	bool active;
	atomic_t count;
	struct pid_namespace *pid_ns;
	char tag[DEV_NS_TAG_LEN + 1];
	struct blocking_notifier_head notifiers;
	unsigned long timestamp; /* jiffies */

	struct mutex mutex;
	struct dev_ns_info *info[DEV_NS_DESC_MAX];
};

struct dev_ns_info {
	struct dev_namespace *dev_ns;
	struct list_head list;
	struct notifier_block nb;
	atomic_t count;
};

#ifdef CONFIG_DEV_NS

struct dev_ns_ops {
	struct dev_ns_info *(*create) (struct dev_namespace *dev_ns);
	void (*release) (struct dev_ns_info *dev_ns_info);
};

/* device namespace notifications */
#define DEV_NS_EVENT_ACTIVATE		0x1
#define DEV_NS_EVENT_DEACTIVATE		0x2

extern struct dev_namespace init_dev_ns;
extern struct dev_namespace *active_dev_ns;

extern void __put_dev_ns(struct dev_namespace *dev_ns);

static inline void put_dev_ns(struct dev_namespace *dev_ns)
{
	if (atomic_dec_and_test(&dev_ns->count))
		__put_dev_ns(dev_ns);
}

static inline struct dev_namespace *get_dev_ns(struct dev_namespace *dev_ns)
{
	atomic_inc(&dev_ns->count);
	return dev_ns;
}

/* return the device namespaces of the current process */
static inline struct dev_namespace *current_dev_ns(void)
{
	BUG_ON(in_interrupt());
	return current->nsproxy->dev_ns;
}

/* return whether given device namespace is active */
static inline bool is_active_dev_ns(struct dev_namespace *dev_ns)
{
	return dev_ns->active;
}

/* return and get the device namespace of a given task */
extern struct dev_namespace *get_dev_ns_by_task(struct task_struct *task);
extern struct dev_namespace *get_dev_ns_by_vpid(pid_t vpid);

/*
 * set_active_dev_ns() will lock and unlock dev_namespace_lock
 * and call all registered activate and inactivate notifiers.
 */
extern void set_active_dev_ns(struct dev_namespace *dev_ns);

/* return the tag of the current namespace */
extern void get_dev_ns_tag(char *to, struct dev_namespace *dev);

/* return root pid of the init process in a device namespace */
static inline pid_t dev_ns_init_pid(struct dev_namespace *dev_ns)
{
	return dev_ns->pid_ns->child_reaper->pid;
}

/* device namespaces: notifiers (de)registration */
extern void dev_ns_register_notify(struct dev_namespace *dev_ns,
				   struct notifier_block *nb);
extern void dev_ns_unregister_notify(struct dev_namespace *dev_ns,
				     struct notifier_block *nb);

extern struct dev_namespace *copy_dev_ns(unsigned long flags,
					 struct task_struct *task);

/* helpers for per-driver logic of device namespace */

extern int register_dev_ns_ops(char *name, struct dev_ns_ops *ops);
extern void unregister_dev_ns_ops(int ns_id);
extern struct dev_ns_info *get_dev_ns_info(int ns_id,
					   struct dev_namespace *dev_ns,
					   bool lock, bool create);
extern struct dev_ns_info *get_dev_ns_info_task(int ns_id,
						struct task_struct *task);
extern void put_dev_ns_info(int ns_id,
			    struct dev_ns_info *dev_ns_info,
			    int lock);
extern void loop_dev_ns_info(int ns_id, void *ptr,
			     void (*func)(struct dev_ns_info *dev_ns_info,
					  void *ptr));

/* macro-mania to reduce repetitive code - not for the faint of heart */

#define i_to_x_dev_ns(i, x) container_of(i, struct x ## _dev_ns, dev_ns_info)

#define _dev_ns_id(X) \
	static int X ## _ns_id;

#define _dev_ns_get(X) \
	static inline \
	struct X ## _dev_ns *get_ ## X ## _ns(struct dev_namespace *dev_ns) \
	{ \
		struct dev_ns_info *info; \
		info = get_dev_ns_info(X ## _ns_id, dev_ns, 1, 1); \
		return info ? i_to_x_dev_ns(info, X) : NULL; \
	}

#define _dev_ns_find(X) \
	static inline \
	struct X ## _dev_ns *find_ ## X ## _ns(struct dev_namespace *dev_ns) \
	{ \
		struct dev_ns_info *info; \
		info = get_dev_ns_info(X ## _ns_id, dev_ns, 0, 0); \
		return info ? i_to_x_dev_ns(info, X) : NULL; \
	}


#define _dev_ns_get_cur(X) \
	static inline struct X ## _dev_ns *get_ ## X ## _ns_cur(void) \
	{ \
		struct dev_ns_info *info; \
		info = get_dev_ns_info_task(X ## _ns_id, current); \
		return info ? i_to_x_dev_ns(info, X) : NULL; \
	}

#define _dev_ns_put(X) \
	static inline void put_ ## X ## _ns(struct X ## _dev_ns *X ## _ns) \
	{ \
		put_dev_ns_info(X ## _ns_id, &X ## _ns->dev_ns_info, 1); \
	}

/*
 * Finally, this is what a driver author really needs to use:
 * DEFINE_DEV_NS_INFO(X): X_ns_id, put_X_ns(), get_X_ns(), get_X_ns_cur()
 * DEV_NS_REGISTER(X): will register X with device namespace
 * DEV_NS_UNREGISTER(X): will unregister X from device namespace
 */

#define DEFINE_DEV_NS_INFO(X) \
	_dev_ns_id(X) \
	_dev_ns_find(X) \
	_dev_ns_get(X) \
	_dev_ns_get_cur(X) \
	_dev_ns_put(X)

#define DEV_NS_REGISTER(X, s) \
	(X ## _ns_id = register_dev_ns_ops(s, &X ## _ns_ops))

#define DEV_NS_UNREGISTER(X) \
	unregister_dev_ns_ops(X ## _ns_id)


#else  /* !CONFIG_DEV_NS */

/* appease static assignment in kernel/nsproxy.c */
#define init_dev_ns (*(struct dev_namespace *) NULL)

/*
 * Driver authors should use this macro instead if !CONFIG_DEV_NS:
 * DEFINE_DEV_NS_INIT(X): put_X_ns(), get_X_ns(), get_X_ns_cur()
 */
#define DEFINE_DEV_NS_INIT(x) \
	struct x ## _dev_ns init_ ## x ## _ns; \
	static inline \
	struct x ## _dev_ns *find_ ## x ## _ns(struct dev_namespace *dev_ns) \
	{ return &init_ ## x ## _ns; } \
	struct x ## _dev_ns *get_ ## x ## _ns(struct dev_namespace *dev_ns) \
	{ return &init_ ## x ## _ns; } \
	static inline struct x ## _dev_ns *get_ ## x ## _ns_cur(void) \
	{ return &init_ ## x ## _ns; } \
	static inline void put_ ## x ## _ns(struct x ## _dev_ns *x ## _ns) \
	{ /* */ }

static inline struct dev_namespace *current_dev_ns(void)
{ return &init_dev_ns; }

static inline struct dev_namespace *copy_dev_ns(unsigned long flags,
						struct task_struct *task)
{
	if (flags & CLONE_NEWPID)
		return ERR_PTR(-EINVAL);
	return task->nsproxy->dev_ns;
}

static inline pid_t dev_ns_init_pid(struct dev_namespace *dev_ns)
{
	return init_task.pid;
}

static inline get_dev_ns_tag(char *to, struct dev_namespace *dev_ns)
{
	strcpy(to, "");
}

#endif /* CONFIG_DEV_NS */


#endif /* __KERNEL__ */
#endif /* _LINUX_DEV_NS_H */
