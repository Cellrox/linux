/* drivers/rtc/alarm-dev.c
 *
 * Copyright (C) 2007-2009 Google, Inc.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/time.h>
#include <linux/module.h>
#include <linux/device.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/platform_device.h>
#include <linux/sched.h>
#include <linux/spinlock.h>
#include <linux/uaccess.h>
#include <linux/alarmtimer.h>
#include <linux/wakelock.h>
#include <linux/slab.h>
#include <linux/dev_namespace.h>
#include "android_alarm.h"

#define ANDROID_ALARM_PRINT_INFO (1U << 0)
#define ANDROID_ALARM_PRINT_IO (1U << 1)
#define ANDROID_ALARM_PRINT_INT (1U << 2)

struct alarm_dev_ns;

struct devalarm {
	union {
		struct hrtimer hrt;
		struct alarm alrm;
	} u;
	enum android_alarm_type type;

	struct alarm_dev_ns *alarm_ns;
};

struct alarm_dev_ns {
	int               alarm_opened;
	spinlock_t        alarm_slock;
	struct wake_lock  alarm_wake_lock;
	wait_queue_head_t alarm_wait_queue;
	uint32_t          alarm_pending;
	uint32_t          alarm_enabled;
	uint32_t          wait_pending;

	struct devalarm   alarms[ANDROID_ALARM_TYPE_COUNT];
	char wakelock_name[32];

	struct dev_ns_info dev_ns_info;
};

static int debug_mask = ANDROID_ALARM_PRINT_INFO;
module_param_named(debug_mask, debug_mask, int, S_IRUGO | S_IWUSR | S_IWGRP);

#define pr_alarm(debug_level_mask, args...) \
	do { \
		if (debug_mask & ANDROID_ALARM_PRINT_##debug_level_mask) { \
			pr_info(args); \
		} \
	} while (0)

#define ANDROID_ALARM_WAKEUP_MASK ( \
	ANDROID_ALARM_RTC_WAKEUP_MASK | \
	ANDROID_ALARM_ELAPSED_REALTIME_WAKEUP_MASK)

/* support old usespace code */
#define ANDROID_ALARM_SET_OLD               _IOW('a', 2, time_t) /* set alarm */
#define ANDROID_ALARM_SET_AND_WAIT_OLD      _IOW('a', 3, time_t)

static void devalarm_alarms_init(struct devalarm *alarms);

static void alarm_ns_initialize(struct alarm_dev_ns *alarm_ns)
{
	alarm_ns->alarm_slock = __SPIN_LOCK_UNLOCKED(alarm_ns->alarm_slock);
	init_waitqueue_head(&alarm_ns->alarm_wait_queue);

	alarm_ns->alarm_pending = 0;
	alarm_ns->alarm_enabled = 0;
	alarm_ns->wait_pending = 0;

	devalarm_alarms_init(alarm_ns->alarms);

#ifdef CONFIG_HAS_WAKELOCK
#ifdef CONFIG_DEV_NS
	/* encode device-namespace into wakelock to ensure uniqueness */
	sprintf(alarm_ns->wakelock_name, "alarm[ns:%d]",
		dev_ns_init_pid(current_dev_ns()));
#else
	sprintf(alarm_ns->wakelock_name, "alarm");
#endif

	wake_lock_init(&alarm_ns->alarm_wake_lock, WAKE_LOCK_SUSPEND,
		       alarm_ns->wakelock_name);
#endif
}

static void alarm_ns_destroy(struct alarm_dev_ns *alarm_ns)
{
#ifdef CONFIG_HAS_WAKELOCK
	wake_lock_destroy(&alarm_ns->alarm_wake_lock);
#endif
}

#ifdef CONFIG_DEV_NS

/* alarm_ns_id, get_alarm_ns(), get_alarm_ns_cur(), put_alarm_ns() */
DEFINE_DEV_NS_INFO(alarm)

static struct dev_ns_info *alarm_ns_create(struct dev_namespace *dev_ns)
{
	struct alarm_dev_ns *alarm_ns;

	alarm_ns = kzalloc(sizeof(*alarm_ns), GFP_KERNEL);
	if (!alarm_ns)
		return ERR_PTR(-ENOMEM);

	alarm_ns_initialize(alarm_ns);

	return &alarm_ns->dev_ns_info;
}

static void alarm_ns_release(struct dev_ns_info *dev_ns_info)
{
	struct alarm_dev_ns *alarm_ns;

	alarm_ns = container_of(dev_ns_info, struct alarm_dev_ns, dev_ns_info);
	alarm_ns_destroy(alarm_ns);
	kfree(alarm_ns);
}

/*
 * If the RTC is set in any namespace (via alarm_set_rtc), make sure
 * the remaining namespaces are udpates suitably.
 */

static void update_alarm_time_change(struct alarm_dev_ns *alarm_ns);

static void propagate_alarm_time_change_func(struct dev_ns_info *dev_ns_info,
					     void *unused)
{
	struct alarm_dev_ns *alarm_ns;

	alarm_ns = container_of(dev_ns_info, struct alarm_dev_ns, dev_ns_info);
	update_alarm_time_change(alarm_ns);
}

static void propagate_alarm_time_change(void)
{
	loop_dev_ns_info(alarm_ns_id, NULL, propagate_alarm_time_change_func);
}


static struct dev_ns_ops alarm_ns_ops = {
	.create = alarm_ns_create,
	.release = alarm_ns_release,
};

#else

/* init_alarm_ns, get_alarm_ns(), get_alarm_ns_cur(), put_alarm_ns() */
DEFINE_DEV_NS_INIT(alarm)

#endif /* CONFIG_DEV_NS */


static int is_wakeup(enum android_alarm_type type)
{
	if (type == ANDROID_ALARM_RTC_WAKEUP ||
			type == ANDROID_ALARM_ELAPSED_REALTIME_WAKEUP)
		return 1;
	return 0;
}


static void devalarm_start(struct devalarm *alrm, ktime_t exp)
{
	if (is_wakeup(alrm->type))
		alarm_start(&alrm->u.alrm, exp);
	else
		hrtimer_start(&alrm->u.hrt, exp, HRTIMER_MODE_ABS);
}


static int devalarm_try_to_cancel(struct devalarm *alrm)
{
	int ret;
	if (is_wakeup(alrm->type))
		ret = alarm_try_to_cancel(&alrm->u.alrm);
	else
		ret = hrtimer_try_to_cancel(&alrm->u.hrt);
	return ret;
}

static void devalarm_cancel(struct devalarm *alrm)
{
	if (is_wakeup(alrm->type))
		alarm_cancel(&alrm->u.alrm);
	else
		hrtimer_cancel(&alrm->u.hrt);
}

static void update_alarm_time_change(struct alarm_dev_ns *alarm_ns)
{
	unsigned long flags;

	spin_lock_irqsave(&alarm_ns->alarm_slock, flags);
	alarm_ns->alarm_pending |= ANDROID_ALARM_TIME_CHANGE_MASK;
	wake_up(&alarm_ns->alarm_wait_queue);
	spin_unlock_irqrestore(&alarm_ns->alarm_slock, flags);
}

static long alarm_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	int rv = 0;
	unsigned long flags;
	struct timespec new_alarm_time;
	struct timespec new_rtc_time;
	struct timespec tmp_time;
	struct rtc_time new_rtc_tm;
	struct rtc_device *rtc_dev;
	enum android_alarm_type alarm_type = ANDROID_ALARM_IOCTL_TO_TYPE(cmd);
	uint32_t alarm_type_mask = 1U << alarm_type;
	struct alarm_dev_ns *alarm_ns;
	bool opened = false;

	if (alarm_type >= ANDROID_ALARM_TYPE_COUNT)
		return -EINVAL;

	alarm_ns = get_alarm_ns_cur();
	if (!alarm_ns)
		return -ENOMEM;

	if (ANDROID_ALARM_BASE_CMD(cmd) != ANDROID_ALARM_GET_TIME(0)) {
		if ((file->f_flags & O_ACCMODE) == O_RDONLY) {
			rv = -EPERM;
			goto err1;
		}

		if (file->private_data == NULL &&
		    cmd != ANDROID_ALARM_SET_RTC) {
			spin_lock_irqsave(&alarm_ns->alarm_slock, flags);
			if (alarm_ns->alarm_opened) {
				spin_unlock_irqrestore(&alarm_ns->alarm_slock,
						       flags);
				rv = -EBUSY;
				goto err1;
			}
			opened = true;
			alarm_ns->alarm_opened = 1;
			file->private_data = (void *)1;
			spin_unlock_irqrestore(&alarm_ns->alarm_slock, flags);
		}
	}

	switch (ANDROID_ALARM_BASE_CMD(cmd)) {
	case ANDROID_ALARM_CLEAR(0):
		spin_lock_irqsave(&alarm_ns->alarm_slock, flags);
		pr_alarm(IO, "alarm %d clear\n", alarm_type);
		devalarm_try_to_cancel(&alarm_ns->alarms[alarm_type]);
		if (alarm_ns->alarm_pending) {
			alarm_ns->alarm_pending &= ~alarm_type_mask;
			if (!alarm_ns->alarm_pending && !alarm_ns->wait_pending)
				wake_unlock(&alarm_ns->alarm_wake_lock);
		}
		alarm_ns->alarm_enabled &= ~alarm_type_mask;
		spin_unlock_irqrestore(&alarm_ns->alarm_slock, flags);
		break;

	case ANDROID_ALARM_SET_OLD:
	case ANDROID_ALARM_SET_AND_WAIT_OLD:
		if (get_user(new_alarm_time.tv_sec, (int __user *)arg)) {
			rv = -EFAULT;
			goto err1;
		}
		new_alarm_time.tv_nsec = 0;
		goto from_old_alarm_set;

	case ANDROID_ALARM_SET_AND_WAIT(0):
	case ANDROID_ALARM_SET(0):
		if (copy_from_user(&new_alarm_time, (void __user *)arg,
		    sizeof(new_alarm_time))) {
			rv = -EFAULT;
			goto err1;
		}
from_old_alarm_set:
		spin_lock_irqsave(&alarm_ns->alarm_slock, flags);
		pr_alarm(IO, "alarm %d set %ld.%09ld\n", alarm_type,
			new_alarm_time.tv_sec, new_alarm_time.tv_nsec);
		alarm_ns->alarm_enabled |= alarm_type_mask;
		devalarm_start(&alarm_ns->alarms[alarm_type],
			timespec_to_ktime(new_alarm_time));
		spin_unlock_irqrestore(&alarm_ns->alarm_slock, flags);
		if (ANDROID_ALARM_BASE_CMD(cmd) != ANDROID_ALARM_SET_AND_WAIT(0)
		    && cmd != ANDROID_ALARM_SET_AND_WAIT_OLD)
			break;
		/* fall though */
	case ANDROID_ALARM_WAIT:
		spin_lock_irqsave(&alarm_ns->alarm_slock, flags);
		pr_alarm(IO, "alarm wait\n");
		if (!alarm_ns->alarm_pending && alarm_ns->wait_pending) {
			wake_unlock(&alarm_ns->alarm_wake_lock);
			alarm_ns->wait_pending = 0;
		}
		spin_unlock_irqrestore(&alarm_ns->alarm_slock, flags);
		rv = wait_event_interruptible(alarm_ns->alarm_wait_queue,
					      alarm_ns->alarm_pending);
		if (rv)
			goto err1;
		spin_lock_irqsave(&alarm_ns->alarm_slock, flags);
		rv = alarm_ns->alarm_pending;
		alarm_ns->wait_pending = 1;
		alarm_ns->alarm_pending = 0;
		spin_unlock_irqrestore(&alarm_ns->alarm_slock, flags);
		break;
	case ANDROID_ALARM_SET_RTC:
		if (copy_from_user(&new_rtc_time, (void __user *)arg,
		    sizeof(new_rtc_time))) {
			rv = -EFAULT;
			goto err1;
		}
		rtc_time_to_tm(new_rtc_time.tv_sec, &new_rtc_tm);
		rtc_dev = alarmtimer_get_rtcdev();
		rv = do_settimeofday(&new_rtc_time);
		if (rv < 0)
			goto err1;
		if (rtc_dev)
			rv = rtc_set_time(rtc_dev, &new_rtc_tm);
		update_alarm_time_change(alarm_ns);
#ifdef CONFIG_DEV_NS
		propagate_alarm_time_change();
#endif
		if (rv < 0)
			goto err1;
		break;
	case ANDROID_ALARM_GET_TIME(0):
		switch (alarm_type) {
		case ANDROID_ALARM_RTC_WAKEUP:
		case ANDROID_ALARM_RTC:
			getnstimeofday(&tmp_time);
			break;
		case ANDROID_ALARM_ELAPSED_REALTIME_WAKEUP:
		case ANDROID_ALARM_ELAPSED_REALTIME:
			get_monotonic_boottime(&tmp_time);
			break;
		case ANDROID_ALARM_TYPE_COUNT:
		case ANDROID_ALARM_SYSTEMTIME:
			ktime_get_ts(&tmp_time);
			break;
		}
		if (copy_to_user((void __user *)arg, &tmp_time,
		    sizeof(tmp_time))) {
			rv = -EFAULT;
			goto err1;
		}
		break;

	default:
		rv = -EINVAL;
		goto err1;
	}
err1:
	if (!opened)
		put_alarm_ns(alarm_ns);
	return rv;
}

static int alarm_open(struct inode *inode, struct file *file)
{
	file->private_data = NULL;
	return 0;
}

static int alarm_release(struct inode *inode, struct file *file)
{
	int i;
	unsigned long flags;
	struct alarm_dev_ns *alarm_ns;

	alarm_ns = get_alarm_ns_cur();

	spin_lock_irqsave(&alarm_ns->alarm_slock, flags);
	if (file->private_data != 0) {
		for (i = 0; i < ANDROID_ALARM_TYPE_COUNT; i++) {
			uint32_t alarm_type_mask = 1U << i;
			if (alarm_ns->alarm_enabled & alarm_type_mask) {
				pr_alarm(INFO, "alarm_release: clear alarm, "
					"pending %d\n",
					!!(alarm_ns->alarm_pending &
					   alarm_type_mask));
				alarm_ns->alarm_enabled &= ~alarm_type_mask;
			}
			spin_unlock_irqrestore(&alarm_ns->alarm_slock, flags);
			devalarm_cancel(&alarm_ns->alarms[i]);
			spin_lock_irqsave(&alarm_ns->alarm_slock, flags);
		}
		if (alarm_ns->alarm_pending | alarm_ns->wait_pending) {
			if (alarm_ns->alarm_pending)
				pr_alarm(INFO, "alarm_release: clear "
					 "pending alarms %x\n",
					 alarm_ns->alarm_pending);
			wake_unlock(&alarm_ns->alarm_wake_lock);
			alarm_ns->wait_pending = 0;
			alarm_ns->alarm_pending = 0;
		}
		alarm_ns->alarm_opened = 0;
		put_alarm_ns(alarm_ns);  /* drop reference from open time */
	}
	spin_unlock_irqrestore(&alarm_ns->alarm_slock, flags);
	put_alarm_ns(alarm_ns);
	return 0;
}

static void devalarm_triggered(struct devalarm *alarm)
{
	unsigned long flags;
	uint32_t alarm_type_mask = 1U << alarm->type;
	struct alarm_dev_ns *alarm_ns = alarm->alarm_ns;

	pr_alarm(INT, "devalarm_triggered type %d\n", alarm->type);
	spin_lock_irqsave(&alarm_ns->alarm_slock, flags);
	if (alarm_ns->alarm_enabled & alarm_type_mask) {
		wake_lock_timeout(&alarm_ns->alarm_wake_lock, 5 * HZ);
		alarm_ns->alarm_enabled &= ~alarm_type_mask;
		alarm_ns->alarm_pending |= alarm_type_mask;
		wake_up(&alarm_ns->alarm_wait_queue);
	}
	spin_unlock_irqrestore(&alarm_ns->alarm_slock, flags);
}


static enum hrtimer_restart devalarm_hrthandler(struct hrtimer *hrt)
{
	struct devalarm *devalrm = container_of(hrt, struct devalarm, u.hrt);

	devalarm_triggered(devalrm);
	return HRTIMER_NORESTART;
}

static enum alarmtimer_restart devalarm_alarmhandler(struct alarm *alrm,
							ktime_t now)
{
	struct devalarm *devalrm = container_of(alrm, struct devalarm, u.alrm);

	devalarm_triggered(devalrm);
	return ALARMTIMER_NORESTART;
}


static const struct file_operations alarm_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = alarm_ioctl,
	.open = alarm_open,
	.release = alarm_release,
};

static struct miscdevice alarm_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "alarm",
	.fops = &alarm_fops,
};

static void devalarm_alarms_init(struct devalarm *alarms)
{
	struct alarm_dev_ns *alarm_ns =
		container_of(alarms, struct alarm_dev_ns, alarms[0]);
	int i;

	alarm_init(&alarms[ANDROID_ALARM_RTC_WAKEUP].u.alrm,
			ALARM_REALTIME, devalarm_alarmhandler);
	hrtimer_init(&alarms[ANDROID_ALARM_RTC].u.hrt,
			CLOCK_REALTIME, HRTIMER_MODE_ABS);
	alarm_init(&alarms[ANDROID_ALARM_ELAPSED_REALTIME_WAKEUP].u.alrm,
			ALARM_BOOTTIME, devalarm_alarmhandler);
	hrtimer_init(&alarms[ANDROID_ALARM_ELAPSED_REALTIME].u.hrt,
			CLOCK_BOOTTIME, HRTIMER_MODE_ABS);
	hrtimer_init(&alarms[ANDROID_ALARM_SYSTEMTIME].u.hrt,
			CLOCK_MONOTONIC, HRTIMER_MODE_ABS);

	for (i = 0; i < ANDROID_ALARM_TYPE_COUNT; i++) {
		alarms[i].type = i;
		alarms[i].alarm_ns = alarm_ns;
		if (!is_wakeup(i))
			alarms[i].u.hrt.function = devalarm_hrthandler;
	}

}

static int __init alarm_dev_init(void)
{
	int err;

	err = misc_register(&alarm_device);
	if (err)
		return err;

#ifdef CONFIG_DEV_NS
	err = DEV_NS_REGISTER(alarm, "alarm");
	if (err < 0) {
		misc_deregister(&alarm_device);
		return err;
	}
#else
	alarm_ns_initialize(&init_alarm_ns);
#endif
	return 0;
}

static void  __exit alarm_dev_exit(void)
{
	misc_deregister(&alarm_device);
#ifdef CONFIG_DEV_NS
	DEV_NS_UNREGISTER(alarm);
#else
	alarm_ns_destroy(&init_alarm_ns);
#endif
}

module_init(alarm_dev_init);
module_exit(alarm_dev_exit);

