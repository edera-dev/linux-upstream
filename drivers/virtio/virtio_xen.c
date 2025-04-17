/*
 * Virtio Xenbus driver
 *
 * Front-end for a Xen grant-based virtio driver.
 *
 * TODO: Copyright? Ref to Liu Wei?
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/virtio.h>
#include <linux/virtio_fs.h>
#include <linux/virtio_config.h>
#include <linux/virtio_ring.h>

#include <xen/events.h>
#include <xen/grant_table.h>
#include <xen/xen.h>
#include <xen/xenbus.h>

MODULE_DESCRIPTION("virtio over xenbus");
MODULE_AUTHOR("Edera");
MODULE_LICENSE("GPL");

static const struct xenbus_device_id xen_virtio_ids[] = { { "xen-virtio" }, { "" } };

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

#define NOT_IMPL pr_crit("%s: not implemented\n", __func__)
#define TRACE(fmt, ...) pr_info("%s: " fmt "\n", __func__, ##__VA_ARGS__)

static irqreturn_t vx_interrupt(int irq, void *opaque)
{
	TRACE("enter");
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
	NOT_IMPL;
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

	vx_dev = kzalloc(sizeof(*vx_dev), GFP_KERNEL);
	if (!vx_dev)
		return -ENOMEM;

	vx_dev->vio_dev.dev.parent = &xb_dev->dev;
	vx_dev->vio_dev.dev.release = virtio_xenbus_release_dev;
	vx_dev->vio_dev.config = &virtio_xenbus_config_ops;

	if (0 != strncmp(xb_dev->devicetype, "virtio-fs", 9)) {
		ret = -ENODEV;
		goto err_vxdev;
	}

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

	ret = virtio_xenbus_connect_backend(xb_dev, vx_dev);
	if (ret < 0)
		goto err_conf;

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
static void xen_virtio_changed(struct xenbus_device *dev,
				   enum xenbus_state backend_state)
{
	switch (backend_state) {
	case XenbusStateReconfiguring:
	case XenbusStateReconfigured:
	case XenbusStateUnknown:
		break;

	case XenbusStateInitialising:
		break;

	case XenbusStateInitWait:
		/*  */
		break;

	case XenbusStateInitialised:
		break;

	case XenbusStateConnected:
		/* switch our state to connected? */
		break;

	case XenbusStateClosing:
	case XenbusStateClosed:
		break;
	}
}

static struct xenbus_driver front_driver = {
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
