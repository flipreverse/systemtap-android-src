/* -*- linux-c -*-
 *
 * /proc transport and control
 * Copyright (C) 2005 Red Hat Inc.
 *
 * This file is part of systemtap, and is free software.  You can
 * redistribute it and/or modify it under the terms of the GNU General
 * Public License (GPL); either version 2, or (at your option) any
 * later version.
 */

#define STP_DEFAULT_BUFFERS 128
static int _stp_current_buffers = STP_DEFAULT_BUFFERS;

static struct list_head _stp_ready_q;
static struct list_head _stp_pool_q;
spinlock_t _stp_pool_lock = SPIN_LOCK_UNLOCKED;
spinlock_t _stp_ready_lock = SPIN_LOCK_UNLOCKED;

#ifdef STP_RELAYFS
/* handle the per-cpu subbuf info read for relayfs */
static ssize_t
_stp_proc_read (struct file *file, char __user *buf, size_t count, loff_t *ppos)
{
	int num;
	struct buf_info out;

	int cpu = *(int *)(PDE(file->f_dentry->d_inode)->data);

	if (!_stp_chan)
		return -EINVAL;

	out.cpu = cpu;
	out.produced = atomic_read(&_stp_chan->buf[cpu]->subbufs_produced);
	out.consumed = atomic_read(&_stp_chan->buf[cpu]->subbufs_consumed);

	num = sizeof(out);
	if (copy_to_user(buf, &out, num))
		return -EFAULT;

	return num;
}

/* handle the per-cpu subbuf info write for relayfs */
static ssize_t _stp_proc_write (struct file *file, const char __user *buf,
				size_t count, loff_t *ppos)
{
	struct consumed_info info;
	int cpu = *(int *)(PDE(file->f_dentry->d_inode)->data);
	if (copy_from_user(&info, buf, count))
		return -EFAULT;

	relay_subbufs_consumed(_stp_chan, cpu, info.consumed);
	return count;
}

static struct file_operations _stp_proc_fops = {
	.read = _stp_proc_read,
	.write = _stp_proc_write,
};
#endif

static ssize_t _stp_proc_write_cmd (struct file *file, const char __user *buf,
				    size_t count, loff_t *ppos)
{
	int type;

	if (count < sizeof(int))
		return 0;

	if (get_user(type, (int __user *)buf))
		return -EFAULT;

	//printk ("_stp_proc_write_cmd. count:%d type:%d\n", count, type);

	count -= sizeof(int);

	switch (type) {
	case STP_START:
	{
		struct transport_start st;
		if (count < sizeof(struct transport_start))
			return 0;
		if (copy_from_user (&st, buf, sizeof(struct transport_start)))
			return -EFAULT;
		_stp_handle_start (&st);
		break;
	}
	case STP_EXIT:
		_stp_handle_exit(NULL);
		break;
	case STP_TRANSPORT_INFO:
	{
		struct transport_info ti;
		//printk("STP_TRANSPORT_INFO %d %d\n", count, sizeof(struct transport_info));
		if (count < sizeof(struct transport_info))
			return 0;
		if (copy_from_user (&ti, &buf[4], sizeof(struct transport_info)))
			return -EFAULT;
		_stp_transport_open (&ti);
		break;
	}
	default:
		printk ("invalid command type %d\n", type);
		return -EINVAL;
	}

	return count;
}

struct _stp_buffer {
	struct list_head list;
	int len;
	char buf[STP_BUFFER_SIZE];
};

static DECLARE_WAIT_QUEUE_HEAD(_stp_proc_wq);

static int _stp_write (int type, void *data, int len)
{
	struct _stp_buffer *bptr;

	spin_lock(&_stp_pool_lock);
	if (list_empty(&_stp_pool_q)) {
		spin_unlock(&_stp_pool_lock);
		return -1;
	}

	/* get the next buffer from the pool */
	bptr = (struct _stp_buffer *)_stp_pool_q.next;
	list_del_init(&bptr->list);
	spin_unlock(&_stp_pool_lock);

	memcpy (bptr->buf, &type, 4);
	memcpy (&bptr->buf[4], data, len);
	bptr->len = len;
	

	/* put it on the pool of ready buffers */
	spin_lock(&_stp_ready_lock);
	list_add_tail(&bptr->list, &_stp_ready_q);
	spin_unlock(&_stp_ready_lock);

	/* now wake up readers */
	wake_up_interruptible(&_stp_proc_wq);

	return len;
}


static ssize_t
_stp_proc_read_cmd (struct file *file, char __user *buf, size_t count, loff_t *ppos)
{
	struct _stp_buffer *bptr;
	int len;

	/* wait for nonempty ready queue */
	spin_lock(&_stp_ready_lock);
	while (list_empty(&_stp_ready_q)) {
		spin_unlock(&_stp_ready_lock);
		if (file->f_flags & O_NONBLOCK)
			return -EAGAIN;
		if (wait_event_interruptible(_stp_proc_wq, !list_empty(&_stp_ready_q)))
			return -ERESTARTSYS;
		spin_lock(&_stp_ready_lock);
	}
  
	/* get the next buffer off the ready list */
	bptr = (struct _stp_buffer *)_stp_ready_q.next;
	list_del_init(&bptr->list);
	spin_unlock(&_stp_ready_lock);

	/* write it out */
	len = bptr->len + 4;
	if (copy_to_user(buf, bptr->buf, len)) {
		/* now what?  We took it off the queue then failed to send it */
		/* we can't put it back on the queue because it will likely be out-of-order */
		/* fortunately this should never happen */
		/* FIXME need to mark this as a transport failure */
		return -EFAULT;
	}

	/* put it on the pool of free buffers */
	spin_lock(&_stp_pool_lock);
	list_add_tail(&bptr->list, &_stp_pool_q);
	spin_unlock(&_stp_pool_lock);

	return len;
}



static struct file_operations _stp_proc_fops_cmd = {
	.read = _stp_proc_read_cmd,
	.write = _stp_proc_write_cmd,
//	.poll = _stp_proc_poll_cmd
};

static struct proc_dir_entry *_stp_proc_root, *_stp_proc_mod;

/* copy since proc_match is not MODULE_EXPORT'd */
static int my_proc_match(int len, const char *name, struct proc_dir_entry *de)
{
	if (de->namelen != len)
		return 0;
	return !memcmp(name, de->name, len);
}

/* set the number of buffers to use to 'num' */
static int _stp_set_buffers(int num)
{
	int i;
	struct list_head *p;
	
	printk("stp_set_buffers %d\n", num);

	if (num == 0 || num == _stp_current_buffers)
		return _stp_current_buffers;
	
	spin_lock(&_stp_pool_lock);
	if (num > _stp_current_buffers) {
		for (i = 0; i < num - _stp_current_buffers; i++) {
			p = (struct list_head *)vmalloc(sizeof(struct _stp_buffer));
			if (!p)	{
				_stp_current_buffers += i;
				goto err;
			}
			list_add (p, &_stp_pool_q);
		}
	} else {
		for (i = 0; i < _stp_current_buffers - num; i++) {
			p = _stp_pool_q.next;
			list_del(p);
			vfree(p);
		}
	}
	_stp_current_buffers = num;
err:
	spin_unlock(&_stp_pool_lock);
	return _stp_current_buffers;
}

static int _stp_register_procfs (void)
{
	int i, j;
	const char *dirname = "systemtap";
	char buf[8];
	struct proc_dir_entry *de;
	struct list_head *p, *tmp;

	INIT_LIST_HEAD(&_stp_ready_q);
	INIT_LIST_HEAD(&_stp_pool_q);

	/* allocate buffers */
	for (i = 0; i < STP_DEFAULT_BUFFERS; i++) {
		p = (struct list_head *)vmalloc(sizeof(struct _stp_buffer));
		// printk("allocated buffer at %lx\n", (long)p);
		if (!p)
			goto err2;
		list_add (p, &_stp_pool_q);
	}
	

	/* look for existing /proc/systemtap */
	for (de = proc_root.subdir; de; de = de->next) {
		if (my_proc_match (strlen (dirname), dirname, de)) {
			_stp_proc_root = de;
			break;
		}
	}

	/* create /proc/systemtap if it doesn't exist */
	if (_stp_proc_root == NULL) {
		_stp_proc_root = proc_mkdir (dirname, NULL);
		if (_stp_proc_root == NULL)
			goto err0;
	}

	/* now create /proc/systemtap/module_name */
	_stp_proc_mod = proc_mkdir (THIS_MODULE->name, _stp_proc_root);
	if (_stp_proc_mod == NULL)
		goto err0;

#ifdef STP_RELAYFS	
	/* now for each cpu "n", create /proc/systemtap/module_name/n  */
	for_each_cpu(i) {
		sprintf(buf, "%d", i);
		de = create_proc_entry (buf, S_IFREG|S_IRUSR, _stp_proc_mod);
		if (de == NULL) 
			goto err1;
		de->proc_fops = &_stp_proc_fops;
		de->data = kmalloc(sizeof(int), GFP_KERNEL);
		if (de->data == NULL) {
			remove_proc_entry (buf, _stp_proc_mod);
			goto err1;
		}
		*(int *)de->data = i;
	}
#endif

	/* finally create /proc/systemtap/module_name/cmd  */
	de = create_proc_entry ("cmd", S_IFREG|S_IRUSR, _stp_proc_mod);
	if (de == NULL) 
		goto err1;
	de->proc_fops = &_stp_proc_fops_cmd;
	return 0;

err2:
	list_for_each_safe(p, tmp, &_stp_pool_q) {
		list_del(p);
		vfree(p);
	}

err1:
#ifdef STP_RELAYFS
	for (de = _stp_proc_mod->subdir; de; de = de->next)
		kfree (de->data);
	for_each_cpu(j) {
		if (j == i)
			break;
		sprintf(buf, "%d", i);
		remove_proc_entry (buf, _stp_proc_mod);
		
	}
#endif
	remove_proc_entry (THIS_MODULE->name, _stp_proc_root);
err0:
	printk (KERN_ERR "Error creating systemtap /proc entries.\n");
	return -1;
}


static void _stp_unregister_procfs (void)
{
	int i;
	char buf[8];
	struct list_head *p, *tmp;
	struct proc_dir_entry *de;
#ifdef STP_RELAYFS
	for (de = _stp_proc_mod->subdir; de; de = de->next)
		kfree (de->data);

	for_each_cpu(i) {
		sprintf(buf, "%d", i);
		remove_proc_entry (buf, _stp_proc_mod);
	}
#endif
	remove_proc_entry ("cmd", _stp_proc_mod);
	remove_proc_entry (THIS_MODULE->name, _stp_proc_root);

	/* free memory pools */
	list_for_each_safe(p, tmp, &_stp_pool_q) {
		list_del(p);
		vfree(p);
	}
	list_for_each_safe(p, tmp, &_stp_ready_q) {
		list_del(p);
		vfree(p);
	}
}

