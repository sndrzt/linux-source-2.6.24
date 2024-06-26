/*P:200 This contains all the /dev/lguest code, whereby the userspace launcher
 * controls and communicates with the Guest.  For example, the first write will
 * tell us the Guest's memory layout, pagetable, entry point and kernel address
 * offset.  A read will run the Guest until something happens, such as a signal
 * or the Guest doing a NOTIFY out to the Launcher. :*/
#include <linux/uaccess.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include "lg.h"

/*L:055 When something happens, the Waker process needs a way to stop the
 * kernel running the Guest and return to the Launcher.  So the Waker writes
 * LHREQ_BREAK and the value "1" to /dev/lguest to do this.  Once the Launcher
 * has done whatever needs attention, it writes LHREQ_BREAK and "0" to release
 * the Waker. */
static int break_guest_out(struct lg_cpu *cpu, const unsigned long __user*input)
{
	unsigned long on;

	/* Fetch whether they're turning break on or off. */
	if (get_user(on, input) != 0)
		return -EFAULT;

	if (on) {
		cpu->break_out = 1;
		/* Pop it out of the Guest (may be running on different CPU) */
		wake_up_process(cpu->tsk);
		/* Wait for them to reset it */
		return wait_event_interruptible(cpu->break_wq, !cpu->break_out);
	} else {
		cpu->break_out = 0;
		wake_up(&cpu->break_wq);
		return 0;
	}
}

/*L:050 Sending an interrupt is done by writing LHREQ_IRQ and an interrupt
 * number to /dev/lguest. */
static int user_send_irq(struct lg_cpu *cpu, const unsigned long __user *input)
{
	unsigned long irq;

	if (get_user(irq, input) != 0)
		return -EFAULT;
	if (irq >= LGUEST_IRQS)
		return -EINVAL;
	/* Next time the Guest runs, the core code will see if it can deliver
	 * this interrupt. */
	set_bit(irq, cpu->irqs_pending);
	return 0;
}

/*L:040 Once our Guest is initialized, the Launcher makes it run by reading
 * from /dev/lguest. */
static ssize_t read(struct file *file, char __user *user, size_t size,loff_t*o)
{
	struct lguest *lg = file->private_data;
	struct lg_cpu *cpu;
	unsigned int cpu_id = *o;

	/* You must write LHREQ_INITIALIZE first! */
	if (!lg)
		return -EINVAL;

	/* Watch out for arbitrary vcpu indexes! */
	if (cpu_id >= lg->nr_cpus)
		return -EINVAL;

	cpu = &lg->cpus[cpu_id];

	/* If you're not the task which owns the Guest, go away. */
	if (current != cpu->tsk)
		return -EPERM;

	/* If the guest is already dead, we indicate why */
	if (lg->dead) {
		size_t len;

		/* lg->dead either contains an error code, or a string. */
		if (IS_ERR(lg->dead))
			return PTR_ERR(lg->dead);

		/* We can only return as much as the buffer they read with. */
		len = min(size, strlen(lg->dead)+1);
		if (copy_to_user(user, lg->dead, len) != 0)
			return -EFAULT;
		return len;
	}

	/* If we returned from read() last time because the Guest notified,
	 * clear the flag. */
	if (cpu->pending_notify)
		cpu->pending_notify = 0;

	/* Run the Guest until something interesting happens. */
	return run_guest(cpu, (unsigned long __user *)user);
}

static int lg_cpu_start(struct lg_cpu *cpu, unsigned id, unsigned long start_ip)
{
	if (id >= NR_CPUS)
		return -EINVAL;

	cpu->id = id;
	cpu->lg = container_of((cpu - id), struct lguest, cpus[0]);
	cpu->lg->nr_cpus++;
	init_clockdev(cpu);

	/* We need a complete page for the Guest registers: they are accessible
	 * to the Guest and we can only grant it access to whole pages. */
	cpu->regs_page = get_zeroed_page(GFP_KERNEL);
	if (!cpu->regs_page)
		return -ENOMEM;

	/* We actually put the registers at the bottom of the page. */
	cpu->regs = (void *)cpu->regs_page + PAGE_SIZE - sizeof(*cpu->regs);

	/* Now we initialize the Guest's registers, handing it the start
	 * address. */
	lguest_arch_setup_regs(cpu, start_ip);

	/* Initialize the queue for the waker to wait on */
	init_waitqueue_head(&cpu->break_wq);

	/* We keep a pointer to the Launcher task (ie. current task) for when
	 * other Guests want to wake this one (inter-Guest I/O). */
	cpu->tsk = current;

	/* We need to keep a pointer to the Launcher's memory map, because if
	 * the Launcher dies we need to clean it up.  If we don't keep a
	 * reference, it is destroyed before close() is called. */
	cpu->mm = get_task_mm(cpu->tsk);

	return 0;
}

/*L:020 The initialization write supplies 4 pointer sized (32 or 64 bit)
 * values (in addition to the LHREQ_INITIALIZE value).  These are:
 *
 * base: The start of the Guest-physical memory inside the Launcher memory.
 *
 * pfnlimit: The highest (Guest-physical) page number the Guest should be
 * allowed to access.  The Guest memory lives inside the Launcher, so it sets
 * this to ensure the Guest can only reach its own memory.
 *
 * pgdir: The (Guest-physical) address of the top of the initial Guest
 * pagetables (which are set up by the Launcher).
 *
 * start: The first instruction to execute ("eip" in x86-speak).
 */
static int initialize(struct file *file, const unsigned long __user *input)
{
	/* "struct lguest" contains everything we (the Host) know about a
	 * Guest. */
	struct lguest *lg;
	int err;
	unsigned long args[4];

	/* We grab the Big Lguest lock, which protects against multiple
	 * simultaneous initializations. */
	mutex_lock(&lguest_lock);
	/* You can't initialize twice!  Close the device and start again... */
	if (file->private_data) {
		err = -EBUSY;
		goto unlock;
	}

	if (copy_from_user(args, input, sizeof(args)) != 0) {
		err = -EFAULT;
		goto unlock;
	}

	lg = kzalloc(sizeof(*lg), GFP_KERNEL);
	if (!lg) {
		err = -ENOMEM;
		goto unlock;
	}

	/* Populate the easy fields of our "struct lguest" */
	lg->mem_base = (void __user *)(long)args[0];
	lg->pfn_limit = args[1];

	/* This is the first cpu */
	err = lg_cpu_start(&lg->cpus[0], 0, args[3]);
	if (err)
		goto release_guest;

	/* Initialize the Guest's shadow page tables, using the toplevel
	 * address the Launcher gave us.  This allocates memory, so can
	 * fail. */
	err = init_guest_pagetable(lg, args[2]);
	if (err)
		goto free_regs;

	/* We remember which CPU's pages this Guest used last, for optimization
	 * when the same Guest runs on the same CPU twice. */
	lg->last_pages = NULL;

	/* We keep our "struct lguest" in the file's private_data. */
	file->private_data = lg;

	mutex_unlock(&lguest_lock);

	/* And because this is a write() call, we return the length used. */
	return sizeof(args);

free_regs:
	/* FIXME: This should be in free_vcpu */
	free_page(lg->cpus[0].regs_page);
release_guest:
	kfree(lg);
unlock:
	mutex_unlock(&lguest_lock);
	return err;
}

/*L:010 The first operation the Launcher does must be a write.  All writes
 * start with an unsigned long number: for the first write this must be
 * LHREQ_INITIALIZE to set up the Guest.  After that the Launcher can use
 * writes of other values to send interrupts. */
static ssize_t write(struct file *file, const char __user *in,
		     size_t size, loff_t *off)
{
	/* Once the guest is initialized, we hold the "struct lguest" in the
	 * file private data. */
	struct lguest *lg = file->private_data;
	const unsigned long __user *input = (const unsigned long __user *)in;
	unsigned long req;
	struct lg_cpu *uninitialized_var(cpu);
	unsigned int cpu_id = *off;

	if (get_user(req, input) != 0)
		return -EFAULT;
	input++;

	/* If you haven't initialized, you must do that first. */
	if (req != LHREQ_INITIALIZE) {
		if (!lg || (cpu_id >= lg->nr_cpus))
			return -EINVAL;
		cpu = &lg->cpus[cpu_id];
		if (!cpu)
			return -EINVAL;
	}

	/* Once the Guest is dead, all you can do is read() why it died. */
	if (lg && lg->dead)
		return -ENOENT;

	/* If you're not the task which owns the Guest, you can only break */
	if (lg && current != cpu->tsk && req != LHREQ_BREAK)
		return -EPERM;

	switch (req) {
	case LHREQ_INITIALIZE:
		return initialize(file, input);
	case LHREQ_IRQ:
		return user_send_irq(cpu, input);
	case LHREQ_BREAK:
		return break_guest_out(cpu, input);
	default:
		return -EINVAL;
	}
}

/*L:060 The final piece of interface code is the close() routine.  It reverses
 * everything done in initialize().  This is usually called because the
 * Launcher exited.
 *
 * Note that the close routine returns 0 or a negative error number: it can't
 * really fail, but it can whine.  I blame Sun for this wart, and K&R C for
 * letting them do it. :*/
static int close(struct inode *inode, struct file *file)
{
	struct lguest *lg = file->private_data;
	unsigned int i;

	/* If we never successfully initialized, there's nothing to clean up */
	if (!lg)
		return 0;

	/* We need the big lock, to protect from inter-guest I/O and other
	 * Launchers initializing guests. */
	mutex_lock(&lguest_lock);

	/* Free up the shadow page tables for the Guest. */
	free_guest_pagetable(lg);

	for (i = 0; i < lg->nr_cpus; i++) {
		/* Cancels the hrtimer set via LHCALL_SET_CLOCKEVENT. */
		hrtimer_cancel(&lg->cpus[i].hrt);
		/* We can free up the register page we allocated. */
		free_page(lg->cpus[i].regs_page);
		/* Now all the memory cleanups are done, it's safe to release
		 * the Launcher's memory management structure. */
		mmput(lg->cpus[i].mm);
	}
	/* If lg->dead doesn't contain an error code it will be NULL or a
	 * kmalloc()ed string, either of which is ok to hand to kfree(). */
	if (!IS_ERR(lg->dead))
		kfree(lg->dead);
	/* We clear the entire structure, which also marks it as free for the
	 * next user. */
	memset(lg, 0, sizeof(*lg));
	/* Release lock and exit. */
	mutex_unlock(&lguest_lock);

	return 0;
}

/*L:000
 * Welcome to our journey through the Launcher!
 *
 * The Launcher is the Host userspace program which sets up, runs and services
 * the Guest.  In fact, many comments in the Drivers which refer to "the Host"
 * doing things are inaccurate: the Launcher does all the device handling for
 * the Guest, but the Guest can't know that.
 *
 * Just to confuse you: to the Host kernel, the Launcher *is* the Guest and we
 * shall see more of that later.
 *
 * We begin our understanding with the Host kernel interface which the Launcher
 * uses: reading and writing a character device called /dev/lguest.  All the
 * work happens in the read(), write() and close() routines: */
static struct file_operations lguest_fops = {
	.owner	 = THIS_MODULE,
	.release = close,
	.write	 = write,
	.read	 = read,
};

/* This is a textbook example of a "misc" character device.  Populate a "struct
 * miscdevice" and register it with misc_register(). */
static struct miscdevice lguest_dev = {
	.minor	= MISC_DYNAMIC_MINOR,
	.name	= "lguest",
	.fops	= &lguest_fops,
};

int __init lguest_device_init(void)
{
	return misc_register(&lguest_dev);
}

void __exit lguest_device_remove(void)
{
	misc_deregister(&lguest_dev);
}
