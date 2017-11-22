
/* rwsem-any.c: run some threads to do R/W semaphore tests
*
* Copyright (C) 2004 Red Hat, Inc. All Rights Reserved.
* Written by David Howells (dhowells@xxxxxxxxxx)
*
* This program is free software; you can redistribute it and/or
* modify it under the terms of the GNU General Public License
* as published by the Free Software Foundation; either version
* 2 of the License, or (at your option) any later version.
*/

#include <linux/module.h>
#include <linux/poll.h>
#include <linux/moduleparam.h>
#include <linux/stat.h>
#include <linux/init.h>
#include <asm/atomic.h>
#include <linux/personality.h>
#include <linux/smp_lock.h>
#include <linux/delay.h>
#include <linux/timer.h>
#include <linux/completion.h>

#define SCHED

static int numrd = 1, numwr = 1, numdg = 1, elapse = 5;

MODULE_AUTHOR("David Howells");
MODULE_DESCRIPTION("R/W semaphore test demo");
MODULE_LICENSE("GPL");

module_param(numrd, int, 0);
MODULE_PARM_DESC(numrd, "Number of reader threads");

module_param(numwr, int, 0);
MODULE_PARM_DESC(numwr, "Number of writer threads");

module_param(numdg, int, 0);
MODULE_PARM_DESC(numdg, "Number of downgrader threads");

module_param(elapse, int, 0);
MODULE_PARM_DESC(elapse, "Number of seconds to run for");

#ifdef SCHED
static int do_sched = 0;
module_param(do_sched, int, 0);
MODULE_PARM_DESC(numdg, "True if each thread should schedule regularly");
#endif

/* the semaphore under test */
static struct rw_semaphore rwsem_sem;


#ifdef DEBUG_RWSEM
extern struct rw_semaphore *rwsem_to_monitor;
#endif

static atomic_t readers = ATOMIC_INIT(0);
static atomic_t writers = ATOMIC_INIT(0);
static atomic_t do_stuff = ATOMIC_INIT(0);
static atomic_t reads_taken = ATOMIC_INIT(0);
static atomic_t writes_taken = ATOMIC_INIT(0);
static atomic_t downgrades_taken = ATOMIC_INIT(0);

static struct completion rd_comp[20], wr_comp[20], dg_comp[20];

static struct timer_list timer;

#define CHECK(expr) \
do { \
if (!(expr)) \
printk("check " #expr " failed in %s\n", __func__); \
} while (0)

#define CHECKA(var, val) \
do { \
int x = atomic_read(&(var)); \
if (x != (val)) \
printk("check [%s != %d, == %d] failed in %s\n", #var, (val), x, __func__); \
} while (0)

static inline void dr(void)
{
		down_read(&rwsem_sem);
		atomic_inc(&readers);
		atomic_inc(&reads_taken);
		CHECKA(writers, 0);
}

static inline void ur(void)
{
		CHECKA(writers, 0);
		atomic_dec(&readers);
		up_read(&rwsem_sem);
}

static inline void dw(void)
{
		down_write(&rwsem_sem);
		atomic_inc(&writers);
		atomic_inc(&writes_taken);
		CHECKA(writers, 1);
		CHECKA(readers, 0);
}

static inline void uw(void)
{
		CHECKA(writers, 1);
		CHECKA(readers, 0);
		atomic_dec(&writers);
		up_write(&rwsem_sem);
}

static inline void dgw(void)
{
		CHECKA(writers, 1);
		CHECKA(readers, 0);
		atomic_dec(&writers);
		atomic_inc(&readers);
		downgrade_write(&rwsem_sem);
		atomic_inc(&downgrades_taken);
}

static inline void sched(void)
{
#ifdef SCHED
		if (do_sched)
				schedule();
#endif
}

int reader(void *arg)
{
		unsigned int N = (unsigned long) arg;

		daemonize("Read%u", N);

		while (atomic_read(&do_stuff)) {
				dr();
				ur();
				sched();
		}

		printk("%s: done\n", current->comm);
		complete_and_exit(&rd_comp[N], 0);
}

int writer(void *arg)
{
		unsigned int N = (unsigned long) arg;

		daemonize("Write%u", N);

		while (atomic_read(&do_stuff)) {
				dw();
				uw();
				sched();
		}

		printk("%s: done\n", current->comm);
		complete_and_exit(&wr_comp[N], 0);
}

int downgrader(void *arg)
{
		unsigned int N = (unsigned long) arg;

		daemonize("Down%u", N);

		while (atomic_read(&do_stuff)) {
				dw();
				dgw();
				ur();
				sched();
		}

		printk("%s: done\n", current->comm);
		complete_and_exit(&dg_comp[N], 0);
}

static void stop_test(unsigned long dummy)
{
		atomic_set(&do_stuff, 0);
}

/*****************************************************************************/
/*
*
*/
static int __init rwsem_init_module(void)
{
		unsigned long loop;

		printk("\nrwsem_any starting tests...\n");

		init_rwsem(&rwsem_sem);
		atomic_set(&do_stuff, 1);

#ifdef DEBUG_RWSEM
		rwsem_to_monitor = &rwsem_sem;
#endif

		/* kick off all the children */
		for (loop = 0; loop < 20; loop++) {
				if (loop < numrd) {
						init_completion(&rd_comp[loop]);
						kernel_thread(reader, (void *) loop, 0);
				}

				if (loop < numwr) {
						init_completion(&wr_comp[loop]);
						kernel_thread(writer, (void *) loop, 0);
				}

				if (loop < numdg) {
						init_completion(&dg_comp[loop]);
						kernel_thread(downgrader, (void *) loop, 0);
				}
		}

		/* set a stop timer */
		init_timer(&timer);
		timer.function = stop_test;
		timer.expires = jiffies + elapse * HZ;
		add_timer(&timer);

		/* now wait until it's all done */
		for (loop = 0; loop < numrd; loop++)
				wait_for_completion(&rd_comp[loop]);

		for (loop = 0; loop < numwr; loop++)
				wait_for_completion(&wr_comp[loop]);

		for (loop = 0; loop < numdg; loop++)
				wait_for_completion(&dg_comp[loop]);

		atomic_set(&do_stuff, 0);
		del_timer(&timer);

		/* print the results */
#ifdef CONFIG_RWSEM_XCHGADD_ALGORITHM
		printk("rwsem counter = %ld\n", rwsem_sem.count);
#else
		printk("rwsem counter = %d\n", rwsem_sem.activity);
#endif

		printk("reads taken: %d\n", atomic_read(&reads_taken));
		printk("writes taken: %d\n", atomic_read(&writes_taken));
		printk("downgrades taken: %d\n", atomic_read(&downgrades_taken));

#ifdef DEBUG_RWSEM
		rwsem_to_monitor = NULL;
#endif

		/* tell insmod to discard the module */
		return -ENOANO;
} /* end rwsem_init_module() */

module_init(rwsem_init_module);
