// Copyright 2022-2023 TII (SSRC) and the Ghaf contributors
// SPDX-License-Identifier: Apache-2.0
/*
 * virtio_pmem.c: Virtio pmem char Driver
 *
 * Discovers persistent memory range information
 * from host and registers the virtual pmem device
 * with libnvdimm core.
 */
#include <linux/major.h>
#include <linux/ioport.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include "virtio_pmem_char.h"

static struct virtio_device_id id_table[] = {
	{ VIRTIO_ID_PMEM, VIRTIO_DEV_ANY_ID },
	{ 0 },
};

static struct virtio_pmem *vpmem = NULL;
static struct miscdevice pmem_miscdev;

/* Initialize virt queue */
static int init_vq(struct virtio_pmem *vpmem)
{
	/* single vq */
	vpmem->req_vq = virtio_find_single_vq(vpmem->vdev, virtio_pmem_host_ack,
					      "flush_queue");
	if (IS_ERR(vpmem->req_vq))
		return PTR_ERR(vpmem->req_vq);

	spin_lock_init(&vpmem->pmem_lock);
	INIT_LIST_HEAD(&vpmem->req_list);

	return 0;
};

static loff_t pmem_lseek(struct file *filp, loff_t off, int whence)
{
	loff_t newpos;

	switch (whence) {
	case SEEK_SET:
		if (off >= vpmem->size) {
			newpos = -ESPIPE;
			goto out;
		}
		newpos = off;
		break;

	case SEEK_CUR:
		newpos = filp->f_pos + off;
		if (newpos >= vpmem->size) {
			newpos = -ESPIPE;
			goto out;
		}
		break;

	case SEEK_END:
		newpos = vpmem->size;
		break;

	default: /* can't happen */
		newpos = -EINVAL;
		goto out;
	}

	filp->f_pos = newpos;
out:
	return newpos;
}

static ssize_t pmem_write(struct file *file, const char __user *buf,
			  size_t count, loff_t *ppos)
{
	loff_t pos = *ppos;
	void *pmem_addr, *temp_buf = NULL;
	unsigned long long map_start_addr;

	if (count > vpmem->size - pos)
		count = vpmem->size - pos;
	if (!count)
		return 0;

	map_start_addr = vpmem->start + pos;
	pmem_addr = ioremap(map_start_addr, count);
	if (!pmem_addr)
		return -ENOMEM;
	temp_buf = kmalloc(count, GFP_USER);
	if (!temp_buf)
		count = -ENOMEM;
		goto out;

	if (copy_from_user(temp_buf, buf, count)) {
		count = -EFAULT;
		goto out;
	}
	memcpy_toio(pmem_addr, temp_buf, count);

	*ppos += count;
out:
	iounmap(pmem_addr);
	if (temp_buf)
		kfree(temp_buf);

	return count;
}

static ssize_t pmem_read(struct file *file, char __user *buf, size_t count,
			 loff_t *ppos)
{
	loff_t pos = *ppos;
	void *pmem_addr, *temp_buf = NULL;
	unsigned long long map_start_addr;

	if (count > vpmem->size - pos)
		count = vpmem->size - pos;
	if (!count)
		return 0;

	map_start_addr = vpmem->start + pos;
	pmem_addr = ioremap(map_start_addr, count);
	if (!pmem_addr) {
		return -ENOMEM;
	}
	temp_buf = kmalloc(count, GFP_USER);
	if (!temp_buf)
	{
		count = -ENOMEM;
		goto out;
	}

	memcpy_fromio(temp_buf, pmem_addr, count);

	if (copy_to_user(buf, temp_buf, count)) {
		count = -EFAULT;
		goto out;
	}
	*ppos += count;

out:
	iounmap(pmem_addr);
	if (temp_buf)
		kfree(temp_buf);

	return count;
}

static int pmem_mmap(struct file *file, struct vm_area_struct *vma)
{
	if (vm_iomap_memory(vma, vpmem->start, vpmem->size) < 0)
		return -EIO;

	vma->vm_flags = VM_IO | VM_PFNMAP | VM_DONTEXPAND | VM_DONTDUMP |
			VM_MIXEDMAP | VM_READ | VM_WRITE;

	return 0;
}

static const struct file_operations __maybe_unused pmem_fops = {
	.llseek = 	pmem_lseek,
	.read 	= 	pmem_read,
	.write 	= 	pmem_write,
	.mmap 	= 	pmem_mmap,
};

static int virtio_pmem_probe(struct virtio_device *vdev)
{
	struct resource *req;
	int err = 0;

	if (!vdev->config->get) {
		dev_err(&vdev->dev, "%s failure: config access disabled\n",
			__func__);
		return -EINVAL;
	}

	vpmem = devm_kzalloc(&vdev->dev, sizeof(*vpmem), GFP_KERNEL);
	if (!vpmem) {
		err = -ENOMEM;
		goto out_err;
	}
	vpmem->vdev = vdev;
	vdev->priv = vpmem;
	err = init_vq(vpmem);
	if (err) {
		dev_err(&vdev->dev, "failed to initialize virtio pmem vq's\n");
		goto out_err;
	}

	virtio_cread_le(vpmem->vdev, struct virtio_pmem_config, start,
			&vpmem->start);
	virtio_cread_le(vpmem->vdev, struct virtio_pmem_config, size,
			&vpmem->size);

	req = devm_request_mem_region(&vdev->dev, vpmem->start, vpmem->size,
				      dev_name(&vdev->dev));
	if (!req) {
		dev_warn(&vdev->dev, "could not reserve region\n");
	} else {
		dev_info(&vdev->dev, "reserved region %pR\n", req);
	}

	return misc_register(&pmem_miscdev);

out_err:
	return err;
}

static void virtio_pmem_remove(struct virtio_device *vdev)
{
	vdev->config->del_vqs(vdev);
	virtio_reset_device(vdev);
	devm_release_mem_region(&vdev->dev, vpmem->start, vpmem->size);
	misc_deregister(&pmem_miscdev);
}

static struct virtio_driver virtio_pmem_driver = {
	.driver.name 	= KBUILD_MODNAME,
	.driver.owner 	= THIS_MODULE,
	.id_table 		= id_table,
	.probe 			= virtio_pmem_probe,
	.remove 		= virtio_pmem_remove,
};

static struct miscdevice pmem_miscdev = {
	.minor 	= MISC_DYNAMIC_MINOR,
	.name 	= "pmem_char",
	.fops 	= &pmem_fops,
};

module_virtio_driver(virtio_pmem_driver);
MODULE_DEVICE_TABLE(virtio, id_table);
MODULE_DESCRIPTION("Virtio pmem char driver");
MODULE_LICENSE("GPL");
