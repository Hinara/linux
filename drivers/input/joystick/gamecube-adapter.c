// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *  Copyright (c) 2024 Milas Robin
 *
 *  Based on the work of:
 *	Michael Lelli
 *	Dolphin Emulator project
 */

#include <linux/usb.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/input.h>
#include <linux/usb/input.h>

/* Did not use usb-hid as it is not an hid driver */
#define USB_VENDOR_ID_NINTENDO		0x057e
#define USB_DEVICE_ID_NINTENDO_GCADAPTER 0x0337

#define EP_IN  0x81
#define EP_OUT 0x02

#define GCC_OUT_PKT_LEN 5
#define GCC_IN_PKT_LEN 37

enum gamecube_status {
	GAMECUBE_NONE,
	GAMECUBE_WIRED = 0x10,
	GAMECUBE_WIRELESS = 0x20,
};

struct gcc_data {
	struct ngc_data *adapter;
	struct input_dev *input;
	u8 no;
	u8 status;
	bool enable;
};

struct ngc_data {
	char phys[64];

	struct usb_device *udev;
	struct usb_interface *intf;

	struct urb *irq_in;
	u8 *idata;
	dma_addr_t idata_dma;
	spinlock_t idata_lock;

	struct urb *irq_out;
	struct usb_anchor irq_out_anchor;
	u8 *odata;
	dma_addr_t odata_dma;
	spinlock_t odata_lock;		/* output data */
	bool irq_out_active;		/* we must not use an active URB */
#ifdef CONFIG_JOYSTICK_NGC_FF
	u8 odata_rumbles[4];
	bool rumble_changed;		/* if rumble need update*/
#endif

	struct gcc_data controllers[4];
	struct work_struct work;	/* create/delete controller input files */
};

#ifdef CONFIG_JOYSTICK_NGC_FF
/* Callers must hold gdata->odata_lock spinlock */
static int ngc_queue_rumble(struct ngc_data *gdata)
{
	int error;

	memcpy(gdata->odata + 1, gdata->odata_rumbles,
			 sizeof(gdata->odata_rumbles));
	gdata->irq_out_active = true;
	gdata->rumble_changed = false;
	gdata->odata[0] = 0x11;
	gdata->irq_out->transfer_buffer_length = 5;

	usb_anchor_urb(gdata->irq_out, &gdata->irq_out_anchor);
	error = usb_submit_urb(gdata->irq_out, GFP_ATOMIC);
	if (error) {
		dev_err(&gdata->intf->dev,
			"%s - usb_submit_urb failed with result %d\n",
			__func__, error);
		usb_unanchor_urb(gdata->irq_out);
		error = -EIO;
	}
	return error;
}

static int ngc_set_rumble_value(struct ngc_data *gdata, u8 controller, u8 value)
{
	unsigned long flags;
	int error;

	value = !!value;
	if (controller > 4)
		return -EINVAL;

	spin_lock_irqsave(&gdata->odata_lock, flags);
	if (gdata->odata_rumbles[controller] == value) {
		spin_unlock_irqrestore(&gdata->odata_lock, flags);
		return 0;
	}
	gdata->odata_rumbles[controller] = value;
	gdata->rumble_changed = true;
	if (!gdata->irq_out_active)
		error = ngc_queue_rumble(gdata);
	spin_unlock_irqrestore(&gdata->odata_lock, flags);
	return error;
}

static int ngc_rumble_play(struct input_dev *dev, void *data,
			      struct ff_effect *eff)
{
	struct gcc_data *gccdata = input_get_drvdata(dev);
	u8 value;

	/*
	 * The gamecube controller  supports only a single rumble motor so if any
	 * magnitude is set to non-zero then we start the rumble motor. If both are
	 * set to zero, we stop the rumble motor.
	 */

	if (eff->u.rumble.strong_magnitude || eff->u.rumble.weak_magnitude)
		value = 1;
	else
		value = 0;
	return ngc_set_rumble_value(gccdata->adapter, gccdata->no, value);
}
static int ngc_init_ff(struct gcc_data *gccdev)
{
	input_set_capability(gccdev->input, EV_FF, FF_RUMBLE);

	return input_ff_create_memless(gccdev->input, NULL, ngc_rumble_play);
}
#else
static int ngc_init_ff(struct gcc_data *gccdev) { return 0; }
#endif

static void ngc_irq_out(struct urb *urb)
{
	struct ngc_data *gdata = urb->context;
	struct device *dev = &gdata->intf->dev;
	int status = urb->status;
	unsigned long flags;

	spin_lock_irqsave(&gdata->odata_lock, flags);

	switch (status) {
	case 0:
		/* success */
#ifdef CONFIG_JOYSTICK_NGC_FF
		gdata->irq_out_active = gdata->rumble_changed;
#else
		gdata->irq_out_active = false;
#endif
		break;
	case -ECONNRESET:
	case -ENOENT:
	case -ESHUTDOWN:
		/* this urb is terminated, clean up */
		dev_dbg(dev, "%s - urb shutting down with status: %d\n",
			__func__, status);
		gdata->irq_out_active = false;
		break;

	default:
		dev_dbg(dev, "%s - nonzero urb status received: %d\n",
			__func__, status);
		break;
	}
#ifdef CONFIG_JOYSTICK_NGC_FF
	if (gdata->irq_out_active)
		ngc_queue_rumble(gdata);
#endif
	spin_unlock_irqrestore(&gdata->odata_lock, flags);
}

static int ngc_init_output(struct ngc_data *gdata,
			 struct usb_endpoint_descriptor *irq)
{
	int error = -ENOMEM;

	init_usb_anchor(&gdata->irq_out_anchor);

	gdata->odata = usb_alloc_coherent(gdata->udev, GCC_OUT_PKT_LEN, GFP_KERNEL,
			 &gdata->odata_dma);
	if (!gdata->odata)
		return error;

	spin_lock_init(&gdata->odata_lock);

	gdata->irq_out = usb_alloc_urb(0, GFP_KERNEL);

	if (!gdata->irq_out)
		goto err_free_coherent;

	usb_fill_int_urb(gdata->irq_out, gdata->udev,
			 usb_sndintpipe(gdata->udev, irq->bEndpointAddress),
			 gdata->odata, GCC_OUT_PKT_LEN, ngc_irq_out, gdata,
			 irq->bInterval);
	gdata->irq_out->transfer_dma = gdata->odata_dma;
	gdata->irq_out->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;
	return 0;

err_free_coherent:
	usb_free_coherent(gdata->udev, GCC_OUT_PKT_LEN, gdata->odata,
			 gdata->odata_dma);
	return error;
}

static void ngc_deinit_output(struct ngc_data *gdata)
{
	usb_free_urb(gdata->irq_out);
	usb_free_coherent(gdata->udev, GCC_OUT_PKT_LEN, gdata->odata,
			 gdata->odata_dma);
}

static void gcc_input(struct gcc_data *gccdata, const u8 *keys)
{
	input_report_key(gccdata->input, BTN_A, !!(keys[1] & BIT(0)));
	input_report_key(gccdata->input, BTN_B, !!(keys[1] & BIT(1)));
	input_report_key(gccdata->input, BTN_X, !!(keys[1] & BIT(2)));
	input_report_key(gccdata->input, BTN_Y, !!(keys[1] & BIT(3)));
	input_report_key(gccdata->input, BTN_DPAD_LEFT, !!(keys[1] & BIT(4)));
	input_report_key(gccdata->input, BTN_DPAD_RIGHT, !!(keys[1] & BIT(5)));
	input_report_key(gccdata->input, BTN_DPAD_DOWN, !!(keys[1] & BIT(6)));
	input_report_key(gccdata->input, BTN_DPAD_UP, !!(keys[1] & BIT(7)));

	input_report_key(gccdata->input, BTN_START, !!(keys[2] & BIT(0)));
	input_report_key(gccdata->input, BTN_TR2, !!(keys[2] & BIT(1)));
	input_report_key(gccdata->input, BTN_TR, !!(keys[2] & BIT(2)));
	input_report_key(gccdata->input, BTN_TL, !!(keys[2] & BIT(3)));

	input_report_abs(gccdata->input, ABS_X, keys[3]);
	input_report_abs(gccdata->input, ABS_Y, keys[4] ^ 0xFF);
	input_report_abs(gccdata->input, ABS_RX, keys[5]);
	input_report_abs(gccdata->input, ABS_RY, keys[6] ^ 0xFF);
	input_report_abs(gccdata->input, ABS_Z, keys[7]);
	input_report_abs(gccdata->input, ABS_RZ, keys[8]);

	input_sync(gccdata->input);
}


static u8 ngc_connected_type(u8 status)
{
	u8 type = status & (GAMECUBE_WIRED | GAMECUBE_WIRELESS);

	switch (type) {
	case GAMECUBE_WIRED:
	case GAMECUBE_WIRELESS:
		return type;
	default:
		return 0;
	}
}

static int ngc_controller_init(struct gcc_data *gccdev, u8 status)
{
	int error;

	gccdev->input = input_allocate_device();
	if (!gccdev->input)
		return -ENOMEM;

	input_set_drvdata(gccdev->input, gccdev);
	usb_to_input_id(gccdev->adapter->udev, &gccdev->input->id);
	gccdev->input->name = "Nintendo GameCube Controller";
	gccdev->input->phys = gccdev->adapter->phys;

	set_bit(EV_KEY, gccdev->input->evbit);

	set_bit(BTN_A, gccdev->input->keybit);
	set_bit(BTN_B, gccdev->input->keybit);
	set_bit(BTN_X, gccdev->input->keybit);
	set_bit(BTN_Y, gccdev->input->keybit);
	set_bit(BTN_DPAD_LEFT, gccdev->input->keybit);
	set_bit(BTN_DPAD_RIGHT, gccdev->input->keybit);
	set_bit(BTN_DPAD_DOWN, gccdev->input->keybit);
	set_bit(BTN_DPAD_UP, gccdev->input->keybit);
	set_bit(BTN_START, gccdev->input->keybit);
	set_bit(BTN_TR2, gccdev->input->keybit);
	set_bit(BTN_TR, gccdev->input->keybit);
	set_bit(BTN_TL, gccdev->input->keybit);

	set_bit(EV_ABS, gccdev->input->evbit);

	set_bit(ABS_X, gccdev->input->absbit);
	set_bit(ABS_Y, gccdev->input->absbit);
	set_bit(ABS_RX, gccdev->input->absbit);
	set_bit(ABS_RY, gccdev->input->absbit);
	set_bit(ABS_Z, gccdev->input->absbit);
	set_bit(ABS_RZ, gccdev->input->absbit);

	input_set_abs_params(gccdev->input, ABS_X, 0, 255, 16, 16);
	input_set_abs_params(gccdev->input, ABS_Y, 0, 255, 16, 16);
	input_set_abs_params(gccdev->input, ABS_RX, 0, 255, 16, 16);
	input_set_abs_params(gccdev->input, ABS_RY, 0, 255, 16, 16);
	input_set_abs_params(gccdev->input, ABS_Z, 0, 255, 16, 0);
	input_set_abs_params(gccdev->input, ABS_RZ, 0, 255, 16, 0);
	error = ngc_init_ff(gccdev);
	if (error) {
		dev_warn(&gccdev->input->dev, "Could not create ff (skipped)");
		goto ngc_deinit_controller;
	}
	error = input_register_device(gccdev->input);
	if (error)
		goto ngc_deinit_controller_ff;
	gccdev->enable = true;
	return 0;
ngc_deinit_controller_ff:
	input_ff_destroy(gccdev->input);
ngc_deinit_controller:
	input_free_device(gccdev->input);
	return error;
}

static void ngc_controller_update_work(struct work_struct *work)
{
	int i;
	u8 status[4];
	bool enable[4];
	unsigned long flags;
	struct ngc_data *gdata = container_of(work, struct ngc_data, work);

	for (i = 0; i < 4; i++) {
		status[i] = gdata->controllers[i].status;
		enable[i] = ngc_connected_type(status[i]) != 0;
	}

	for (i = 0; i < 4; i++) {
		if (enable[i] && !gdata->controllers[i].enable) {
			if (ngc_controller_init(&gdata->controllers[i], status[i]) != 0)
				enable[i] = false;
		}
	}

	spin_lock_irqsave(&gdata->idata_lock, flags);
	for (i = 0; i < 4; i++)
		swap(gdata->controllers[i].enable, enable[i]);
	spin_unlock_irqrestore(&gdata->idata_lock, flags);

	for (i = 0; i < 4; i++) {
		if (enable[i] && !gdata->controllers[i].enable)
			input_unregister_device(gdata->controllers[i].input);
	}
}

static void ngc_input(struct ngc_data *gdata)
{
	int i;
	unsigned long flags;
	bool updated = false;

	for (i = 0; i < 4; i++) {
		updated = updated ||
			 gdata->idata[1 + 9 * i] != gdata->controllers[i].status;
		gdata->controllers[i].status = gdata->idata[1 + 9 * i];
	}
	if (updated)
		schedule_work(&gdata->work);
	spin_lock_irqsave(&gdata->idata_lock, flags);
	for (i = 0; i < 4; i++) {
		if (gdata->controllers[i].enable)
			gcc_input(&gdata->controllers[i], &gdata->idata[1 + 9 * i]);
	}
	spin_unlock_irqrestore(&gdata->idata_lock, flags);
}

static void ngc_irq_in(struct urb *urb)
{
	struct ngc_data *gdata = urb->context;
	struct usb_interface *intf = gdata->intf;
	int error;

	switch (urb->status) {
	case 0:
		break;
	case -EOVERFLOW:
	case -ECONNRESET:
	case -ENOENT:
	case -ESHUTDOWN:
		dev_dbg(&intf->dev, "controller urb shutting down: %d\n",
			urb->status);
		return;
	default:
		dev_dbg(&intf->dev, "controller urb status: %d\n", urb->status);
		goto exit;
	}
	if (gdata->irq_in->actual_length != GCC_IN_PKT_LEN)
		dev_warn(&intf->dev, "Bad sized packet\n");
	else if (gdata->idata[0] != 0x21)
		dev_warn(&intf->dev, "Unknown opcode %d\n", gdata->idata[0]);
	else
		ngc_input(gdata);
exit:
	error = usb_submit_urb(gdata->irq_in, GFP_ATOMIC);
	if (error)
		dev_err(&intf->dev, "controller urb failed: %d\n", error);
}

static int ngc_init_input(struct ngc_data *gdata,
			 struct usb_endpoint_descriptor *irq)
{
	int error = -ENOMEM;

	gdata->idata = usb_alloc_coherent(gdata->udev, GCC_IN_PKT_LEN, GFP_KERNEL,
			 &gdata->idata_dma);
	if (!gdata->idata)
		return error;

	gdata->irq_in = usb_alloc_urb(0, GFP_KERNEL);
	if (!gdata->irq_in)
		goto err_free_coherent;

	usb_fill_int_urb(gdata->irq_in, gdata->udev,
			 usb_rcvintpipe(gdata->udev, irq->bEndpointAddress),
			 gdata->idata, GCC_IN_PKT_LEN, ngc_irq_in, gdata,
			 irq->bInterval);
	gdata->irq_in->transfer_dma = gdata->idata_dma;
	gdata->irq_in->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;

	spin_lock_init(&gdata->idata_lock);
	INIT_WORK(&gdata->work, ngc_controller_update_work);

	return 0;

err_free_coherent:
	usb_free_coherent(gdata->udev, GCC_IN_PKT_LEN, gdata->idata,
			 gdata->idata_dma);
	return error;

}


static void ngc_deinit_input(struct ngc_data *gdata)
{
	usb_free_urb(gdata->irq_in);
	usb_free_coherent(gdata->udev, GCC_IN_PKT_LEN, gdata->idata,
			 gdata->idata_dma);
}


static int ngc_init_irq(struct ngc_data *gdata)
{
	struct usb_endpoint_descriptor *eps[] = { NULL, NULL };
	int error;

	error = usb_find_common_endpoints(gdata->intf->cur_altsetting, NULL, NULL,
					  &eps[0], &eps[1]);
	if (error)
		return -ENODEV;
	error = ngc_init_output(gdata, eps[1]);
	if (error)
		return error;
	error = ngc_init_input(gdata, eps[0]);
	if (error)
		goto err_deinit_out;
#ifdef CONFIG_JOYSTICK_NGC_FF
	memset(gdata->odata_rumbles, 0, 4);
	gdata->rumble_changed = false;
#endif
	gdata->irq_out_active = true;
	gdata->odata[0] = 0x13;
	gdata->irq_out->transfer_buffer_length = 1;

	error = usb_submit_urb(gdata->irq_in, GFP_KERNEL);
	if (error)
		goto err_deinit_in;

	usb_anchor_urb(gdata->irq_out, &gdata->irq_out_anchor);
	error = usb_submit_urb(gdata->irq_out, GFP_ATOMIC);
	if (error) {
		dev_err(&gdata->intf->dev,
			"%s - usb_submit_urb failed with result %d\n",
			__func__, error);
		usb_unanchor_urb(gdata->irq_out);
		error = -EIO;
		goto err_kill_in_urb;
	}

	return 0;
err_kill_in_urb:
	usb_kill_urb(gdata->irq_in);
err_deinit_in:
	ngc_deinit_input(gdata);
err_deinit_out:
	ngc_deinit_output(gdata);
	return error;
}

static void ngc_deinit_irq(struct ngc_data *gdata)
{
	if (!usb_wait_anchor_empty_timeout(&gdata->irq_out_anchor, 5000)) {
		dev_warn(&gdata->intf->dev,
			 "timed out waiting for output URB to complete, killing\n");
		usb_kill_anchored_urbs(&gdata->irq_out_anchor);
	}
	usb_kill_urb(gdata->irq_in);
	/* Make sure we are done with presence work if it was scheduled */
	flush_work(&gdata->work);

	ngc_deinit_input(gdata);
	ngc_deinit_output(gdata);
}

static void ngc_init_controllers(struct ngc_data *gdata)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(gdata->controllers); i++) {
		gdata->controllers[i].adapter = gdata;
		gdata->controllers[i].no = i;
		gdata->controllers[i].status = GAMECUBE_NONE;
		gdata->controllers[i].enable = false;
	}
}

static struct attribute *ngc_attrs[] = {
	NULL,
};

static const struct attribute_group ngc_attr_group = {
	.attrs = ngc_attrs,
};

static int ngc_init_attr(struct ngc_data *gdata)
{
	return sysfs_create_group(&gdata->intf->dev.kobj, &ngc_attr_group);
}

static void ngc_deinit_attr(struct ngc_data *gdata)
{
	sysfs_remove_group(&gdata->intf->dev.kobj, &ngc_attr_group);
}


static int ngc_usb_probe(struct usb_interface *iface, const struct usb_device_id *id)
{
	struct usb_device *udev = interface_to_usbdev(iface);
	struct ngc_data *gdata;
	int error;

	gdata = kzalloc(sizeof(struct ngc_data), GFP_KERNEL);
	if (!gdata)
		return -ENOMEM;
	usb_set_intfdata(iface, gdata);
	gdata->udev = udev;
	gdata->intf = iface;

	usb_make_path(udev, gdata->phys, sizeof(gdata->phys));
	strlcat(gdata->phys, "/input0", sizeof(gdata->phys));

	ngc_init_controllers(gdata);
	error = ngc_init_irq(gdata);
	if (error)
		goto err_free_devs;
	error = ngc_init_attr(gdata);
	if (error)
		goto err_deinit_endpoints;
	dev_info(&iface->dev, "New device registered\n");
	return 0;
err_deinit_endpoints:
	ngc_deinit_irq(gdata);
err_free_devs:
	usb_set_intfdata(iface, NULL);
	kfree(gdata);
	return error;
}

static void ngc_usb_disconnect(struct usb_interface *iface)
{
	int i;
	struct ngc_data *gdata = usb_get_intfdata(iface);

	for (i = 0; i < 4; i++) {
		if (gdata->controllers[i].enable)
			input_unregister_device(gdata->controllers[i].input);
	}
	ngc_deinit_attr(gdata);
	ngc_deinit_irq(gdata);
	usb_set_intfdata(iface, NULL);
	kfree(gdata);
}

static const struct usb_device_id ngc_usb_devices[] = {
	{ USB_DEVICE(USB_VENDOR_ID_NINTENDO,
		     USB_DEVICE_ID_NINTENDO_GCADAPTER) },
	{}
};

MODULE_DEVICE_TABLE(usb, ngc_usb_devices);

static struct usb_driver ngc_usb_driver = {
	.name		= "gamecube_adapter",
	.id_table	= ngc_usb_devices,
	.probe		= ngc_usb_probe,
	.disconnect	= ngc_usb_disconnect,
};

module_usb_driver(ngc_usb_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Robin Milas <milas.robin@live.fr>");
MODULE_DESCRIPTION("Driver for GameCube adapter");
