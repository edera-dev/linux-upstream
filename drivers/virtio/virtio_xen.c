/*
 * Virtio Xenbus driver
 *
 * Front-end for a Xen grant-based virtio driver.
 *
 * TODO: Copyright? Ref to Liu Wei?
 */

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

struct virtio_config_page {
	u8  config[256];
	int write;
	int size;
	int offset;
	int be_active; /* backend is active */
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
	struct list_head virtq;

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
	isr = vxread8(vx_dev, VIRTIO_XENBUS_ISR);

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
	TRACE("enter");
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

	ret = gnttab_grant_foreign_access(xb_dev->otherend_id,
					  virt_to_mfn(vx_dev->config_page),
					  0 /* W */);
	if (ret < 0)
		return ret;
	vx_dev->conf_gntref = ret;
	TRACE("conf_gntref = %d", ret);

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

/* A 32-bit r/o bitmask of the features supported by the host */
#define VIRTIO_XENBUS_HOST_FEATURES        0

/* A 32-bit r/w bitmask of features activated by the guest */
#define VIRTIO_XENBUS_GUEST_FEATURES       4

/* A 32-bit r/w PFN for the currently selected queue */
#define VIRTIO_XENBUS_QUEUE_PFN            8

/* A 16-bit r/o queue size for the currently selected queue */
#define VIRTIO_XENBUS_QUEUE_NUM            12

/* A 16-bit r/w queue selector */
#define VIRTIO_XENBUS_QUEUE_SEL            14

/* A 16-bit r/w queue notifier */
#define VIRTIO_XENBUS_QUEUE_NOTIFY         16

/* An 8-bit device status register.  */
#define VIRTIO_XENBUS_STATUS               18

/* An 8-bit r/o interrupt status register.  Reading the value will return the
 * current contents of the ISR and will also clear it.  This is effectively
 * a read-and-acknowledge. */
#define VIRTIO_XENBUS_ISR                  19

/* The bit of the ISR which indicates a device configuration change. */
#define VIRTIO_XENBUS_ISR_CONFIG           0x2

/* The remaining space is defined by each driver as the per-driver
 * configuration space */
#define VIRTIO_XENBUS_CONFIG(dev)          20

/* Virtio Xenbus ABI version, this must match exactly */
#define VIRTIO_XENBUS_ABI_VERSION          0

/* How many bits to shift physical queue address written to QUEUE_PFN.
 * 12 is historical, and due to x86 page size. */
#define VIRTIO_XENBUS_QUEUE_ADDR_SHIFT     12

/* The alignment to use between consumer and producer parts of vring.
 * x86 pagesize. */
#define VIRTIO_XENBUS_VRING_ALIGN          4096

void __vx_wait(struct virtio_xenbus_device *);
void vx_write8(struct virtio_xenbus_device *, int, int);

void __vx_wait(struct virtio_xenbus_device *vx_dev)
{
	evtchn_port_t evtchn = vx_dev->conf_evtchn;
	unsigned irq = vx_dev->conf_irq;

	struct virtio_config_page *page = vx_dev->config_page;

	// s64 ns, ns_timeout;
	u64 ns, ns_timeout;

	unsigned long irq_flags;
	spin_lock_irqsave(&vx_dev->irq_lock, irq_flags);
	page->be_active = 1;

	mb();

	ns_timeout = ktime_get_real_ns() + 2 * (s64)NSEC_PER_SEC;

	notify_remote_via_evtchn(evtchn);
	xen_clear_irq_pending(irq);

	while (page->be_active) {
		xen_poll_irq_timeout(irq, jiffies + 3 * HZ);
		xen_clear_irq_pending(irq);

		ns = ktime_get_real_ns();
		if (ns > ns_timeout) {
			dev_err(&vx_dev->xb_dev->dev,
				"__vx_wait: virtio back not responding!!!\n");
			page->be_active = 0;
			goto out;
		}
	}
out:
	mb();
	spin_unlock_irqrestore(&vx_dev->irq_lock, irq_flags);
}

void vx_write8(struct virtio_xenbus_device *vx_dev, int value, int offset)
{
	vx_dev->config_page->write = 1;
	vx_dev->config_page->offset = offset;
	vx_dev->config_page->size = 1;

	void *addr = &vx_dev->config_page->config[0] + offset;
	writeb(value, addr);

	/* We have wmb() in __vx_wait, no need for another one here. */
	__vx_wait(vx_dev);
}

static void vx_get(struct virtio_device *vdev, unsigned offset,
		   void *buf, unsigned len)
{
	NOT_IMPL;
}

static void vx_set(struct virtio_device *vdev, unsigned offset,
		   const void *buf, unsigned len)
{
	NOT_IMPL;
}

static u8 vx_get_status(struct virtio_device *vdev)
{
	NOT_IMPL;
	return 0xff;
}

static void vx_set_status(struct virtio_device *vdev, u8 status)
{
	NOT_IMPL;
}

static void vx_reset(struct virtio_device *vdev)
{
	struct virtio_xenbus_device *vx_dev = to_vx_device(vdev);
	vx_write8(vx_dev, 0, VIRTIO_XENBUS_STATUS);
}

static int vx_find_vqs(struct virtio_device *vdev, unsigned int nvqs,
		       struct virtqueue *vqs[],
		       struct virtqueue_info vqs_info[],
		       struct irq_affinity *desc)
{
	NOT_IMPL;
	return -ENODEV;
}

static void vx_del_vqs(struct virtio_device *vdev)
{
	NOT_IMPL;
}

static u64 vx_get_features(struct virtio_device *vdev)
{
	NOT_IMPL;
	return 0u;
}

static int vx_finalize_features(struct virtio_device *vdev)
{
	NOT_IMPL;
	return -ENODEV;
}

static struct virtio_config_ops virtio_xenbus_config_ops = {
	.get		= vx_get,
	.set		= vx_set,
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

	INIT_LIST_HEAD(&vx_dev->virtq);
	spin_lock_init(&vx_dev->vq_lock);

	spin_lock_init(&vx_dev->irq_lock);

	vx_dev->config_page = (void *)__get_free_page(GFP_KERNEL | __GFP_ZERO);
	if (!vx_dev->config_page) {
		ret = -ENOMEM;
		goto err_drvdata;
	}
	TRACE("alloc config_page");

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
