#include <linux/version.h>
#include <linux/bits.h>
#include <linux/kernel.h>
#include <linux/input.h>
#include <linux/slab.h>
#include <linux/stat.h>
#include <linux/module.h>
#include <linux/usb/input.h>
#include <linux/timer.h>

// backward compatibility
#if LINUX_VERSION_CODE < KERNEL_VERSION(6,15,0)
#define timer_delete_sync del_timer_sync
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(6,16,0)
#define timer_container_of from_timer
#endif

#ifndef ABS_PROFILE
#define ABS_PROFILE ABS_MISC
#endif

#define XPAD_PKT_LEN 64

/*
 * xbox d-pads should map to buttons, as is required for DDR pads
 * but we map them to axes when possible to simplify things
 */
#define MAP_DPAD_TO_BUTTONS		(1 << 0)
#define MAP_TRIGGERS_TO_BUTTONS		(1 << 1)
#define MAP_STICKS_TO_NULL		(1 << 2)
#define MAP_SELECT_BUTTON		(1 << 3)
#define MAP_PADDLES			(1 << 4)
#define MAP_PROFILE_BUTTON		(1 << 5)
#define MAP_MOUSE_EMULATION		(1 << 6)

#define XTYPE_XBOXONE     3

static bool mouse_emulation = true;
module_param(mouse_emulation, bool, S_IRUGO);
MODULE_PARM_DESC(mouse_emulation, "Enable mouse emulation for Xbox Series S controller");

// Параметры чувствительности мыши
static int mouse_sensitivity = 3;
module_param(mouse_sensitivity, int, S_IRUGO);
MODULE_PARM_DESC(mouse_sensitivity, "Mouse sensitivity (1-10)");

static int scroll_sensitivity = 2;
module_param(scroll_sensitivity, int, S_IRUGO);
MODULE_PARM_DESC(scroll_sensitivity, "Scroll sensitivity (1-10)");

// Только Xbox Series S|X контроллер
static const struct xpad_device {
	u16 idVendor;
	u16 idProduct;
	char *name;
	u8 mapping;
	u8 xtype;
} xpad_device[] = {
	{ 0x045e, 0x0b12, "Microsoft Xbox Series S|X Controller", MAP_SELECT_BUTTON | MAP_MOUSE_EMULATION, XTYPE_XBOXONE },
	{ 0x0000, 0x0000, "Generic X-Box pad", 0, XTYPE_XBOXONE }
};

/* buttons shared with xbox */
static const signed short xpad_common_btn[] = {
	BTN_A, BTN_B, BTN_X, BTN_Y,
	BTN_START, BTN_SELECT, BTN_THUMBL, BTN_THUMBR,
	-1
};

static const signed short xpad360_btn[] = {
	BTN_TL, BTN_TR,
	BTN_MODE,
	-1
};

static const signed short xpad_abs[] = {
	ABS_X, ABS_Y,
	ABS_RX, ABS_RY,
	-1
};

static const signed short xpad_abs_pad[] = {
	ABS_HAT0X, ABS_HAT0Y,
	-1
};

static const signed short xpad_abs_triggers[] = {
	ABS_Z, ABS_RZ,
	-1
};

/* The Xbox One controller uses subclass 71 and protocol 208. */
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

/*
 * starting with xbox one, the game input protocol is used
 */
#define GIP_CMD_ACK      0x01
#define GIP_CMD_IDENTIFY 0x04
#define GIP_CMD_POWER    0x05
#define GIP_CMD_VIRTUAL_KEY  0x07
#define GIP_CMD_RUMBLE   0x09
#define GIP_CMD_FIRMWARE 0x0c
#define GIP_CMD_INPUT    0x20

#define GIP_SEQ0 0x00
#define GIP_OPT_INTERNAL 0x20
#define GIP_PL_LEN(N) (N)
#define GIP_PWR_ON 0x00
#define GIP_WIRED_INTF_DATA 0

/*
 * This packet is required for all Xbox One pads with 2015
 * or later firmware installed (or present from the factory).
 */
static const u8 xboxone_power_on[] = {
	GIP_CMD_POWER, GIP_OPT_INTERNAL, GIP_SEQ0, GIP_PL_LEN(1), GIP_PWR_ON
};

/*
 * This packet is required for Xbox One S and Xbox One Elite Series 2 pads to
 * initialize the controller that was previously used in Bluetooth mode.
 */
static const u8 xboxone_s_init[] = {
	GIP_CMD_POWER, GIP_OPT_INTERNAL, GIP_SEQ0, 0x0f, 0x06
};

struct xpad_output_packet {
	u8 data[XPAD_PKT_LEN];
	u8 len;
	bool pending;
};

#define XPAD_OUT_CMD_IDX	0
#define XPAD_NUM_OUT_PACKETS	1

// Структура для эмуляции мыши
struct xpad_mouse {
	struct input_dev *dev;
	bool left_button;
	bool right_button;
	bool middle_button;
	int deadzone;
};

struct usb_xpad {
	struct input_dev *dev;
	struct usb_device *udev;
	struct usb_interface *intf;

	bool input_created;

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

	struct xpad_output_packet out_packets[XPAD_NUM_OUT_PACKETS];
	int last_out_packet;
	int init_seq;

	char phys[64];
	int mapping;
	int xtype;
	const char *name;
	
	// Поддержка мыши
	struct xpad_mouse *mouse;
};

static int xpad_init_input(struct usb_xpad *xpad);
static void xpad_deinit_input(struct usb_xpad *xpad);
static void xpadone_ack_mode_report(struct usb_xpad *xpad, u8 seq_num);

// Функции для эмуляции мыши
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

	// Настраиваем устройство мыши
	input_dev->name = "Xbox Series S Mouse Emulation";
	input_dev->phys = xpad->phys;
	strlcat(input_dev->phys, "/mouse1", sizeof(input_dev->phys));
	usb_to_input_id(xpad->udev, &input_dev->id);
	input_dev->dev.parent = &xpad->intf->dev;

	// Устанавливаем возможности мыши
	__set_bit(EV_KEY, input_dev->evbit);
	__set_bit(EV_REL, input_dev->evbit);
	
	// Кнопки мыши
	__set_bit(BTN_LEFT, input_dev->keybit);
	__set_bit(BTN_RIGHT, input_dev->keybit);
	__set_bit(BTN_MIDDLE, input_dev->keybit);
	
	// Относительные оси
	__set_bit(REL_X, input_dev->relbit);
	__set_bit(REL_Y, input_dev->relbit);
	__set_bit(REL_WHEEL, input_dev->relbit);
	__set_bit(REL_HWHEEL, input_dev->relbit);

	error = input_register_device(input_dev);
	if (error)
		goto err_free_mouse;

	printk(KERN_INFO "xpad: Mouse emulation initialized for %s\n", xpad->name);
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

	// Обработка левого стика для движения мыши
	int left_x = (__s16)le16_to_cpup((__le16 *)(data + 12));
	int left_y = ~(__s16)le16_to_cpup((__le16 *)(data + 14));
	
	// Обработка правого стика для прокрутки
	int right_x = (__s16)le16_to_cpup((__le16 *)(data + 16));
	int right_y = ~(__s16)le16_to_cpup((__le16 *)(data + 18));

	// Применяем мертвую зону и чувствительность
	int deadzone = mouse->deadzone;
	int sens = clamp(mouse_sensitivity, 1, 10);
	int scroll_sens = clamp(scroll_sensitivity, 1, 10);

	// Движение мыши левым стиком
	if (abs(left_x) > deadzone) {
		int move_x = (left_x * sens) / 32767;
		input_report_rel(mouse->dev, REL_X, move_x);
	}
	if (abs(left_y) > deadzone) {
		int move_y = (left_y * sens) / 32767;
		input_report_rel(mouse->dev, REL_Y, move_y);
	}

	// Прокрутка правым стиком
	if (abs(right_y) > deadzone) {
		int scroll = -(right_y * scroll_sens) / 32767;
		input_report_rel(mouse->dev, REL_WHEEL, scroll);
	}
	if (abs(right_x) > deadzone) {
		int hscroll = (right_x * scroll_sens) / 32767;
		input_report_rel(mouse->dev, REL_HWHEEL, hscroll);
	}

	// Обработка кнопок
	bool a_pressed = data[4];  // Кнопка A -> левая кнопка мыши
	bool b_pressed = data[5];  // Кнопка B -> правая кнопка мыши  
	bool x_pressed = data[6];  // Кнопка X -> средняя кнопка мыши

	// Левая кнопка мыши - A
	if (a_pressed != mouse->left_button) {
		input_report_key(mouse->dev, BTN_LEFT, a_pressed);
		mouse->left_button = a_pressed;
	}

	// Правая кнопка мыши - B
	if (b_pressed != mouse->right_button) {
		input_report_key(mouse->dev, BTN_RIGHT, b_pressed);
		mouse->right_button = b_pressed;
	}

	// Средняя кнопка мыши - X
	if (x_pressed != mouse->middle_button) {
		input_report_key(mouse->dev, BTN_MIDDLE, x_pressed);
		mouse->middle_button = x_pressed;
	}

	input_sync(mouse->dev);
}

/*
 *	xpadone_process_packet
 *
 *	Completes a request by converting the data into events for the
 *	input subsystem. This version is for the Xbox One controller.
 */
static void xpadone_process_packet(struct usb_xpad *xpad, u16 cmd, unsigned char *data)
{
	struct input_dev *dev = xpad->dev;
	bool do_sync = false;

	// Обновляем состояние мыши если включена эмуляция
	if ((xpad->mapping & MAP_MOUSE_EMULATION) && xpad->mouse && data[0] == GIP_CMD_INPUT) {
		xpad_update_mouse(xpad, data);
	}

	/* the xbox button has its own special report */
	if (data[0] == GIP_CMD_VIRTUAL_KEY) {
		if (data[1] == (0x10 | GIP_OPT_INTERNAL)) // GIP_OPT_ACK
			xpadone_ack_mode_report(xpad, data[2]);

		input_report_key(dev, BTN_MODE, data[4] & GENMASK(1, 0));
		input_sync(dev);
		do_sync = true;
	} else if (data[0] == GIP_CMD_INPUT) {
		/* menu/view buttons */
		input_report_key(dev, BTN_START,  data[4] & BIT(2));
		input_report_key(dev, BTN_SELECT, data[4] & BIT(3));
		if (xpad->mapping & MAP_SELECT_BUTTON)
			input_report_key(dev, KEY_RECORD, data[22] & BIT(0));

		/* buttons A,B,X,Y */
		input_report_key(dev, BTN_A,	data[4] & BIT(4));
		input_report_key(dev, BTN_B,	data[4] & BIT(5));
		input_report_key(dev, BTN_X,	data[4] & BIT(6));
		input_report_key(dev, BTN_Y,	data[4] & BIT(7));

		/* digital pad */
		input_report_abs(dev, ABS_HAT0X,
				!!(data[5] & 0x08) - !!(data[5] & 0x04));
		input_report_abs(dev, ABS_HAT0Y,
				!!(data[5] & 0x02) - !!(data[5] & 0x01));

		/* TL/TR */
		input_report_key(dev, BTN_TL,	data[5] & BIT(4));
		input_report_key(dev, BTN_TR,	data[5] & BIT(5));

		/* stick press left/right */
		input_report_key(dev, BTN_THUMBL, data[5] & BIT(6));
		input_report_key(dev, BTN_THUMBR, data[5] & BIT(7));

		/* left stick */
		input_report_abs(dev, ABS_X,
				(__s16) le16_to_cpup((__le16 *)(data + 10)));
		input_report_abs(dev, ABS_Y,
				~(__s16) le16_to_cpup((__le16 *)(data + 12)));

		/* right stick */
		input_report_abs(dev, ABS_RX,
				(__s16) le16_to_cpup((__le16 *)(data + 14)));
		input_report_abs(dev, ABS_RY,
				~(__s16) le16_to_cpup((__le16 *)(data + 16)));

		/* triggers left/right */
		input_report_abs(dev, ABS_Z,
				(__u16) le16_to_cpup((__le16 *)(data + 6)));
		input_report_abs(dev, ABS_RZ,
				(__u16) le16_to_cpup((__le16 *)(data + 8)));

		do_sync = true;
	}

	if (do_sync)
		input_sync(dev);
}

static void xpad_irq_in(struct urb *urb)
{
	struct usb_xpad *xpad = urb->context;
	struct device *dev = &xpad->intf->dev;
	int retval, status;

	status = urb->status;

	switch (status) {
	case 0:
		/* success */
		break;
	case -ECONNRESET:
	case -ENOENT:
	case -ESHUTDOWN:
		/* this urb is terminated, clean up */
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

/* Callers must hold xpad->odata_lock spinlock */
static bool xpad_prepare_next_out_packet(struct usb_xpad *xpad)
{
	struct xpad_output_packet *pkt, *packet = NULL;
	int i;

	for (i = 0; i < XPAD_NUM_OUT_PACKETS; i++) {
		if (++xpad->last_out_packet >= XPAD_NUM_OUT_PACKETS)
			xpad->last_out_packet = 0;

		pkt = &xpad->out_packets[xpad->last_out_packet];
		if (pkt->pending) {
			dev_dbg(&xpad->intf->dev,
				"%s - found pending output packet %d\n",
				__func__, xpad->last_out_packet);
			packet = pkt;
			break;
		}
	}

	if (packet) {
		memcpy(xpad->odata, packet->data, packet->len);
		xpad->irq_out->transfer_buffer_length = packet->len;
		packet->pending = false;
		return true;
	}

	return false;
}

/* Callers must hold xpad->odata_lock spinlock */
static int xpad_try_sending_next_out_packet(struct usb_xpad *xpad)
{
	int error;

	if (!xpad->irq_out_active && xpad_prepare_next_out_packet(xpad)) {
		usb_anchor_urb(xpad->irq_out, &xpad->irq_out_anchor);
		error = usb_submit_urb(xpad->irq_out, GFP_ATOMIC);
		if (error) {
			dev_err(&xpad->intf->dev,
				"%s - usb_submit_urb failed with result %d\n",
				__func__, error);
			usb_unanchor_urb(xpad->irq_out);
			return -EIO;
		}

		xpad->irq_out_active = true;
	}

	return 0;
}

static void xpad_irq_out(struct urb *urb)
{
	struct usb_xpad *xpad = urb->context;
	struct device *dev = &xpad->intf->dev;
	int status = urb->status;
	int error;
	unsigned long flags;

	spin_lock_irqsave(&xpad->odata_lock, flags);

	switch (status) {
	case 0:
		/* success */
		xpad->irq_out_active = xpad_prepare_next_out_packet(xpad);
		break;

	case -ECONNRESET:
	case -ENOENT:
	case -ESHUTDOWN:
		/* this urb is terminated, clean up */
		dev_dbg(dev, "%s - urb shutting down with status: %d\n",
			__func__, status);
		xpad->irq_out_active = false;
		break;

	default:
		dev_dbg(dev, "%s - nonzero urb status received: %d\n",
			__func__, status);
		break;
	}

	if (xpad->irq_out_active) {
		usb_anchor_urb(urb, &xpad->irq_out_anchor);
		error = usb_submit_urb(urb, GFP_ATOMIC);
		if (error) {
			dev_err(dev,
				"%s - usb_submit_urb failed with result %d\n",
				__func__, error);
			usb_unanchor_urb(urb);
			xpad->irq_out_active = false;
		}
	}

	spin_unlock_irqrestore(&xpad->odata_lock, flags);
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

static void xpad_stop_output(struct usb_xpad *xpad)
{
	if (!usb_wait_anchor_empty_timeout(&xpad->irq_out_anchor, 5000)) {
		dev_warn(&xpad->intf->dev,
			 "timed out waiting for output URB to complete, killing\n");
		usb_kill_anchored_urbs(&xpad->irq_out_anchor);
	}
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

	// Отправляем пакет инициализации
	spin_lock_irqsave(&xpad->odata_lock, flags);
	
	// Пакет включения для Xbox One
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

static void xpadone_ack_mode_report(struct usb_xpad *xpad, u8 seq_num)
{
	unsigned long flags;
	struct xpad_output_packet *packet = &xpad->out_packets[XPAD_OUT_CMD_IDX];
	static const u8 mode_report_ack[] = {
		GIP_CMD_ACK, GIP_OPT_INTERNAL, GIP_SEQ0, GIP_PL_LEN(9),
		0x00, GIP_CMD_VIRTUAL_KEY, GIP_OPT_INTERNAL, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00
	};

	spin_lock_irqsave(&xpad->odata_lock, flags);

	packet->len = sizeof(mode_report_ack);
	memcpy(packet->data, mode_report_ack, packet->len);
	packet->data[2] = seq_num;
	packet->pending = true;

	xpad->last_out_packet = -1;
	xpad_try_sending_next_out_packet(xpad);

	spin_unlock_irqrestore(&xpad->odata_lock, flags);
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

static int xpad_open(struct input_dev *dev)
{
	struct usb_xpad *xpad = input_get_drvdata(dev);
	return xpad_start_input(xpad);
}

static void xpad_close(struct input_dev *dev)
{
	struct usb_xpad *xpad = input_get_drvdata(dev);
	xpad_stop_input(xpad);
}

static void xpad_set_up_abs(struct input_dev *input_dev, signed short abs)
{
	switch (abs) {
	case ABS_X:
	case ABS_Y:
	case ABS_RX:
	case ABS_RY:
		input_set_abs_params(input_dev, abs, -32768, 32767, 16, 128);
		break;
	case ABS_Z:
	case ABS_RZ:
		input_set_abs_params(input_dev, abs, 0, 1023, 0, 0);
		break;
	case ABS_HAT0X:
	case ABS_HAT0Y:
		input_set_abs_params(input_dev, abs, -1, 1, 0, 0);
		break;
	default:
		input_set_abs_params(input_dev, abs, 0, 0, 0, 0);
		break;
	}
}

static void xpad_deinit_input(struct usb_xpad *xpad)
{
	if (xpad->input_created) {
		xpad->input_created = false;
		input_unregister_device(xpad->dev);
	}
	
	xpad_deinit_mouse(xpad);
}

static int xpad_init_input(struct usb_xpad *xpad)
{
	struct input_dev *input_dev;
	int i, error;

	input_dev = input_allocate_device();
	if (!input_dev)
		return -ENOMEM;

	xpad->dev = input_dev;
	input_dev->name = xpad->name;
	input_dev->phys = xpad->phys;
	usb_to_input_id(xpad->udev, &input_dev->id);
	input_dev->dev.parent = &xpad->intf->dev;

	input_set_drvdata(input_dev, xpad);

	input_dev->open = xpad_open;
	input_dev->close = xpad_close;

	/* set up axes */
	for (i = 0; xpad_abs[i] >= 0; i++)
		xpad_set_up_abs(input_dev, xpad_abs[i]);

	/* set up standard buttons */
	for (i = 0; xpad_common_btn[i] >= 0; i++)
		input_set_capability(input_dev, EV_KEY, xpad_common_btn[i]);

	/* set up model-specific ones */
	for (i = 0; xpad360_btn[i] >= 0; i++)
		input_set_capability(input_dev, EV_KEY, xpad360_btn[i]);

	if (xpad->mapping & MAP_SELECT_BUTTON)
		input_set_capability(input_dev, EV_KEY, KEY_RECORD);

	/* digital pad axes */
	for (i = 0; xpad_abs_pad[i] >= 0; i++)
		xpad_set_up_abs(input_dev, xpad_abs_pad[i]);

	/* triggers axes */
	for (i = 0; xpad_abs_triggers[i] >= 0; i++)
		xpad_set_up_abs(input_dev, xpad_abs_triggers[i]);

	error = input_register_device(xpad->dev);
	if (error)
		goto err_free_input;

	xpad->input_created = true;

	// Инициализация мыши
	error = xpad_init_mouse(xpad);
	if (error) {
		dev_warn(&xpad->intf->dev,
			 "unable to initialize mouse emulation: %d\n", error);
	}

	return 0;

err_free_input:
	input_free_device(input_dev);
	return error;
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

	// Применяем настройку эмуляции мыши
	if (mouse_emulation)
		xpad->mapping |= MAP_MOUSE_EMULATION;

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

	error = xpad_init_input(xpad);
	if (error)
		goto err_deinit_output;

	return 0;

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

	xpad_deinit_input(xpad);
	xpad_stop_output(xpad);
	xpad_deinit_output(xpad);
	usb_free_urb(xpad->irq_in);
	usb_free_coherent(xpad->udev, XPAD_PKT_LEN, xpad->idata, xpad->idata_dma);
	kfree(xpad);

	usb_set_intfdata(intf, NULL);
}

static int xpad_suspend(struct usb_interface *intf, pm_message_t message)
{
	struct usb_xpad *xpad = usb_get_intfdata(intf);
	struct input_dev *input = xpad->dev;

	mutex_lock(&input->mutex);
	if (input->users)
		xpad_stop_input(xpad);
	mutex_unlock(&input->mutex);

	xpad_stop_output(xpad);
	return 0;
}

static int xpad_resume(struct usb_interface *intf)
{
	struct usb_xpad *xpad = usb_get_intfdata(intf);
	struct input_dev *input = xpad->dev;
	int retval = 0;

	mutex_lock(&input->mutex);
	if (input->users) {
		retval = xpad_start_input(xpad);
	} else {
		retval = xpad_start_xbox_one(xpad);
	}
	mutex_unlock(&input->mutex);

	return retval;
}

static struct usb_driver xpad_driver = {
	.name		= "xpad",
	.probe		= xpad_probe,
	.disconnect	= xpad_disconnect,
	.suspend	= xpad_suspend,
	.resume		= xpad_resume,
	.id_table	= xpad_table,
};

module_usb_driver(xpad_driver);

MODULE_AUTHOR("Marko Friedemann <mfr@bmx-chemnitz.de>");
MODULE_DESCRIPTION("Xbox pad driver");
MODULE_LICENSE("GPL");