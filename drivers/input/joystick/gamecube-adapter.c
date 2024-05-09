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

#define CONTROLLER_COUNT 4

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
	u8 *odata;
	dma_addr_t odata_dma;
	spinlock_t odata_lock;		/* output data */
#ifdef CONFIG_JOYSTICK_NGC_FF
	bool irq_out_active;		/* we must not use an active URB */
	u8 odata_rumbles[CONTROLLER_COUNT];
	bool rumble_changed;		/* if rumble need update*/
#endif

	struct gcc_data controllers[CONTROLLER_COUNT];
	struct work_struct work;	/* create/delete controller input files */
};

struct ngc_key {
	u8		byte;
	u8		bit;
	unsigned int	keycode;
};

static const struct ngc_key ngc_keys[] = {
	{ 1, 0, BTN_A },
	{ 1, 1, BTN_B },
	{ 1, 2, BTN_X },
	{ 1, 3, BTN_Y },
	{ 1, 4, BTN_DPAD_LEFT },
	{ 1, 5, BTN_DPAD_RIGHT },
	{ 1, 6, BTN_DPAD_DOWN },
	{ 1, 7, BTN_DPAD_UP },
	{ 2, 0, BTN_START },
	{ 2, 1, BTN_TR2 },
	{ 2, 2, BTN_TR },
	{ 2, 3, BTN_TL },
};

static int ngc_send_urb(struct ngc_data *gdata)
{
	int error;

	lockdep_assert_held(&gdata->odata_lock);

	error = usb_submit_urb(gdata->irq_out, GFP_ATOMIC);
	if (error) {
		dev_err(&gdata->intf->dev,
			"%s - usb_submit_urb failed with result %d\n",
			__func__, error);
		error = -EIO;

	}
#ifdef CONFIG_JOYSTICK_NGC_FF
	gdata->irq_out_active = error == 0;
#endif
	return error;
}

#ifdef CONFIG_JOYSTICK_NGC_FF

static bool ngc_prepare_next_packet(struct ngc_data *gdata)
{
	lockdep_assert_held(&gdata->odata_lock);

	if (!gdata->rumble_changed)
		return false;
	gdata->rumble_changed = false;
	memcpy(gdata->odata + 1, gdata->odata_rumbles,
			 sizeof(gdata->odata_rumbles));
	gdata->odata[0] = 0x11;
	gdata->irq_out->transfer_buffer_length = 5;
	return true;
}

static int ngc_set_rumble_value(struct ngc_data *gdata, u8 controller, bool value)
{
	if (controller > CONTROLLER_COUNT)
		return -EINVAL;
	guard(spinlock_irqsave)(&gdata->odata_lock);
	if (gdata->odata_rumbles[controller] == value)
		return 0;
	gdata->odata_rumbles[controller] = value;
	gdata->rumble_changed = true;
	if (gdata->irq_out_active)
		return 0;
	ngc_prepare_next_packet(gdata);
	return ngc_send_urb(gdata);
}

static int ngc_rumble_play(struct input_dev *dev, void *data,
			      struct ff_effect *eff)
{
	struct gcc_data *gccdata = input_get_drvdata(dev);

	/*
	 * The gamecube controller  supports only a single rumble motor so if any
	 * magnitude is set to non-zero then we start the rumble motor. If both are
	 * set to zero, we stop the rumble motor.
	 */

	return ngc_set_rumble_value(gccdata->adapter, gccdata->no,
				    eff->u.rumble.strong_magnitude ||
					eff->u.rumble.weak_magnitude);
}
static int ngc_init_ff(struct gcc_data *gccdev)
{
	input_set_capability(gccdev->input, EV_FF, FF_RUMBLE);

	return input_ff_create_memless(gccdev->input, NULL, ngc_rumble_play);
}
#else
static int ngc_init_ff(struct gcc_data *gccdev) { return 0; }
static bool ngc_prepare_next_packet(struct ngc_data *gdata) { return false; }
#endif

static void ngc_irq_out(struct urb *urb)
{
	struct ngc_data *gdata = urb->context;
	struct device *dev = &gdata->intf->dev;

	guard(spinlock_irqsave)(&gdata->odata_lock);

	switch (urb->status) {
	case 0:
		/* success */
		break;
	case -ECONNRESET:
	case -ENOENT:
	case -ESHUTDOWN:
		/* this urb is terminated, clean up */
		dev_dbg(dev, "%s - urb shutting down with status: %d\n",
			__func__, urb->status);
		goto shutdown;

	default:
		dev_dbg(dev, "%s - nonzero urb status received: %d\n",
			__func__, urb->status);
		goto resubmit;
	}
	if (!ngc_prepare_next_packet(gdata))
		goto shutdown;
resubmit:
	ngc_send_urb(gdata);
	return;
shutdown:
#ifdef CONFIG_JOYSTICK_NGC_FF
	gdata->irq_out_active = false;
#endif
}

static int ngc_init_output(struct ngc_data *gdata,
			 struct usb_endpoint_descriptor *irq)
{
	int error = -ENOMEM;

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
	int i;

	for (i = 0; i < ARRAY_SIZE(ngc_keys); i++) {
		input_report_key(gccdata->input, ngc_keys[i].keycode,
			!!(keys[ngc_keys[i].byte] & BIT(ngc_keys[i].bit)));
	}
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
	int i;
	int error;

	gccdev->input = input_allocate_device();
	if (!gccdev->input)
		return -ENOMEM;

	input_set_drvdata(gccdev->input, gccdev);
	usb_to_input_id(gccdev->adapter->udev, &gccdev->input->id);
	gccdev->input->name = "Nintendo GameCube Controller";
	gccdev->input->phys = gccdev->adapter->phys;

	set_bit(EV_KEY, gccdev->input->evbit);
	for (i = 0; i < ARRAY_SIZE(ngc_keys); i++)
		set_bit(ngc_keys[i].keycode, gccdev->input->keybit);
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
	u8 status[CONTROLLER_COUNT];
	bool enable[CONTROLLER_COUNT];
	unsigned long flags;
	struct ngc_data *gdata = container_of(work, struct ngc_data, work);

	for (i = 0; i < CONTROLLER_COUNT; i++) {
		status[i] = gdata->controllers[i].status;
		enable[i] = ngc_connected_type(status[i]) != 0;
	}

	for (i = 0; i < CONTROLLER_COUNT; i++) {
		if (enable[i] && !gdata->controllers[i].enable) {
			if (ngc_controller_init(&gdata->controllers[i], status[i]) != 0)
				enable[i] = false;
		}
	}

	spin_lock_irqsave(&gdata->idata_lock, flags);
	for (i = 0; i < CONTROLLER_COUNT; i++)
		swap(gdata->controllers[i].enable, enable[i]);
	spin_unlock_irqrestore(&gdata->idata_lock, flags);

	for (i = 0; i < CONTROLLER_COUNT; i++) {
		if (enable[i] && !gdata->controllers[i].enable)
			input_unregister_device(gdata->controllers[i].input);
	}
}

static void ngc_input(struct ngc_data *gdata)
{
	int i;
	unsigned long flags;
	bool updated = false;

	for (i = 0; i < CONTROLLER_COUNT; i++) {
		updated = updated ||
			 gdata->idata[1 + 9 * i] != gdata->controllers[i].status;
		gdata->controllers[i].status = gdata->idata[1 + 9 * i];
	}
	if (updated)
		schedule_work(&gdata->work);
	spin_lock_irqsave(&gdata->idata_lock, flags);
	for (i = 0; i < CONTROLLER_COUNT; i++) {
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
	gdata->rumble_changed = false;
	gdata->irq_out_active = true;
#endif
	gdata->odata[0] = 0x13;
	gdata->irq_out->transfer_buffer_length = 1;

	error = usb_submit_urb(gdata->irq_in, GFP_KERNEL);
	if (error)
		goto err_deinit_in;

	error = usb_submit_urb(gdata->irq_out, GFP_KERNEL);
	if (error) {
		dev_err(&gdata->intf->dev,
			"%s - usb_submit_urb failed with result %d\n",
			__func__, error);
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
	usb_kill_urb(gdata->irq_out);
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
	dev_info(&iface->dev, "New device registered\n");
	return 0;
err_free_devs:
	usb_set_intfdata(iface, NULL);
	kfree(gdata);
	return error;
}

static void ngc_usb_disconnect(struct usb_interface *iface)
{
	struct ngc_data *gdata = usb_get_intfdata(iface);
	int i;

	for (i = 0; i < CONTROLLER_COUNT; i++) {
		if (gdata->controllers[i].enable)
			input_unregister_device(gdata->controllers[i].input);
	}
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
