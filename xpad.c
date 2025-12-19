#include <linux/version.h>
#include <linux/bits.h>
#include <linux/kernel.h>
#include <linux/input.h>
#include <linux/slab.h>
#include <linux/stat.h>
#include <linux/module.h>
#include <linux/usb/input.h>
#include <linux/timer.h>

#if LINUX_VERSION_CODE < KERNEL_VERSION(6,15,0)
#define timer_delete_sync del_timer_sync
#endif

#ifndef ABS_PROFILE
#define ABS_PROFILE ABS_MISC
#endif

#define XPAD_PKT_LEN 64

#define MAP_MOUSE_EMULATION		(1 << 0)

#define XTYPE_XBOXONE     3

static bool mouse_emulation = true;
module_param(mouse_emulation, bool, S_IRUGO);
MODULE_PARM_DESC(mouse_emulation, "Enable mouse emulation for Xbox Series S controller");

static int mouse_sensitivity = 3;
module_param(mouse_sensitivity, int, S_IRUGO);
MODULE_PARM_DESC(mouse_sensitivity, "Mouse sensitivity (1-10)");

static int scroll_sensitivity = 2;
module_param(scroll_sensitivity, int, S_IRUGO);
MODULE_PARM_DESC(scroll_sensitivity, "Scroll sensitivity (1-10)");

static bool mouse_debug = true;
module_param(mouse_debug, bool, S_IRUGO);
MODULE_PARM_DESC(mouse_debug, "Enable debug messages for mouse emulation");

static const struct xpad_device {
	u16 idVendor;
	u16 idProduct;
	char *name;
	u8 mapping;
	u8 xtype;
} xpad_device[] = {
	{ 0x045e, 0x0b12, "Microsoft Xbox Series S|X Controller", MAP_MOUSE_EMULATION, XTYPE_XBOXONE },
	{ 0x0000, 0x0000, "Generic X-Box pad", 0, XTYPE_XBOXONE }
};

#define XPAD_XBOXONE_VENDOR_PROTOCOL(vend, pr) \
	.match_flags = USB_DEVICE_ID_MATCH_VENDOR | USB_DEVICE_ID_MATCH_INT_INFO, \
	.idVendor = (vend), \
	.bInterfaceClass = USB_CLASS_VENDOR_SPEC, \
	.bInterfaceSubClass = 71, \
	.bInterfaceProtocol = (pr)
#define XPAD_XBOXONE_VENDOR(vend) \
	{ XPAD_XBOXONE_VENDOR_PROTOCOL((vend), 208) }

static const struct usb_device_id xpad_table[] = {
	XPAD_XBOXONE_VENDOR(0x045e),
	{ }
};

MODULE_DEVICE_TABLE(usb, xpad_table);

#define GIP_CMD_POWER    0x05
#define GIP_CMD_VIRTUAL_KEY  0x07
#define GIP_CMD_INPUT    0x20

#define GIP_SEQ0 0x00
#define GIP_OPT_INTERNAL 0x20
#define GIP_PL_LEN(N) (N)
#define GIP_PWR_ON 0x00
#define GIP_WIRED_INTF_DATA 0

static const u8 xboxone_power_on[] = {
	GIP_CMD_POWER, GIP_OPT_INTERNAL, GIP_SEQ0, GIP_PL_LEN(1), GIP_PWR_ON
};

struct xpad_mouse {
	struct input_dev *dev;
	bool left_button;
	bool right_button;
	bool middle_button;
	int deadzone;
};

struct usb_xpad {
	struct usb_device *udev;
	struct usb_interface *intf;

	struct urb *irq_in;
	unsigned char *idata;
	dma_addr_t idata_dma;

	struct urb *irq_out;
	struct usb_anchor irq_out_anchor;
	bool irq_out_active;
	u8 odata_serial;
	unsigned char *odata;
	dma_addr_t odata_dma;
	spinlock_t odata_lock;

	char phys[64];
	int mapping;
	int xtype;
	const char *name;
	
	struct xpad_mouse *mouse;
};

static int xpad_init_mouse(struct usb_xpad *xpad)
{
	struct input_dev *input_dev;
	int error;

	if (!(xpad->mapping & MAP_MOUSE_EMULATION))
		return 0;

	input_dev = input_allocate_device();
	if (!input_dev)
		return -ENOMEM;

	xpad->mouse = kzalloc(sizeof(struct xpad_mouse), GFP_KERNEL);
	if (!xpad->mouse) {
		error = -ENOMEM;
		goto err_free_input;
	}

	xpad->mouse->dev = input_dev;
	xpad->mouse->deadzone = 8000;

	input_dev->name = "Xbox Series S Mouse Emulation";
	input_dev->phys = xpad->phys;
	strlcat(input_dev->phys, "/mouse1", sizeof(input_dev->phys));
	usb_to_input_id(xpad->udev, &input_dev->id);
	input_dev->dev.parent = &xpad->intf->dev;

	__set_bit(EV_KEY, input_dev->evbit);
	__set_bit(EV_REL, input_dev->evbit);
	
	__set_bit(BTN_LEFT, input_dev->keybit);
	__set_bit(BTN_RIGHT, input_dev->keybit);
	__set_bit(BTN_MIDDLE, input_dev->keybit);
	
	__set_bit(REL_X, input_dev->relbit);
	__set_bit(REL_Y, input_dev->relbit);
	__set_bit(REL_WHEEL, input_dev->relbit);
	__set_bit(REL_HWHEEL, input_dev->relbit);

	error = input_register_device(input_dev);
	if (error)
		goto err_free_mouse;

	printk(KERN_INFO "xpad: Mouse emulation initialized for %s\n", xpad->name);
	printk(KERN_INFO "xpad: Mouse sensitivity: %d, Scroll sensitivity: %d\n", 
	       mouse_sensitivity, scroll_sensitivity);
	if (mouse_debug)
		printk(KERN_INFO "xpad: Mouse debug mode enabled\n");

	return 0;

err_free_mouse:
	kfree(xpad->mouse);
	xpad->mouse = NULL;
err_free_input:
	input_free_device(input_dev);
	return error;
}

static void xpad_deinit_mouse(struct usb_xpad *xpad)
{
	if (xpad->mouse) {
		if (xpad->mouse->dev) {
			input_unregister_device(xpad->mouse->dev);
			input_free_device(xpad->mouse->dev);
		}
		kfree(xpad->mouse);
		xpad->mouse = NULL;
	}
}

static void xpad_update_mouse(struct usb_xpad *xpad, unsigned char *data)
{
	struct xpad_mouse *mouse = xpad->mouse;
	if (!mouse) return;

	int left_x_raw = (__s16)le16_to_cpup((__le16 *)(data + 10));
	int left_y_raw = (__s16)le16_to_cpup((__le16 *)(data + 12));
	
	int right_x_raw = (__s16)le16_to_cpup((__le16 *)(data + 14));
	int right_y_raw = ~(__s16)le16_to_cpup((__le16 *)(data + 16));

	int deadzone = mouse->deadzone;
	int sens = clamp(mouse_sensitivity, 1, 10);
	int scroll_sens = clamp(scroll_sensitivity, 1, 10);

	int move_x = 0, move_y = 0;
	int scroll_x = 0, scroll_y = 0;
	
	if (abs(left_x_raw) > deadzone) {
		move_x = (left_x_raw * sens) / 32767;
		input_report_rel(mouse->dev, REL_X, move_x);
	}
	if (abs(left_y_raw) > deadzone) {
		move_y = -(left_y_raw * sens) / 32767;
		input_report_rel(mouse->dev, REL_Y, move_y);
	}

	if (abs(right_y_raw) > deadzone) {
		scroll_y = -(right_y_raw * scroll_sens) / 32767;
		input_report_rel(mouse->dev, REL_WHEEL, scroll_y);
	}
	if (abs(right_x_raw) > deadzone) {
		scroll_x = (right_x_raw * scroll_sens) / 32767;
		input_report_rel(mouse->dev, REL_HWHEEL, scroll_x);
	}

	if (mouse_debug && (move_x != 0 || move_y != 0 || scroll_x != 0 || scroll_y != 0)) {
		if (move_x != 0 || move_y != 0) {
			printk(KERN_DEBUG "xpad_mouse: Moved - X: %d, Y: %d (raw: X=%d, Y=%d)\n", 
			       move_x, move_y, left_x_raw, left_y_raw);
		}
		if (scroll_x != 0 || scroll_y != 0) {
			printk(KERN_DEBUG "xpad_mouse: Scrolled - Horizontal: %d, Vertical: %d (raw: X=%d, Y=%d)\n", 
			       scroll_x, scroll_y, right_x_raw, right_y_raw);
		}
	}

	bool a_pressed = data[4] & BIT(4);
	bool b_pressed = data[4] & BIT(5);
	bool x_pressed = data[4] & BIT(6);

	if (mouse_debug && (a_pressed != mouse->left_button || 
	                    b_pressed != mouse->right_button || 
	                    x_pressed != mouse->middle_button)) {
		if (a_pressed != mouse->left_button) {
			printk(KERN_DEBUG "xpad_mouse: Left button %s\n", 
			       a_pressed ? "pressed" : "released");
		}
		if (b_pressed != mouse->right_button) {
			printk(KERN_DEBUG "xpad_mouse: Right button %s\n", 
			       b_pressed ? "pressed" : "released");
		}
		if (x_pressed != mouse->middle_button) {
			printk(KERN_DEBUG "xpad_mouse: Middle button %s\n", 
			       x_pressed ? "pressed" : "released");
		}
	}

	if (a_pressed != mouse->left_button) {
		input_report_key(mouse->dev, BTN_LEFT, a_pressed);
		mouse->left_button = a_pressed;
	}

	if (b_pressed != mouse->right_button) {
		input_report_key(mouse->dev, BTN_RIGHT, b_pressed);
		mouse->right_button = b_pressed;
	}

	if (x_pressed != mouse->middle_button) {
		input_report_key(mouse->dev, BTN_MIDDLE, x_pressed);
		mouse->middle_button = x_pressed;
	}

	input_sync(mouse->dev);
}

static void xpadone_process_packet(struct usb_xpad *xpad, u16 cmd, unsigned char *data)
{
	if ((xpad->mapping & MAP_MOUSE_EMULATION) && xpad->mouse && data[0] == GIP_CMD_INPUT) {
		xpad_update_mouse(xpad, data);
	}
}

static void xpad_irq_in(struct urb *urb)
{
	struct usb_xpad *xpad = urb->context;
	struct device *dev = &xpad->intf->dev;
	int retval, status;

	status = urb->status;

	switch (status) {
	case 0:
		break;
	case -ECONNRESET:
	case -ENOENT:
	case -ESHUTDOWN:
		dev_dbg(dev, "%s - urb shutting down with status: %d\n",
			__func__, status);
		return;
	default:
		dev_dbg(dev, "%s - nonzero urb status received: %d\n",
			__func__, status);
		goto exit;
	}

	xpadone_process_packet(xpad, 0, xpad->idata);

exit:
	retval = usb_submit_urb(urb, GFP_ATOMIC);
	if (retval)
		dev_err(dev, "%s - usb_submit_urb failed with result %d\n",
			__func__, retval);
}

static void xpad_irq_out(struct urb *urb)
{
	struct usb_xpad *xpad = urb->context;
	struct device *dev = &xpad->intf->dev;
	int status = urb->status;

	switch (status) {
	case 0:
		break;
	case -ECONNRESET:
	case -ENOENT:
	case -ESHUTDOWN:
		dev_dbg(dev, "%s - urb shutting down with status: %d\n",
			__func__, status);
		break;
	default:
		dev_dbg(dev, "%s - nonzero urb status received: %d\n",
			__func__, status);
		break;
	}
}

static int xpad_init_output(struct usb_interface *intf, struct usb_xpad *xpad,
			struct usb_endpoint_descriptor *ep_irq_out)
{
	int error;

	init_usb_anchor(&xpad->irq_out_anchor);

	xpad->odata = usb_alloc_coherent(xpad->udev, XPAD_PKT_LEN,
					 GFP_KERNEL, &xpad->odata_dma);
	if (!xpad->odata)
		return -ENOMEM;

	spin_lock_init(&xpad->odata_lock);

	xpad->irq_out = usb_alloc_urb(0, GFP_KERNEL);
	if (!xpad->irq_out) {
		error = -ENOMEM;
		goto err_free_coherent;
	}

	usb_fill_int_urb(xpad->irq_out, xpad->udev,
			 usb_sndintpipe(xpad->udev, ep_irq_out->bEndpointAddress),
			 xpad->odata, XPAD_PKT_LEN,
			 xpad_irq_out, xpad, ep_irq_out->bInterval);
	xpad->irq_out->transfer_dma = xpad->odata_dma;
	xpad->irq_out->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;

	return 0;

err_free_coherent:
	usb_free_coherent(xpad->udev, XPAD_PKT_LEN, xpad->odata, xpad->odata_dma);
	return error;
}

static void xpad_deinit_output(struct usb_xpad *xpad)
{
	usb_free_urb(xpad->irq_out);
	usb_free_coherent(xpad->udev, XPAD_PKT_LEN, xpad->odata, xpad->odata_dma);
}

static int xpad_start_xbox_one(struct usb_xpad *xpad)
{
	unsigned long flags;
	int retval;

	spin_lock_irqsave(&xpad->odata_lock, flags);
	
	memcpy(xpad->odata, xboxone_power_on, sizeof(xboxone_power_on));
	xpad->odata[2] = xpad->odata_serial++;
	xpad->irq_out->transfer_buffer_length = sizeof(xboxone_power_on);
	
	usb_anchor_urb(xpad->irq_out, &xpad->irq_out_anchor);
	retval = usb_submit_urb(xpad->irq_out, GFP_ATOMIC);
	if (retval) {
		dev_err(&xpad->intf->dev,
			"%s - usb_submit_urb failed with result %d\n",
			__func__, retval);
		usb_unanchor_urb(xpad->irq_out);
	} else {
		xpad->irq_out_active = true;
	}

	spin_unlock_irqrestore(&xpad->odata_lock, flags);

	return retval;
}

static int xpad_start_input(struct usb_xpad *xpad)
{
	int error;

	if (usb_submit_urb(xpad->irq_in, GFP_KERNEL))
		return -EIO;

	error = xpad_start_xbox_one(xpad);
	if (error) {
		usb_kill_urb(xpad->irq_in);
		return error;
	}

	return 0;
}

static void xpad_stop_input(struct usb_xpad *xpad)
{
	usb_kill_urb(xpad->irq_in);
}

static void xpad_deinit(struct usb_xpad *xpad)
{
	xpad_deinit_mouse(xpad);
}

static int xpad_probe(struct usb_interface *intf, const struct usb_device_id *id)
{
	struct usb_device *udev = interface_to_usbdev(intf);
	struct usb_xpad *xpad;
	struct usb_endpoint_descriptor *ep_irq_in, *ep_irq_out;
	int i, error;

	if (intf->cur_altsetting->desc.bNumEndpoints != 2)
		return -ENODEV;

	for (i = 0; xpad_device[i].idVendor; i++) {
		if ((le16_to_cpu(udev->descriptor.idVendor) == xpad_device[i].idVendor) &&
		    (le16_to_cpu(udev->descriptor.idProduct) == xpad_device[i].idProduct))
			break;
	}

	xpad = kzalloc(sizeof(*xpad), GFP_KERNEL);
	if (!xpad)
		return -ENOMEM;

	usb_make_path(udev, xpad->phys, sizeof(xpad->phys));
	strlcat(xpad->phys, "/input0", sizeof(xpad->phys));

	xpad->idata = usb_alloc_coherent(udev, XPAD_PKT_LEN,
					 GFP_KERNEL, &xpad->idata_dma);
	if (!xpad->idata) {
		error = -ENOMEM;
		goto err_free_mem;
	}

	xpad->irq_in = usb_alloc_urb(0, GFP_KERNEL);
	if (!xpad->irq_in) {
		error = -ENOMEM;
		goto err_free_idata;
	}

	xpad->udev = udev;
	xpad->intf = intf;
	xpad->mapping = xpad_device[i].mapping;
	xpad->xtype = xpad_device[i].xtype;
	xpad->name = xpad_device[i].name;

	if (mouse_emulation)
		xpad->mapping |= MAP_MOUSE_EMULATION;
	
	printk(KERN_INFO "xpad: Device %s connected (VID:PID %04x:%04x)\n",
	       xpad->name, le16_to_cpu(udev->descriptor.idVendor),
	       le16_to_cpu(udev->descriptor.idProduct));

	if (xpad->mapping & MAP_MOUSE_EMULATION) {
		printk(KERN_INFO "xpad: Mouse emulation enabled for this device\n");
	}

	if (xpad->xtype == XTYPE_XBOXONE &&
	    intf->cur_altsetting->desc.bInterfaceNumber != GIP_WIRED_INTF_DATA) {
		error = -ENODEV;
		goto err_free_in_urb;
	}

	ep_irq_in = ep_irq_out = NULL;

	for (i = 0; i < 2; i++) {
		struct usb_endpoint_descriptor *ep =
				&intf->cur_altsetting->endpoint[i].desc;

		if (usb_endpoint_xfer_int(ep)) {
			if (usb_endpoint_dir_in(ep))
				ep_irq_in = ep;
			else
				ep_irq_out = ep;
		}
	}

	if (!ep_irq_in || !ep_irq_out) {
		error = -ENODEV;
		goto err_free_in_urb;
	}

	error = xpad_init_output(intf, xpad, ep_irq_out);
	if (error)
		goto err_free_in_urb;

	usb_fill_int_urb(xpad->irq_in, udev,
			 usb_rcvintpipe(udev, ep_irq_in->bEndpointAddress),
			 xpad->idata, XPAD_PKT_LEN, xpad_irq_in,
			 xpad, ep_irq_in->bInterval);
	xpad->irq_in->transfer_dma = xpad->idata_dma;
	xpad->irq_in->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;

	usb_set_intfdata(intf, xpad);

	error = xpad_init_mouse(xpad);
	if (error) {
		dev_warn(&xpad->intf->dev,
			 "unable to initialize mouse emulation: %d\n", error);
		goto err_deinit_output;
	}

	error = xpad_start_input(xpad);
	if (error) {
		dev_err(&xpad->intf->dev,
			"unable to start input: %d\n", error);
		goto err_deinit_all;
	}

	return 0;

err_deinit_all:
	xpad_deinit(xpad);
err_deinit_output:
	xpad_deinit_output(xpad);
err_free_in_urb:
	usb_free_urb(xpad->irq_in);
err_free_idata:
	usb_free_coherent(udev, XPAD_PKT_LEN, xpad->idata, xpad->idata_dma);
err_free_mem:
	kfree(xpad);
	return error;
}

static void xpad_disconnect(struct usb_interface *intf)
{
	struct usb_xpad *xpad = usb_get_intfdata(intf);

	printk(KERN_INFO "xpad: Device %s disconnected\n", xpad->name);
	
	xpad_stop_input(xpad);
	xpad_deinit(xpad);
	xpad_deinit_output(xpad);
	usb_free_urb(xpad->irq_in);
	usb_free_coherent(xpad->udev, XPAD_PKT_LEN, xpad->idata, xpad->idata_dma);
	kfree(xpad);

	usb_set_intfdata(intf, NULL);
}

static struct usb_driver xpad_driver = {
	.name		= "xpad",
	.probe		= xpad_probe,
	.disconnect	= xpad_disconnect,
	.id_table	= xpad_table,
};

module_usb_driver(xpad_driver);

MODULE_AUTHOR("Trokhan Andrey");
MODULE_DESCRIPTION("Xbox gamepad mouse emulation driver");
MODULE_LICENSE("GPL");