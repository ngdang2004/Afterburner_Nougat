#include <linux/atomic.h>
#include <linux/cpu.h>
#include <linux/cpufreq.h>
#include <linux/fb.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/sched.h>

#if defined(CONFIG_POWERSUSPEND)
#include <linux/powersuspend.h>
#endif

#define HOTPLUG_BOOSTED

#if defined(HOTPLUG_BOOSTED)
#include "cpu_load_metric.h"
#include <../drivers/gpu/arm/t7xx/r15p0/platform/exynos/mali_kbase_platform.h>
#endif

static struct delayed_work exynos_hotplug;
static struct workqueue_struct *khotplug_wq;

enum hstate {
	H0,
	H1,
	H2,
	H3,
	H4,
	H5,
	H6,
	H7,
	MAX_HSTATE,
};

enum action {
	DOWN,
	UP,
	STAY,
};

struct hotplug_hstates_usage {
	unsigned long time;
};

struct exynos_hotplug_ctrl {
	ktime_t last_time;
	ktime_t last_check_time;
	unsigned int sampling_rate;
	unsigned int up_threshold;
	unsigned int down_threshold;
	unsigned int up_tasks;
	unsigned int down_tasks;
#if defined(HOTPLUG_BOOSTED)
	unsigned int gpu_load_threshold;
	unsigned int cpu_load_threshold;
#endif
	int max_lock;
	int min_lock;
	int force_hstate;
	int cur_hstate;
	enum hstate old_state;
	struct hotplug_hstates_usage usage[MAX_HSTATE];
};

struct hotplug_hstate {
	char *name;
	unsigned int core_count;
	enum hstate state;
};

static struct hotplug_hstate hstate_set[] = {
	[H0] = {
		.name		= "H0",
		.core_count	= NR_CPUS,
		.state		= H0,
	},
	[H1] = {
		.name		= "H1",
		.core_count	= 7,
		.state		= H1,
	},
	[H2] = {
		.name		= "H2",
		.core_count	= 6,
		.state		= H2,
	},
	[H3] = {
		.name		= "H3",
		.core_count	= 5,
		.state		= H3,
	},
	[H4] = {
		.name		= "H4",
		.core_count	= 4,
		.state		= H4,
	},
	[H5] = {
		.name		= "H5",
		.core_count	= 3,
		.state		= H5,
	},
	[H6] = {
		.name		= "H6",
		.core_count	= 2,
		.state		= H6,
	},
	[H7] = {
		.name		= "H7",
		.core_count	= 1,
		.state		= H7,
	},
};

#define SUSPENDED_MIN_STATE 	H6
#define SCREEN_ON_MAX_STATE 	H6
#define WAKE_UP_STATE			H0
#define AWAKE_SAMPLING_RATE 	100		// 100ms (Stock)
#define ASLEEP_SAMPLING_RATE 	1000	// 1s
#define CPU_DOWN_LOAD			25		// If load is less than 25 percent then it will turn off cores
#define CPU_UP_LOAD				60
#define GPU_UP_LOAD				80

static struct exynos_hotplug_ctrl ctrl_hotplug = {
	.sampling_rate = AWAKE_SAMPLING_RATE,		/* ms */
	.up_threshold = 3,
	.down_threshold = 3,
	.up_tasks = 2,
	.down_tasks = 1,
	.force_hstate = -1,
	.min_lock = -1,
	.max_lock = -1,
	.cur_hstate = H0,
	.old_state = H0,
#if defined(HOTPLUG_BOOSTED)
	.gpu_load_threshold = GPU_UP_LOAD,
	.cpu_load_threshold = CPU_UP_LOAD,
#endif
};

static DEFINE_MUTEX(hotplug_lock);
static DEFINE_SPINLOCK(hstate_status_lock);

static atomic_t freq_history[STAY] =  {ATOMIC_INIT(0), ATOMIC_INIT(0)};

/*
 * If 'state' is less than "MAX_STATE"
 *	return core_count of 'state'
 * else
 *	return core count of 'H0'
 */
static int get_core_count(enum hstate state)
{
	if (state < MAX_HSTATE)
		return hstate_set[state].core_count;
	else
		return hstate_set[H0].core_count;
}

static void __ref hotplug_cpu(enum hstate state)
{
	int i, cnt_target, num_online, least_busy_cpu;

	cnt_target = get_core_count(state);

	/* Check the Online CPU supposed to be online or offline */
	for (i = 0 ; i < NR_CPUS ; i++) {
		num_online = num_online_cpus();
	
		if(num_online == cnt_target) 
		{
			break;
		}
		
		if (cnt_target > num_online) {
			if (!cpu_online(i))
				cpu_up(i);
		} else {
			least_busy_cpu = get_least_busy_cpu();
		
			cpu_down(least_busy_cpu);
		}
	}
}

static s64 hotplug_update_time_status(void)
{
	ktime_t curr_time, last_time;
	s64 diff;

	curr_time = ktime_get();
	last_time = ctrl_hotplug.last_time;

	diff = ktime_to_ms(ktime_sub(curr_time, last_time));

	if (diff > INT_MAX)
		diff = INT_MAX;

	ctrl_hotplug.usage[ctrl_hotplug.old_state].time += diff;
	ctrl_hotplug.last_time = curr_time;

	return diff;
}

static void hotplug_enter_hstate(bool force, enum hstate state)
{
	int min_state, max_state;

	if (!force) {
		min_state = ctrl_hotplug.min_lock;
		max_state = ctrl_hotplug.max_lock;

		if (min_state >= 0 && state > min_state)
			state = min_state;

		if (max_state > 0 && state < max_state)
			state = max_state;
	}
	
	// min state when not suspended is SCREEN_ON_MAX_STATE
	if (!power_suspend_active && state > SCREEN_ON_MAX_STATE)
		state = SCREEN_ON_MAX_STATE;
	else if (power_suspend_active && state < SUSPENDED_MIN_STATE)
		state = SUSPENDED_MIN_STATE;

	if (ctrl_hotplug.old_state == state)
		return;

	spin_lock(&hstate_status_lock);
	hotplug_update_time_status();
	spin_unlock(&hstate_status_lock);

	hotplug_cpu(state);

	atomic_set(&freq_history[UP], 0);
	atomic_set(&freq_history[DOWN], 0);

	spin_lock(&hstate_status_lock);
	hotplug_update_time_status();
	spin_unlock(&hstate_status_lock);

	ctrl_hotplug.old_state = state;
	ctrl_hotplug.cur_hstate = state;
}

static enum action select_up_down(void)
{
	int up_threshold, down_threshold;
	unsigned int cpu_load;
	int nr, num_online;
	bool boosted = false;

	nr = nr_running();

	up_threshold = ctrl_hotplug.up_threshold;
	down_threshold = ctrl_hotplug.down_threshold;
	
	num_online = num_online_cpus();
	cpu_load = cpu_get_avg_load();
	
#if defined(HOTPLUG_BOOSTED)
	boosted = (gpu_get_load() >= ctrl_hotplug.gpu_load_threshold);
#endif

	if (((num_online * ctrl_hotplug.down_tasks) >= nr)
#if defined(HOTPLUG_BOOSTED)
			&& !boosted
#endif
			) {
		if (cpu_load <= CPU_DOWN_LOAD) {
			atomic_inc(&freq_history[DOWN]);
			atomic_set(&freq_history[UP], 0);
		} else {
			atomic_set(&freq_history[UP], 0);
			atomic_set(&freq_history[DOWN], 0);
		}
	} else if (((cpu_load >= CPU_UP_LOAD) && ((num_online * ctrl_hotplug.up_tasks) <= nr)) 
#if defined(HOTPLUG_BOOSTED)
			|| boosted
#endif
			) {
		atomic_inc(&freq_history[UP]);
		atomic_set(&freq_history[DOWN], 0);
	} /* else if nothing matched up then we just leave the UP/DOWN history alone */

	if (atomic_read(&freq_history[UP]) > up_threshold)
		return UP;
	else if (atomic_read(&freq_history[DOWN]) > down_threshold)
		return DOWN;

	return STAY;
}

static enum hstate hotplug_adjust_state(enum action move)
{
	int state;
	state = ctrl_hotplug.old_state;

	if (move == DOWN) {
		state++;
		if (state >= MAX_HSTATE)
			state = MAX_HSTATE - 1;
	} else if (move != STAY){
		state -= 4;		// turn on 4 cores when moving up
		if(state < 0)
			state = 0;
	} 

	return state;
}

static void exynos_work(struct work_struct *dwork)
{
	enum action move = select_up_down();
	enum hstate target_state;

	mutex_lock(&hotplug_lock);

	target_state = hotplug_adjust_state(move);
		
	if(power_suspend_active && (ctrl_hotplug.sampling_rate == AWAKE_SAMPLING_RATE)) {
		hotplug_enter_hstate(false, SUSPENDED_MIN_STATE);
		
		ctrl_hotplug.sampling_rate = ASLEEP_SAMPLING_RATE;
		
	} else if(!power_suspend_active && (ctrl_hotplug.sampling_rate == ASLEEP_SAMPLING_RATE)) {
		hotplug_enter_hstate(true, WAKE_UP_STATE);		// just woke up, so give a boost
		
		ctrl_hotplug.sampling_rate = AWAKE_SAMPLING_RATE;
	} else if ((get_core_count(ctrl_hotplug.old_state) != num_online_cpus())
		|| (move != STAY)) {
		hotplug_enter_hstate(false, target_state);
	}

	queue_delayed_work_on(0, khotplug_wq, &exynos_hotplug, msecs_to_jiffies(ctrl_hotplug.sampling_rate));
	mutex_unlock(&hotplug_lock);
}

#define define_show_state_function(_name) \
static ssize_t show_##_name(struct device *dev, struct device_attribute *attr, \
			char *buf) \
{ \
	return sprintf(buf, "%d\n", ctrl_hotplug._name); \
}

#define define_store_state_function(_name) \
static ssize_t store_##_name(struct device *dev, struct device_attribute *attr, \
		const char *buf, size_t count) \
{ \
	unsigned long value; \
	int ret; \
	ret = kstrtoul(buf, 10, &value); \
	if (ret) \
		return ret; \
	ctrl_hotplug._name = value; \
	return ret ? ret : count; \
}

define_show_state_function(up_threshold)
define_store_state_function(up_threshold)

define_show_state_function(down_threshold)
define_store_state_function(down_threshold)

define_show_state_function(sampling_rate)
define_store_state_function(sampling_rate)

define_show_state_function(up_tasks)
define_store_state_function(up_tasks)

define_show_state_function(down_tasks)
define_store_state_function(down_tasks)

define_show_state_function(min_lock)

define_show_state_function(max_lock)

define_show_state_function(cur_hstate)

define_show_state_function(force_hstate)

void __set_force_hstate(int target_state)
{
	if (target_state < 0) {
		mutex_lock(&hotplug_lock);
		ctrl_hotplug.force_hstate = -1;
		queue_delayed_work_on(0, khotplug_wq, &exynos_hotplug,
				msecs_to_jiffies(ctrl_hotplug.sampling_rate));
	} else {
		cancel_delayed_work_sync(&exynos_hotplug);

		mutex_lock(&hotplug_lock);
		hotplug_enter_hstate(true, target_state);
		ctrl_hotplug.force_hstate = target_state;
	}

	mutex_unlock(&hotplug_lock);
}

static ssize_t store_force_hstate(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	int ret, target_state;

	ret = sscanf(buf, "%d", &target_state);
	if (ret != 1 || target_state >= MAX_HSTATE)
		return -EINVAL;

	__set_force_hstate(target_state);

	return count;
}

static void __force_hstate(int target_state, int *value)
{
	if (target_state < 0) {
		mutex_lock(&hotplug_lock);
		*value = -1;
	} else {
		cancel_delayed_work_sync(&exynos_hotplug);

		mutex_lock(&hotplug_lock);
		hotplug_enter_hstate(true, target_state);
		*value = target_state;
	}

	queue_delayed_work_on(0, khotplug_wq, &exynos_hotplug,
			msecs_to_jiffies(ctrl_hotplug.sampling_rate));

	mutex_unlock(&hotplug_lock);
}

static ssize_t store_max_lock(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	int max_state;
	int state;

	int ret, target_state;

	ret = sscanf(buf, "%d", &target_state);
	if (ret != 1 || target_state >= MAX_HSTATE)
		return -EINVAL;

	max_state = target_state;
	state = target_state;

	mutex_lock(&hotplug_lock);

	if (ctrl_hotplug.force_hstate != -1) {
		mutex_unlock(&hotplug_lock);
		return count;
	}

	if (state < 0) {
		mutex_unlock(&hotplug_lock);
		goto out;
	}

	if (ctrl_hotplug.min_lock >= 0)
		state = ctrl_hotplug.min_lock;

	if (max_state >= 0 && state <= max_state)
		state = max_state;

	if ((int)ctrl_hotplug.old_state > state) {
		ctrl_hotplug.max_lock = state;
		mutex_unlock(&hotplug_lock);
		return count;
	}

	mutex_unlock(&hotplug_lock);

out:
	__force_hstate(state, &ctrl_hotplug.max_lock);

	return count;
}

static ssize_t store_min_lock(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	int max_state = -1;
	int state;

	int ret, target_state;

	ret = sscanf(buf, "%d", &target_state);
	if (ret != 1 || target_state >= MAX_HSTATE)
		return -EINVAL;

	state = target_state;

	mutex_lock(&hotplug_lock);

	if (ctrl_hotplug.force_hstate != -1) {
		mutex_unlock(&hotplug_lock);
		return count;
	}

	if (state < 0) {
		mutex_unlock(&hotplug_lock);
		goto out;
	}

	if (ctrl_hotplug.max_lock >= 0)
		max_state = ctrl_hotplug.max_lock;

	if (max_state >= 0 && state <= max_state)
		state = max_state;

	if ((int)ctrl_hotplug.old_state < state) {
		ctrl_hotplug.min_lock = state;
		mutex_unlock(&hotplug_lock);
		return count;
	}

	mutex_unlock(&hotplug_lock);

out:
	__force_hstate(state, &ctrl_hotplug.min_lock);

	return count;
}

static ssize_t show_time_in_state(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	ssize_t len = 0;
	int i;

	spin_lock(&hstate_status_lock);
	hotplug_update_time_status();
	spin_unlock(&hstate_status_lock);

	for (i = 0; i < MAX_HSTATE; i++) {
		len += sprintf(buf + len, "%s %llu\n", hstate_set[i].name,
				(unsigned long long)ctrl_hotplug.usage[i].time);
	}
	return len;
}

void exynos_dm_hotplug_disable(void)
{

}

void exynos_dm_hotplug_enable(void)
{

}

static DEVICE_ATTR(up_threshold, S_IRUGO | S_IWUSR, show_up_threshold, store_up_threshold);
static DEVICE_ATTR(down_threshold, S_IRUGO | S_IWUSR, show_down_threshold, store_down_threshold);
static DEVICE_ATTR(sampling_rate, S_IRUGO | S_IWUSR, show_sampling_rate, store_sampling_rate);
static DEVICE_ATTR(up_tasks, S_IRUGO | S_IWUSR, show_up_tasks, store_up_tasks);
static DEVICE_ATTR(down_tasks, S_IRUGO | S_IWUSR, show_down_tasks, store_down_tasks);
static DEVICE_ATTR(force_hstate, S_IRUGO | S_IWUSR, show_force_hstate, store_force_hstate);
static DEVICE_ATTR(cur_hstate, S_IRUGO, show_cur_hstate, NULL);
static DEVICE_ATTR(min_lock, S_IRUGO | S_IWUSR, show_min_lock, store_min_lock);
static DEVICE_ATTR(max_lock, S_IRUGO | S_IWUSR, show_max_lock, store_max_lock);

static DEVICE_ATTR(time_in_state, S_IRUGO, show_time_in_state, NULL);

static struct attribute *clusterhotplug_default_attrs[] = {
	&dev_attr_up_threshold.attr,
	&dev_attr_down_threshold.attr,
	&dev_attr_sampling_rate.attr,
	&dev_attr_up_tasks.attr,
	&dev_attr_down_tasks.attr,
	&dev_attr_force_hstate.attr,
	&dev_attr_cur_hstate.attr,
	&dev_attr_time_in_state.attr,
	&dev_attr_min_lock.attr,
	&dev_attr_max_lock.attr,
	NULL
};

static struct attribute_group clusterhotplug_attr_group = {
	.attrs = clusterhotplug_default_attrs,
	.name = "clusterhotplug",
};

static int __init dm_cluster_hotplug_init(void)
{
	int ret;

	INIT_DEFERRABLE_WORK(&exynos_hotplug, exynos_work);

	khotplug_wq = alloc_workqueue("khotplug", WQ_FREEZABLE, 0);
	if (!khotplug_wq) {
		pr_err("Failed to create khotplug workqueue\n");
		ret = -EFAULT;
		goto err_wq;
	}

	ret = sysfs_create_group(&cpu_subsys.dev_root->kobj, &clusterhotplug_attr_group);
	if (ret) {
		pr_err("Failed to create sysfs for hotplug\n");
		goto err_sys;
	}

	queue_delayed_work_on(0, khotplug_wq, &exynos_hotplug, msecs_to_jiffies(ctrl_hotplug.sampling_rate) * 250);

	return 0;

err_sys:
	destroy_workqueue(khotplug_wq);
err_wq:
	return ret;
}
late_initcall(dm_cluster_hotplug_init);
