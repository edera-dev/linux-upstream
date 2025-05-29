/*
 * Virtio Xenbus driver
 *
 * Front-end for a Xen grant-based virtio driver.
 *
 * TODO: Copyright? Ref to Liu Wei?
 */

#include <linux/dma-mapping.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/spinlock.h>
#include <linux/time.h>
#include <linux/timekeeping.h>
#include <linux/virtio.h>
#include <linux/virtio_config.h>
#include <linux/virtio_fs.h>
#include <linux/virtio_ids.h>
#include <linux/virtio_ring.h>

#include <xen/events.h>
#include <xen/grant_table.h>
#include <xen/xen.h>
#include <xen/xenbus.h>

MODULE_DESCRIPTION("virtio over xenbus");
MODULE_AUTHOR("Edera");
MODULE_LICENSE("GPL");

static const struct xenbus_device_id xen_virtio_ids[] = { { "virtio-fs" }, { "" } };

// NOTE: must match backend definition
struct virtio_config_page {
	u8 device[48];  /* our virtio_xenbus config message buffer */
	u8 driver[256]; /* driver-private config space */
	u32 offset;     /* offset into driver-private config space */
	u32 cmd_code;
	u32 write;
	u32 size;
	u32 be_active; /* backend sync toggle */
};

struct virtio_xenbus_vq_info {
	struct virtqueue *vq;
	int num_entries;

	dma_addr_t queue_dma;
	void *queue_va; // TODO: maybe not needed
	u64 queue_idx;

	struct list_head node;
};

struct virtio_xenbus_device {
	struct virtio_device vio_dev;
	struct xenbus_device *xb_dev;

	int conf_gntref;
	struct virtio_config_page *config_page;

	spinlock_t irq_lock;
	int conf_irq, notify_irq;

	evtchn_port_t conf_evtchn, notify_evtchn;

	spinlock_t vq_lock;
	struct list_head vq_list;

	char phys[32];
};

static struct virtio_xenbus_device *to_vx_device(struct virtio_device *vdev)
{
	return container_of(vdev, struct virtio_xenbus_device, vio_dev);
}

static void virtio_xenbus_release_dev(struct device *_d)
{
	struct virtio_device *dev =
		container_of(_d, struct virtio_device, dev);
	struct virtio_xenbus_device *vx_dev = to_vx_device(dev);

	kfree(vx_dev);
}

#define TRACE(fmt, ...) pr_info("%s: " fmt "\n", __func__, ##__VA_ARGS__)
#define NOT_IMPL TRACE("not implemented")

// upcall from backend
static irqreturn_t vx_interrupt(int irq, void *opaque)
{
	NOT_IMPL;
	return IRQ_HANDLED;

#if 0 // FIXME:
	struct virtio_xenbus_device *vx_dev = opaque;
	u8 isr;
	irqreturn_t ret;

	/* reading the ISR has the effect of also clearing it so it's very
	 * important to save the value. */
	isr = vx_read8(vx_dev, VIRTIO_XENBUS_ISR);

	/* It's definitely not us if the ISR was not high */
	if (!isr)
		return IRQ_NONE;

	/* Configuration change? Tell driver if it wants to know. */
	if (isr & VIRTIO_XENBUS_ISR_CONFIG)
		vx_config_changed(irq, opaque);

	ret = vx_vring_interrupt(irq, opaque);

	return ret;
#endif
}

static irqreturn_t vx_conf_handler(int irq, void *data)
{
	NOT_IMPL;
	return IRQ_HANDLED;
}

static int virtio_xenbus_connect_backend(struct xenbus_device *xb_dev,
					 struct virtio_xenbus_device *vx_dev)
{
	TRACE("enter");

	int ret;
	int evtchn;
	struct xenbus_transaction xbt;

	// TODO: clean this fn up

	// export configuration page to grant table

	ret = gnttab_grant_foreign_access(xb_dev->otherend_id,
					  virt_to_mfn(vx_dev->config_page),
					  0 /* RW */);
	if (ret < 0)
		return ret;
	vx_dev->conf_gntref = ret;
	TRACE("conf_gntref = %d", ret);

	// create notification channel for virtqueues
	// and bind to handler

	ret = xenbus_alloc_evtchn(xb_dev, &evtchn);
	if (ret)
		goto error_grant;
	vx_dev->notify_evtchn = evtchn;
	TRACE("notify_evtchn = %d", evtchn);

	ret = bind_evtchn_to_irqhandler(evtchn, vx_interrupt, 0,
					xb_dev->devicetype, vx_dev);
	if (ret < 0)
		goto error_notify_evtchn;
	vx_dev->notify_irq = ret;
	TRACE("notify_irq = %d", ret);

	// create notification channel for the config page
	// and bind to handler

	ret = xenbus_alloc_evtchn(xb_dev, &evtchn);
	if (ret)
		goto error_irqh;
	vx_dev->conf_evtchn = evtchn;
	TRACE("conf_evtchn = %d", evtchn);

	ret = bind_evtchn_to_irqhandler(evtchn, vx_conf_handler, 0,
					xb_dev->devicetype, vx_dev);
	if (ret < 0)
		goto error_conf_evtchn;
	vx_dev->conf_irq = ret;
	TRACE("conf_irq = %d", ret);

again:
	ret = xenbus_transaction_start(&xbt);
	if (ret)
		goto error_conf_irqh;
	ret = xenbus_printf(xbt, xb_dev->nodename, "conf-mfn", "%lu",
			    virt_to_mfn(vx_dev->config_page));
	if (ret)
		goto error_xenbus;
	ret = xenbus_printf(xbt, xb_dev->nodename, "conf-gntref", "%u",
			    vx_dev->conf_gntref);
	if (ret)
		goto error_xenbus;
	ret = xenbus_printf(xbt, xb_dev->nodename, "conf-evtchn", "%u",
			    vx_dev->conf_evtchn);
	if (ret)
		goto error_xenbus;
	ret = xenbus_printf(xbt, xb_dev->nodename, "notify-evtchn", "%u",
			    vx_dev->notify_evtchn);
	if (ret)
		goto error_xenbus;
	ret = xenbus_transaction_end(xbt, 0);
	if (ret) {
		if (ret == -EAGAIN)
			goto again;
		goto error_conf_irqh;
	}
	xenbus_switch_state(xb_dev, XenbusStateInitialised);

	return 0;

error_xenbus:
	xenbus_transaction_end(xbt, 1);
	xenbus_dev_fatal(xb_dev, ret, "writing xenstore");
error_conf_irqh:
	unbind_from_irqhandler(vx_dev->conf_irq, vx_dev);
	vx_dev->conf_irq = -1;
error_conf_evtchn:
	xenbus_free_evtchn(xb_dev, vx_dev->conf_evtchn);
	vx_dev->conf_evtchn = -1;
error_irqh:
	unbind_from_irqhandler(vx_dev->notify_irq, vx_dev);
	vx_dev->notify_irq = -1;
error_notify_evtchn:
	xenbus_free_evtchn(xb_dev, vx_dev->notify_evtchn);
	vx_dev->notify_evtchn = -1;
error_grant:
	gnttab_end_foreign_access_ref(vx_dev->conf_gntref);
	vx_dev->conf_gntref = -1;
	return ret;
}

static void virtio_xenbus_disconnect_backend(struct virtio_xenbus_device
					     *vx_dev)
{
	if (vx_dev->notify_irq >= 0)
		unbind_from_irqhandler(vx_dev->notify_irq, vx_dev);
	vx_dev->notify_irq = -1;

	if (vx_dev->conf_irq >= 0)
		unbind_from_irqhandler(vx_dev->conf_irq, vx_dev);
	vx_dev->conf_irq = -1;

	if (vx_dev->conf_gntref >= 0)
		gnttab_end_foreign_access_ref(vx_dev->conf_gntref);
	vx_dev->conf_gntref = -1;

	xenbus_free_evtchn(vx_dev->xb_dev, vx_dev->notify_evtchn);
	xenbus_free_evtchn(vx_dev->xb_dev, vx_dev->conf_evtchn);

	vx_dev->notify_evtchn = -1;
	vx_dev->conf_evtchn = -1;
}

// virtio config operations

/* A 64-bit r/o bitmask of the features supported by the host */
#define VIRTIO_XENBUS_HOST_FEATURES        0

/* A 64-bit r/w bitmask of features activated by the guest */
#define VIRTIO_XENBUS_GUEST_FEATURES       8

/* A 32-bit r/w PFN for the currently selected queue */
#define VIRTIO_XENBUS_QUEUE_PFN            16

/* A 16-bit r/o queue size for the currently selected queue */
#define VIRTIO_XENBUS_QUEUE_NUM            20

/* A 16-bit r/w queue selector */
#define VIRTIO_XENBUS_QUEUE_SEL            22

/* A 16-bit r/w queue notifier */
#define VIRTIO_XENBUS_QUEUE_NOTIFY         24

/* An 8-bit device status register.  */
#define VIRTIO_XENBUS_STATUS               26

/* An 8-bit r/o interrupt status register.  Reading the value will return the
 * current contents of the ISR and will also clear it.  This is effectively
 * a read-and-acknowledge. */
#define VIRTIO_XENBUS_ISR                  27

/* The bit of the ISR which indicates a device configuration change. */
#define VIRTIO_XENBUS_ISR_CONFIG           0x2

#define VIRTIO_XENBUS_CONFIG_OFF           28
#define VX_CMD_CONFIG VIRTIO_XENBUS_CONFIG_OFF

/* Virtio Xenbus ABI version, this must match exactly */
#define VIRTIO_XENBUS_ABI_VERSION          0

/* How many bits to shift physical queue address written to QUEUE_PFN.
 * 12 is historical, and due to x86 page size. */
#define VIRTIO_XENBUS_QUEUE_ADDR_SHIFT     12

/* The alignment to use between consumer and producer parts of vring.
 * x86 pagesize. */
#define VIRTIO_XENBUS_VRING_ALIGN          4096

/* read and write callback ops */

void __vx_wait(struct virtio_xenbus_device *vx_dev)
{
	struct virtio_config_page *page = vx_dev->config_page;
	unsigned irq = vx_dev->conf_irq;

	s64 ns, ns_timeout;

	unsigned long irq_flags;
	spin_lock_irqsave(&vx_dev->irq_lock, irq_flags);

	page->be_active = 1;

	mb();

	ns_timeout = ktime_get_real_ns() + (s64)NSEC_PER_SEC / 2;

	notify_remote_via_evtchn(vx_dev->conf_evtchn);
	xen_clear_irq_pending(irq);

	while (page->be_active) { // toggled by backend
		xen_poll_irq_timeout(irq, jiffies + 3 * HZ);
		xen_clear_irq_pending(irq);

		ns = ktime_get_real_ns();
		if (ns > ns_timeout) {
			dev_err(&vx_dev->xb_dev->dev,
				"no response from backend\n");
			page->be_active = 0;
			goto out;
		}
	}
out:
	mb();
	spin_unlock_irqrestore(&vx_dev->irq_lock, irq_flags);
}

#define EMIT_VX_READ(N) \
	u##N vx_read##N(struct virtio_xenbus_device *, u32); \
	u##N vx_read##N(struct virtio_xenbus_device *vx_dev, u32 cmd_code) { \
		struct virtio_config_page *conf = vx_dev->config_page; \
		conf->size = sizeof(u##N); \
		conf->cmd_code = cmd_code; \
		conf->write = 0; \
		__vx_wait(vx_dev); \
		return *((u##N *)conf->device); \
	}

EMIT_VX_READ(8)
EMIT_VX_READ(16)
EMIT_VX_READ(32)
EMIT_VX_READ(64)


/* For commands with arbitrary argument sizes. */
void vx_read(struct virtio_xenbus_device *vx_dev, u32 cmd_code, void *dst, u32 size) {
	struct virtio_config_page *conf = vx_dev->config_page;

	conf->size = size;
	conf->cmd_code = cmd_code;
	conf->write = 0;

	__vx_wait(vx_dev);

	memcpy(dst, conf->device, size);
}

#define EMIT_VX_WRITE(N) \
	void vx_write##N(struct virtio_xenbus_device *, u32, u##N); \
	void vx_write##N(struct virtio_xenbus_device *vx_dev, u32 cmd_code, u##N val) { \
		struct virtio_config_page *conf = vx_dev->config_page; \
		conf->size = sizeof(u##N); \
		conf->cmd_code = cmd_code; \
		conf->write = 1; \
		*((u##N *)conf->device) = val; \
		__vx_wait(vx_dev); \
	}

EMIT_VX_WRITE(8)
EMIT_VX_WRITE(16)
EMIT_VX_WRITE(32)
EMIT_VX_WRITE(64)

/* For commands with arbitrary argument sizes. */
void vx_write(struct virtio_xenbus_device *vx_dev, u32 cmd_code, void *src, u32 size) {
	struct virtio_config_page *conf = vx_dev->config_page;

	conf->size = size;
	conf->cmd_code = cmd_code;
	conf->write = 1;

	memcpy(conf->device, src, size);

	__vx_wait(vx_dev);
}

/* virtqueue helper routines */

static bool vx_notify(struct virtqueue *vq)
{
	struct virtio_xenbus_device *vx_dev = to_vx_device(vq->vdev);
	struct virtio_xenbus_vq_info *info = vq->priv;
	TRACE("");

	// NOTE: we use the conf evtchn for virtqueue notifications
	vx_write16(vx_dev, VIRTIO_XENBUS_QUEUE_NOTIFY, info->queue_idx);

	return true;
}

struct pfn_desc { u64 desc; u64 avail; u64 used; };
typedef struct pfn_desc pfn_desc;

static int vx_read_vq_conf(struct virtio_xenbus_device *vx_dev,
			   struct virtio_xenbus_vq_info *info,
			   unsigned index)
{
	TRACE("enter");

	vx_write16(vx_dev, VIRTIO_XENBUS_QUEUE_SEL, index);

	u16 num = vx_read16(vx_dev, VIRTIO_XENBUS_QUEUE_NUM);

	if (num == 0) {
		pr_err("queue is unavailable");
		return -ENOENT; /* not available */
	}

	pfn_desc desc;
	vx_read(vx_dev, VIRTIO_XENBUS_QUEUE_PFN, &desc, sizeof(desc));
	if (desc.desc | desc.avail | desc.used) {
		pr_err("queue is already activated desc %#lx avail %#lx used %#lx",
			desc.desc, desc.avail, desc.used);
		return -ENOENT; /* already activated */
	}

	info->queue_idx = index;
	info->num_entries = num;

	TRACE("exit");
	return 0;
}

// FIXME: what is ctx indicating? do we have to act on it?
static struct virtqueue *vx_setup_vq(struct virtio_device *vd, unsigned index,
				  void (*callback)(struct virtqueue *vq),
				  const char *name, bool ctx)
{
	struct virtio_xenbus_device *vx_dev = to_vx_device(vd);
	struct virtio_xenbus_vq_info *info;
	struct virtqueue *vq;
	unsigned long flags;
	//unsigned long size;
	int err;

	TRACE("enter");

	info = kmalloc(sizeof(struct virtio_xenbus_vq_info), GFP_KERNEL);
	if (!info)
		return ERR_PTR(-ENOMEM);

	err = vx_read_vq_conf(vx_dev, info, index);
	if (err < 0)
		goto out_info;
	TRACE("conf ok");

	/* NOTE: (lw) the shared queue MUST be machine contiguous */
	//size = PAGE_ALIGN(vring_size(info->num_entries, VIRTIO_XENBUS_VRING_ALIGN));
	//info->queue_va = dma_alloc_coherent(NULL, size,
	//				    &info->queue_dma,
	//				    GFP_KERNEL|__GFP_ZERO);
	//if (info->queue_va == NULL) {
	//	err = -ENOMEM;
	//	goto out_info;
	//}

	/*
	 * NOTE: 2a2d1382fe9dccfc introduced a new API: vring_create_virtqueue
	 * The API vring_new_virtqueue is older and left for compatibility.

	 *      has dma quirk = !VIRTIO_F_ACCESS_PLATFORM
	 *  vring use dma api = !has_quirk || xen_domain
	 *
	 * dma_alloc_coherent = vring use dma api
	 *                    = !has_quirk || xen_domain
	 *                    = VIRTIO_F_ACCESS_PLATFORM || xen_domain
	 *
	 * Seems we do not need to fiddle with this? As long as it is
	 * a Xen domain, then we should be using the DMA API.
	 *
	 * NOTE: if we use vring_create_virtqueue then we need the PFN of the
	 * underlying DMA buffer to share with the backend...
	 */
	bool weak_barriers = true;
	bool may_reduce = false;
	TRACE("-> vring_create_virtqueue");
	vq = vring_create_virtqueue(index, info->num_entries,
				    VIRTIO_XENBUS_VRING_ALIGN,
				    vd, weak_barriers, may_reduce,
				    ctx, vx_notify, callback, name);
	if (!vq) {
		err = -ENOMEM;
		goto out_info;
	}

	vq->priv = info;
	info->vq = vq;

	spin_lock_irqsave(&vx_dev->vq_lock, flags);
	list_add(&info->node, &vx_dev->vq_list);
	spin_unlock_irqrestore(&vx_dev->vq_lock, flags);

	pfn_desc desc;
	desc.desc = virtqueue_get_desc_addr(vq);
	desc.avail = virtqueue_get_avail_addr(vq);
	desc.used = virtqueue_get_used_addr(vq);
	vx_write(vx_dev, VIRTIO_XENBUS_QUEUE_PFN,
		 &desc, sizeof(desc));

	TRACE("exit");
	return vq;

	//if (vq)
	//	vring_del_virtqueue(vq);
	// vx_write32(vx_dev, 0, VIRTIO_XENBUS_QUEUE_PFN);
	//dma_free_coherent(NULL, size,
	//		  info->queue_va, info->queue_dma);
out_info:
	TRACE("exit (error)");
	kfree(info);
	return ERR_PTR(err);
}

static void vx_del_vq(struct virtqueue *vq)
{
	struct virtio_xenbus_device *vxb = to_vx_device(vq->vdev);
	struct virtio_xenbus_vq_info *info = vq->priv;
	unsigned long flags, size;
	TRACE("");

	spin_lock_irqsave(&vxb->vq_lock, flags);
	list_del(&info->node);
	spin_unlock_irqrestore(&vxb->vq_lock, flags);

	vx_write16(vxb, VIRTIO_XENBUS_QUEUE_SEL, info->queue_idx);

	vring_del_virtqueue(vq);

	/* Select and deactivate the queue */
	vx_write32(vxb, VIRTIO_XENBUS_QUEUE_PFN, 0);

	kfree(info);
}

static void vx_del_vqs(struct virtio_device *vd)
{
	struct virtqueue *vq, *n;
	TRACE("");

	list_for_each_entry_safe(vq, n, &vd->vqs, list) {
		vx_del_vq(vq);
	}
}

/* virtio callback fns invoked from our frontend */

static u8 vx_get_status(struct virtio_device *vdev)
{
	u8 ret = vx_read8(to_vx_device(vdev), VIRTIO_XENBUS_STATUS);
	TRACE("%#x", ret);
	return (u8)ret;
}

static void vx_set_status(struct virtio_device *vdev, u8 status)
{
	TRACE("%#x", status);
	vx_write8(to_vx_device(vdev), VIRTIO_XENBUS_STATUS, status);
}

// NOTE: this is the first fn invoked by the virtio subsystem
static void vx_reset(struct virtio_device *vdev)
{
	TRACE("");
	vx_set_status(vdev, 0);
}

static void vx_get_config(struct virtio_device *vdev, unsigned offset,
			  void *buf, unsigned len)
{
	struct virtio_config_page *conf = to_vx_device(vdev)->config_page;

	conf->cmd_code = VX_CMD_CONFIG;
	conf->write = 0;
	conf->size = len;
	conf->offset = offset;

	__vx_wait(to_vx_device(vdev));

	memcpy(buf, conf->driver + offset, len);
}

static void vx_set_config(struct virtio_device *vdev, unsigned offset,
			  const void *buf, unsigned len)
{
	struct virtio_config_page *conf = to_vx_device(vdev)->config_page;

	conf->cmd_code = VX_CMD_CONFIG;
	conf->write = 1;
	conf->size = len;
	conf->offset = offset;

	memcpy(conf->driver + offset, buf, len);

	__vx_wait(to_vx_device(vdev));
}

static int vx_find_vqs(struct virtio_device *vd, unsigned int nvqs,
		       struct virtqueue *vqs[],
		       struct virtqueue_info vqs_info[],
		       struct irq_affinity *desc)
{
	int err;
	int i, queue_idx = 0;

	TRACE("nvqs %u", nvqs);

	// TODO: finish here, model after ccw

	for (i = 0; i < nvqs; ++i) {
		struct virtqueue_info *vqi = &vqs_info[i];

		if (!vqi->name) {
			vqs[i] = NULL;
			continue;
		}

		vqs[i] = vx_setup_vq(vd, queue_idx++, vqi->callback,
				     vqi->name, vqi->ctx);
		if (IS_ERR(vqs[i])) {
			err = PTR_ERR(vqs[i]);
			goto error_find;
		}
	}

	return 0;

error_find:
	vx_del_vqs(vd);
	return err;
}

static u64 vx_get_features(struct virtio_device *vdev)
{
	u64 ret = vx_read64(to_vx_device(vdev), VIRTIO_XENBUS_HOST_FEATURES);
	TRACE("%#llx", ret);
	return ret;
}

static int vx_finalize_features(struct virtio_device *vdev)
{
	vring_transport_features(vdev);
	TRACE("%#llx", vdev->features);
	vx_write64(to_vx_device(vdev), VIRTIO_XENBUS_GUEST_FEATURES, vdev->features);
	return 0;
}

static struct virtio_config_ops virtio_xenbus_config_ops = {
	.get		= vx_get_config,
	.set		= vx_set_config,
	.get_status	= vx_get_status,
	.set_status	= vx_set_status,
	.reset		= vx_reset,
	.find_vqs	= vx_find_vqs,
	.del_vqs	= vx_del_vqs,
	.get_features	= vx_get_features,
	.finalize_features = vx_finalize_features,
};

/*
 * Entry when a new device is created. Allocate basic structures, ring buffers,
 * and inform backend of their details.
 */
static int xen_virtio_probe(struct xenbus_device *xb_dev,
		     const struct xenbus_device_id *id)
{
	int ret;
	struct virtio_xenbus_device *vx_dev;

	TRACE("enter");

	vx_dev = kzalloc(sizeof(*vx_dev), GFP_KERNEL);
	if (!vx_dev)
		return -ENOMEM;
	TRACE("kzalloc ok");

	vx_dev->vio_dev.dev.parent = &xb_dev->dev;
	vx_dev->vio_dev.dev.release = virtio_xenbus_release_dev;
	vx_dev->vio_dev.config = &virtio_xenbus_config_ops;

	if (0 != strncmp(xb_dev->devicetype, "virtio-fs", 9)) {
		TRACE("error: devicetype: %s", xb_dev->devicetype);
		ret = -ENODEV;
		goto err_vxdev;
	}
	TRACE("devicetype virtio-fs");

	vx_dev->vio_dev.id.vendor = VIRTIO_DEV_ANY_ID;
	vx_dev->vio_dev.id.device = VIRTIO_ID_FS;

	dev_set_drvdata(&xb_dev->dev, vx_dev);
	vx_dev->xb_dev = xb_dev;
	vx_dev->notify_irq = -1;
	vx_dev->conf_irq = -1;
	vx_dev->notify_evtchn = -1;
	vx_dev->conf_evtchn = -1;
	vx_dev->conf_gntref = -1;
	snprintf(vx_dev->phys, sizeof(vx_dev->phys), "xenbus/%s",
		 xb_dev->nodename);

	INIT_LIST_HEAD(&vx_dev->vq_list);
	spin_lock_init(&vx_dev->vq_lock);

	spin_lock_init(&vx_dev->irq_lock);

	vx_dev->config_page = (void *)__get_free_page(GFP_KERNEL | __GFP_ZERO);
	if (!vx_dev->config_page) {
		ret = -ENOMEM;
		goto err_drvdata;
	}
	TRACE("alloc config_page");

	// FIXME: unsure if this is required
	// Guest throws a warning in dma_alloc_attrs for
	//		WARN_ON_ONCE(flag & __GFP_COMP)
	// Tried enabling the below. Need to investigate further.
	ret = dma_set_mask_and_coherent(&xb_dev->dev, DMA_BIT_MASK(64));
	if (ret)
		ret = dma_set_mask_and_coherent(&xb_dev->dev, DMA_BIT_MASK(32));
	if (ret)
		dev_warn(&xb_dev->dev, "Failed to enable 64-bit or 32-bit DMA. Trying to continue, but this might not work.\n");

	ret = virtio_xenbus_connect_backend(xb_dev, vx_dev);
	if (ret < 0)
		goto err_conf;

	TRACE("return ok");
	return 0;

err_conf:
	free_page((unsigned long)vx_dev->config_page);
err_drvdata:
	dev_set_drvdata(&xb_dev->dev, NULL);
err_vxdev:
	kfree(vx_dev);
	return ret;
}

/* device is removed from backend */
static void xen_virtio_remove(struct xenbus_device *dev)
{
	NOT_IMPL;
}

/* backend driver state has changed, we are notified
 * xenbus_switch_state(dev, XenbusStateConnected); used to update
 * our state in the frontend in response to changes in the backend.
 *
 * 1. XenbusStateUnknown
 *	initial state
 * 2. XenbusStateInitialising
 *	backend is initialising
 * 3. XenbusStateInitWait
 *	backend is initialised but needs more info before can connect
 *	init state of backend waiting for info
 *	hot-plug info or something from guest
 * 4. XenbusStateInitialised
 *	backend ready for connections
 * 5. XenbusStateConnected
 *	normal state of the bus, both ends communicating normally
 * 6. XenbusStateClosing
 *	device has become unavailable; still connected
 *	backend no longer responding to front
 *	front should begin shutdown
 * 7. XenbusStateClosed
 *	two halves have disconnected
 */
static void xen_virtio_changed(struct xenbus_device *xb_dev,
				   enum xenbus_state backend_state)
{
	int err;

	// FIXME: figure out correct sequence for state change

	pr_info("xen-virtio: be=%u\n", backend_state);
	struct virtio_xenbus_device *vxb_dev = dev_get_drvdata(&xb_dev->dev);

	switch (backend_state) {
	case XenbusStateReconfiguring:
	case XenbusStateReconfigured:
	case XenbusStateUnknown:
		break;

	case XenbusStateInitialising:
		break;

	case XenbusStateInitWait:
		pr_info("xen-virtio: -> fe=%u\n", XenbusStateConnected);
		//xenbus_switch_state(xb_dev, XenbusStateInitialised);
		break;

	case XenbusStateInitialised:
		break;

	case XenbusStateConnected:
		/* switch our state to connected? */
		pr_info("xen-virtio: -> fe=%u (registering virtio)\n", XenbusStateConnected);
		err = register_virtio_device(&vxb_dev->vio_dev);
		if (err != 0)
			pr_err("register_virtio_device returned %d", err);
		xenbus_switch_state(xb_dev, XenbusStateConnected);
		break;

	case XenbusStateClosing:
		pr_info("xen-virtio: -> fe=%u (disconnecting virtio)\n", XenbusStateClosed);
		xenbus_switch_state(xb_dev, XenbusStateClosing);
		virtio_xenbus_disconnect_backend(vxb_dev);
		unregister_virtio_device(&vxb_dev->vio_dev);
		break;

	case XenbusStateClosed:
		pr_info("xen-virtio: -> fe=%u\n", XenbusStateClosed);
		xenbus_switch_state(xb_dev, XenbusStateClosed);
		break;
	}
}

static struct xenbus_driver front_driver = {
	//.name = "virtio-xenbus-front", // TODO: what to do with this
	.ids = xen_virtio_ids,
	.probe = xen_virtio_probe,
	.remove = xen_virtio_remove,
	.otherend_changed = xen_virtio_changed,
};

static int __init xen_virtio_init(void)
{
	if (!xen_domain())
		return -ENODEV;

	pr_info("xen-virtio: init\n");
	return xenbus_register_frontend(&front_driver);
}

static void __exit xen_virtio_exit(void)
{
	pr_info("xen-virtio: exit\n");

	return xenbus_unregister_driver(&front_driver);
}

module_init(xen_virtio_init);
module_exit(xen_virtio_exit);
