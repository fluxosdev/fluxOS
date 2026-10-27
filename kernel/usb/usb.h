/*
 * USB: the core (usb.c) between the host controller drivers (uhci.c,
 * ohci.c, ehci.c, xhci.c) and the device drivers (usbhub.c, usbhid.c,
 * usbmsc.c).
 *
 * Everything here runs holding usb_lock, a spinlock (so with interrupts
 * off): a controller's completions are handled from its interrupt, from
 * the timer tick, or by a waiting caller polling, never two at once.
 * Whatever waits lets it go meanwhile (usb_wait, usb_mdelay, and the
 * storage driver around the VFS lock, which comes first in lock order),
 * so other USB work may run there, as it always could while one slept.
 */
#ifndef USB_H
#define USB_H

#include <stdint.h>

#define USB_SPEED_LOW   0
#define USB_SPEED_FULL  1
#define USB_SPEED_HIGH  2
#define USB_SPEED_SUPER 3

/* Endpoint types, as in bmAttributes */
#define USB_EP_CONTROL 0
#define USB_EP_ISO     1
#define USB_EP_BULK    2
#define USB_EP_INTR    3

#define USB_DIR_IN 0x80

/* bmRequestType */
#define USB_RT_IN        0x80
#define USB_RT_CLASS     0x20
#define USB_RT_INTERFACE 0x01
#define USB_RT_ENDPOINT  0x02
#define USB_RT_OTHER     0x03           /* hub ports */

/* Standard requests */
#define USB_REQ_GET_STATUS        0
#define USB_REQ_CLEAR_FEATURE     1
#define USB_REQ_SET_FEATURE       3
#define USB_REQ_SET_ADDRESS       5
#define USB_REQ_GET_DESCRIPTOR    6
#define USB_REQ_SET_CONFIGURATION 9
#define USB_REQ_SET_INTERFACE     11

/* Descriptor types */
#define USB_DT_DEVICE    1
#define USB_DT_CONFIG    2
#define USB_DT_STRING    3
#define USB_DT_INTERFACE 4
#define USB_DT_ENDPOINT  5
#define USB_DT_HID       0x21
#define USB_DT_REPORT    0x22
#define USB_DT_HUB       0x29
#define USB_DT_SS_HUB    0x2A
#define USB_DT_SS_EP_COMP 0x30

/* Classes */
#define USB_CLASS_HID     3
#define USB_CLASS_STORAGE 8
#define USB_CLASS_HUB     9

#define USB_PENDING   1                 /* usb_xfer.status while in flight */
#define USB_MAX_XFER  65536u            /* bytes per transfer */

struct usb_device_desc {
    uint8_t  bLength, bDescriptorType;
    uint16_t bcdUSB;
    uint8_t  bDeviceClass, bDeviceSubClass, bDeviceProtocol, bMaxPacketSize0;
    uint16_t idVendor, idProduct, bcdDevice;
    uint8_t  iManufacturer, iProduct, iSerialNumber, bNumConfigurations;
} __attribute__((packed));

struct usb_config_desc {
    uint8_t  bLength, bDescriptorType;
    uint16_t wTotalLength;
    uint8_t  bNumInterfaces, bConfigurationValue, iConfiguration, bmAttributes, bMaxPower;
} __attribute__((packed));

struct usb_iface_desc {
    uint8_t bLength, bDescriptorType;
    uint8_t bInterfaceNumber, bAlternateSetting, bNumEndpoints;
    uint8_t bInterfaceClass, bInterfaceSubClass, bInterfaceProtocol, iInterface;
} __attribute__((packed));

struct usb_ep_desc {
    uint8_t  bLength, bDescriptorType;
    uint8_t  bEndpointAddress, bmAttributes;
    uint16_t wMaxPacketSize;
    uint8_t  bInterval;
} __attribute__((packed));

struct usb_ep {
    uint8_t addr;                       /* bEndpointAddress: number | USB_DIR_IN */
    uint8_t type;                       /* USB_EP_* */
    uint16_t maxpacket;                 /* bytes per packet */
    uint8_t mult;                       /* high-speed periodic: extra transactions per microframe */
    uint8_t interval;                   /* bInterval */
    uint8_t burst;                      /* SuperSpeed: extra packets per burst */
    uint8_t toggle;                     /* data toggle, for controllers that leave it to us */
    void *hcpriv;                       /* the controller's state for this endpoint */
};

struct usb_dev;
struct usb_xfer;

/*
 * A transfer. The buffer must be kernel memory below KERNEL_SPACE_END
 * (identity-mapped, so the controller can reach it by DMA). Only one
 * transfer per endpoint may be in flight.
 */
struct usb_xfer {
    struct usb_dev *dev;
    struct usb_ep *ep;
    uint8_t setup[8];                   /* control transfers: the SETUP packet */
    void *buf;
    uint32_t len;
    uint32_t actual;                    /* bytes moved */
    volatile int status;                /* USB_PENDING, then 0, -EPIPE (stall), -EIO, -ETIMEDOUT, -ENODEV */
    /* Called when the transfer ends, from interrupt context; may resubmit.
     * Without it the waiter in usb_wait() is woken instead. */
    void (*complete)(struct usb_xfer *x);
    void *priv;
    void *hcpriv;                       /* the controller's */
    struct usb_xfer *hcnext;
};

#define USB_MAX_IFACES 8
#define USB_MAX_PORTS  31

struct usb_driver;
struct usb_hc;

struct usb_iface {
    const struct usb_driver *drv;
    void *priv;
    uint8_t num, cls, sub, proto;
};

struct usb_hub;

struct usb_dev {
    struct usb_hc *hc;
    struct usb_dev *parent;             /* the hub it hangs off; NULL on a root port */
    int port;                           /* the port on that hub, from 1 */
    int root_port;                      /* the root hub port its chain starts at, from 1 */
    int speed;
    int addr;
    int depth;                          /* hubs above it */
    uint32_t route;                     /* xHCI route string */
    struct usb_dev *tt_hub;             /* low/full speed behind a high-speed hub: its TT */
    int tt_port;
    int gone;                           /* unplugged: transfers fail */
    struct usb_device_desc desc;
    uint8_t *config;                    /* the whole configuration descriptor */
    uint32_t config_len;
    char manufacturer[32];
    char product[48];
    struct usb_ep ep0;
    struct usb_ep eps[32];              /* by usb_ep_index() */
    struct usb_iface ifaces[USB_MAX_IFACES];
    int nifaces;
    struct usb_hub *hub;                /* when it is a hub */
    void *hcpriv;                       /* xHCI: the device slot */
};

static inline int usb_ep_index(uint8_t addr)
{
    return (addr & 0x0F) | ((addr & USB_DIR_IN) ? 16 : 0);
}

/* A class driver: offered each interface (alternate setting 0). */
struct usb_driver {
    const char *name;
    /* desc/len: the interface descriptor and those after it, up to the next
     * interface. Return 0 and set *priv to take the interface. */
    int (*probe)(struct usb_dev *dev, const struct usb_iface_desc *ifd,
                 const uint8_t *desc, uint32_t len, void **priv);
    /* The device is gone (dev->gone is set) or being shut down. */
    void (*disconnect)(struct usb_dev *dev, void *priv);
};

/* Port status, from usb_hub.port_status */
#define PORT_CONNECT 0x1
#define PORT_ENABLE  0x2
#define PORT_CHANGE  0x4                /* the connection changed since the last look */

/* A hub: a controller's root hub, or a hub device (usbhub.c). */
struct usb_hub {
    struct usb_hc *hc;
    struct usb_dev *dev;                /* NULL for a root hub */
    int nports;
    int (*port_status)(struct usb_hub *h, int port);
    /* Reset and enable the port: returns the device's speed, or < 0 (an
     * EHCI port also gives its low/full speed devices away this way). */
    int (*port_reset)(struct usb_hub *h, int port);
    void (*port_disable)(struct usb_hub *h, int port);
    struct usb_dev *child[USB_MAX_PORTS + 1];
    uint8_t failed[USB_MAX_PORTS + 1];  /* would not enumerate: wait for a replug */
    volatile int changed;               /* a hub device reported a port change */
    void *priv;
};

struct usb_hc_ops {
    int (*submit)(struct usb_hc *hc, struct usb_xfer *x);
    /* Take a transfer back; once this returns the controller no longer
     * touches it or its buffer. Process context. */
    void (*cancel)(struct usb_hc *hc, struct usb_xfer *x);
    void (*poll)(struct usb_hc *hc);    /* finish completed transfers */
    /* xHCI: give the device a slot and an address (it sends SET_ADDRESS
     * itself); dev_update after ep0's packet size changed. */
    int (*dev_init)(struct usb_hc *hc, struct usb_dev *dev);
    int (*dev_update)(struct usb_hc *hc, struct usb_dev *dev);
    /* xHCI: the device's endpoints are known (after SET_CONFIGURATION);
     * dev_hub: it is a hub with this many ports. */
    int (*dev_configure)(struct usb_hc *hc, struct usb_dev *dev);
    int (*dev_hub)(struct usb_hc *hc, struct usb_dev *dev, int nports, int ttt, int mtt);
    /* Forget the device's endpoints (and slot); no transfers are pending. */
    void (*dev_free)(struct usb_hc *hc, struct usb_dev *dev);
    /* After a halt was cleared: the endpoint starts again with DATA0. */
    void (*ep_reset)(struct usb_hc *hc, struct usb_dev *dev, struct usb_ep *ep);
    /* Suspend to RAM, with no devices left: stop; then, the power having
     * gone, take the controller from the BIOS and set it up again. */
    void (*suspend)(struct usb_hc *hc);
    void (*resume)(struct usb_hc *hc);
};

struct usb_hc {
    const char *name;                   /* "UHCI", "OHCI", "EHCI", "xHCI" */
    const struct usb_hc_ops *ops;
    struct usb_hub root;
    int irq;                            /* -1: polled from the timer tick */
    int sets_address;                   /* dev_init assigns the address (xHCI) */
    uint8_t addr_used[16];              /* bitmap of USB addresses */
    void *priv;
    struct usb_hc *next;
};

/* ---- for host controller drivers ---- */
void usb_register_hc(struct usb_hc *hc);
/* Memory for controller data structures: zeroed, aligned to its size
 * rounded up to a power of two (32 to 4096), never crossing a page. */
void *usb_dma_alloc(uint32_t size);
void usb_dma_free(void *p, uint32_t size);
/* Finish a transfer from a controller's poll routine. */
void usb_xfer_done(struct usb_xfer *x, int status, uint32_t actual);
int usb_in_process(void);               /* a process that may sleep */
int usb_can_wait(void);                 /* not inside a completion callback */

/* ---- for device drivers ---- */
int usb_submit(struct usb_xfer *x);
int usb_wait(struct usb_xfer *x, uint32_t timeout_ms);
void usb_cancel(struct usb_xfer *x);
/* Returns the bytes moved, or < 0. data must be kernel memory. */
int usb_control(struct usb_dev *dev, uint8_t reqtype, uint8_t req, uint16_t value,
                uint16_t index, void *data, uint16_t len);
int usb_transfer(struct usb_dev *dev, struct usb_ep *ep, void *buf, uint32_t len,
                 uint32_t timeout_ms);
int usb_clear_halt(struct usb_dev *dev, struct usb_ep *ep);
/* The first endpoint of this type and direction in an interface's descriptors. */
struct usb_ep *usb_find_ep(struct usb_dev *dev, const uint8_t *desc, uint32_t len,
                           int type, int in);
/* Wake the hub thread: a hub reported a change, or a driver needs it. */
void usb_kick(void);

/* hub devices (usbhub.c) */
extern const struct usb_driver usbhub_driver;
void usb_hub_scan(struct usb_hub *h, int all);
void usbhub_resubmit(struct usb_hub *h);

/* the drivers */
extern const struct usb_driver usbhid_driver;
extern const struct usb_driver usbmsc_driver;
void usbhid_tick(void);
void usbhid_service(void);

/* the controllers */
void uhci_probe(void);
void ohci_probe(void);
void ehci_probe(void);
void xhci_probe(void);

/* ---- locking (usb.c) ---- */
extern spinlock_t usb_lock;
int usb_enter(void);                    /* take usb_lock unless held: 1 if taken */
void usb_leave(int taken);
void usb_mdelay(uint32_t ms);           /* mdelay(), letting usb_lock go meanwhile */

#endif
