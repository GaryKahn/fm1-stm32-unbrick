// fm1-stm32-unbrick - USB host for the JieLi UBOOT, on the STM32F407's OTG FS controller through TinyUSB.
//
// Takes the place of fm1-esp32-unbrick main/host.c (MIT) and FM-1 Transporter src/pio_host.c (MIT, Leo
// Kuroshita): the same mount -> descriptors -> MSC bulk pair -> (1.5 s) -> GET MAX LUN -> INQUIRY x30
// sequence and the same Bulk-Only commands. No TinyUSB class driver is enabled: nothing sends SCSI commands
// on its own; the MSC bulk endpoints are opened here and driven with tuh_edpt_xfer().

#include "host.h"

#include <string.h>

#include "compat.h"
#include "host/hcd.h"
#include "jieli_uboot.h"
#include "log.h"
#include "tusb.h"

#define FM1_VID 0x4C4A
#define FM1_PID 0x8057
#define FM1_V15_PID 0xC755
#define XFER_TIMEOUT_MS 2000
#define FIRST_CBW_DELAY_MS 1500     // macOS had the device for seconds before its first CBW
#define INQUIRY_RETRY_MS 300
#define INQUIRY_TRIES 30
#define ATTACH_STATUS_MS 2000
#define XFER_BUF (4096 + 64)        // the largest READ_FLASH, rounded up to the packet size

#define CBW_SIGNATURE 0x43425355u
#define CSW_SIGNATURE 0x53425355u

static volatile uint8_t new_addr;           // set by tuh_mount_cb, taken by fm1_host_task
static volatile bool gone;
static uint8_t dev;                         // device address, 0 = none
static uint8_t ep_in, ep_out, msc_itf;
static uint16_t mps_in = 64;
static bool uboot_ready, is_v15;
static uint32_t cbw_tag = 1;
static uint64_t host_start_us, mount_us, last_status_us, next_inquiry_us;
static int inquiry_tries;

static uint8_t x_out[XFER_BUF] __attribute__((aligned(4)));
static uint8_t x_in[XFER_BUF] __attribute__((aligned(4)));
static uint8_t desc_buf[256] __attribute__((aligned(4)));
static tusb_desc_device_t desc_dev __attribute__((aligned(4)));

static void put_le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static uint32_t get_le32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

//--------------------------------------------------------------------+
// TinyUSB callbacks and interrupt
//--------------------------------------------------------------------+

void tuh_mount_cb(uint8_t daddr) { new_addr = daddr; }
void tuh_umount_cb(uint8_t daddr) { (void)daddr; gone = true; }

void OTG_FS_IRQHandler(void) { tuh_int_handler(0, true); }

void fm1_host_start(void) {
    // PA11 (D-) / PA12 (D+) to AF10 (OTG_FS), very high speed; PA9 (VBUS) and PA10 (ID) stay GPIO inputs:
    // the core is forced into host mode and VBUS is not sensed (the FM-1 runs on its battery).
    GPIOA->OSPEEDR |= (3u << (11 * 2)) | (3u << (12 * 2));
    GPIOA->AFR[1] = (GPIOA->AFR[1] & ~((0xFu << ((11 - 8) * 4)) | (0xFu << ((12 - 8) * 4)))) |
                    (10u << ((11 - 8) * 4)) | (10u << ((12 - 8) * 4));
    GPIOA->MODER = (GPIOA->MODER & ~((3u << (11 * 2)) | (3u << (12 * 2)))) | (2u << (11 * 2)) | (2u << (12 * 2));

    RCC->AHB2ENR |= RCC_AHB2ENR_OTGFSEN;
    (void)RCC->AHB2ENR;
    NVIC_SetPriority(OTG_FS_IRQn, 5);

    tusb_rhport_init_t host_init = {.role = TUSB_ROLE_HOST, .speed = TUSB_SPEED_FULL};
    tusb_init(0, &host_init);
    host_start_us = last_status_us = time_us_64();
}

uint64_t fm1_host_started_us(void) {
    return host_start_us;
}

//--------------------------------------------------------------------+
// blocking transfers
//--------------------------------------------------------------------+

static volatile uint32_t xfer_seq;
static volatile bool xfer_done;
static volatile xfer_result_t xfer_res;
static volatile uint32_t xfer_len;

static void xfer_cb(tuh_xfer_t *x) {
    if (x->user_data != xfer_seq) return;   // a late callback from an aborted transfer
    xfer_res = x->result;
    xfer_len = x->actual_len;
    xfer_done = true;
}

static void clear_halt(uint8_t ep) {
    tusb_control_request_t req = {
        .bmRequestType_bit = {.recipient = TUSB_REQ_RCPT_ENDPOINT, .type = TUSB_REQ_TYPE_STANDARD,
                              .direction = TUSB_DIR_OUT},
        .bRequest = TUSB_REQ_CLEAR_FEATURE,
        .wValue = TUSB_REQ_FEATURE_EDPT_HALT,
        .wIndex = ep,
        .wLength = 0,
    };
    xfer_result_t r = XFER_RESULT_INVALID;
    tuh_xfer_t x = {.daddr = dev, .ep_addr = 0, .setup = &req, .buffer = NULL, .complete_cb = NULL,
                    .user_data = (uintptr_t)&r};
    tuh_control_xfer(&x);
    hcd_edpt_clear_stall(0, dev, ep);
}

static bool xfer_sync(uint8_t ep, uint8_t *buf, uint32_t n, uint32_t *actual) {
    uint32_t seq = ++xfer_seq;
    xfer_done = false;
    tuh_xfer_t x = {.daddr = dev, .ep_addr = ep, .buflen = n, .buffer = buf, .complete_cb = xfer_cb,
                    .user_data = seq};
    if (!tuh_edpt_xfer(&x)) {
        dlog("HOST: EP %02X submit failed", ep);
        return false;
    }
    uint64_t deadline = time_us_64() + XFER_TIMEOUT_MS * 1000ull;
    while (!xfer_done) {
        tuh_task();
        if (gone || !dev) {
            dlog("HOST: EP %02X: device gone", ep);
            return false;
        }
        if (time_us_64() > deadline) {
            dlog("HOST: EP %02X timeout", ep);
            tuh_edpt_abort_xfer(dev, ep);
            return false;
        }
    }
    if (actual) *actual = xfer_len;
    if (xfer_res != XFER_RESULT_SUCCESS) {
        dlog("HOST: EP %02X status %d", ep, (int)xfer_res);
        if (xfer_res == XFER_RESULT_STALLED) clear_halt(ep);
        return false;
    }
    return true;
}

static bool ctrl_in_sync(uint8_t type, uint8_t req, uint16_t value, uint16_t index, uint8_t *buf, uint16_t len) {
    static uint8_t data[64] __attribute__((aligned(4)));
    tusb_control_request_t setup = {.bmRequestType = type, .bRequest = req, .wValue = value, .wIndex = index,
                                    .wLength = len};
    xfer_result_t r = XFER_RESULT_INVALID;
    tuh_xfer_t x = {.daddr = dev, .ep_addr = 0, .setup = &setup, .buffer = data, .complete_cb = NULL,
                    .user_data = (uintptr_t)&r};
    if (!tuh_control_xfer(&x) || r != XFER_RESULT_SUCCESS) return false;
    memcpy(buf, data, len);
    return true;
}

static uint32_t round_up(uint32_t n, uint32_t mps) {
    return (n + mps - 1) / mps * mps;
}

// One Bulk-Only Transport command. len == 0 means no data stage.
static bool bot_cmd(const uint8_t *cdb, uint8_t cdb_len, bool in, void *buf, uint16_t len, uint32_t *got) {
    uint8_t *c = x_out;
    uint32_t tag = cbw_tag++;
    memset(c, 0, 31);
    put_le32(c, CBW_SIGNATURE);
    put_le32(c + 4, tag);
    put_le32(c + 8, len);
    c[12] = in ? 0x80 : 0;
    c[13] = 0;                      // LUN
    c[14] = cdb_len;
    memcpy(c + 15, cdb, cdb_len);
    if (!xfer_sync(ep_out, x_out, 31, NULL)) {
        dlog("HOST: CBW not accepted");
        return false;
    }
    if (len) {
        if (in) {
            uint32_t n = 0;
            if (!xfer_sync(ep_in, x_in, round_up(len, mps_in), &n)) return false;
            if (n > len) n = len;
            memcpy(buf, x_in, n);
            if (got) *got = n;
        } else {
            memcpy(x_out, buf, len);
            if (!xfer_sync(ep_out, x_out, len, NULL)) return false;
        }
    }
    uint32_t n = 0;
    if (!xfer_sync(ep_in, x_in, mps_in, &n) || n < 13) {
        dlog("HOST: no CSW (%lu bytes)", (unsigned long)n);
        return false;
    }
    const uint8_t *s = x_in;
    if (get_le32(s) != CSW_SIGNATURE || get_le32(s + 4) != tag) {
        dlog("HOST: bad CSW sig=%08lx tag=%lu", (unsigned long)get_le32(s), (unsigned long)get_le32(s + 4));
        return false;
    }
    if (s[12] != 0) {
        dlog("HOST: CSW status %u residue %lu", s[12], (unsigned long)get_le32(s + 8));
        return false;
    }
    return true;
}

bool fm1_host_bot(const uint8_t *cdb, uint8_t cdb_len, bool in, void *buf, uint16_t len, uint32_t *got) {
    if (!dev || !uboot_ready) return false;
    return bot_cmd(cdb, cdb_len, in, buf, len, got);
}

bool fm1_host_uboot_ready(void) {
    return dev && uboot_ready;
}

bool fm1_host_v15_mounted(void) {
    return dev && is_v15;
}

bool fm1_host_softkey(void) {
    return false;
}

//--------------------------------------------------------------------+
// mount, probe
//--------------------------------------------------------------------+

static bool probe_descriptors(void) {
    if (tuh_descriptor_get_device_sync(dev, &desc_dev, sizeof(desc_dev)) != XFER_RESULT_SUCCESS) {
        dlog("HOST: device descriptor read failed");
        return false;
    }
    const tusb_desc_device_t *dd = &desc_dev;
    dlog("DEVICE DESCRIPTOR OK");
    dlog("  VID=%04X PID=%04X bcdDevice=%04X bcdUSB=%04X", dd->idVendor, dd->idProduct, dd->bcdDevice, dd->bcdUSB);
    is_v15 = dd->idVendor == FM1_VID && dd->idProduct == FM1_V15_PID;
    if (is_v15) dlog("  stock FM-1 application: no soft key on this board; power-cycle the FM-1 under the key");
    else if (dd->idVendor != FM1_VID || dd->idProduct != FM1_PID)
        dlog("  not the JieLi UBOOT (%04X:%04X expected)", FM1_VID, FM1_PID);

    if (tuh_descriptor_get_configuration_sync(dev, 0, desc_buf, sizeof(desc_buf)) != XFER_RESULT_SUCCESS) {
        dlog("HOST: configuration descriptor read failed");
        return false;
    }
    const uint8_t *p = desc_buf;
    uint16_t total = (uint16_t)(p[2] | p[3] << 8);
    if (total > sizeof(desc_buf)) total = sizeof(desc_buf);
    dlog("CONFIGURATION DESCRIPTOR (%u bytes), bConfigurationValue=%u", total, p[5]);
    bool in_msc = false;
    const tusb_desc_endpoint_t *d_in = NULL, *d_out = NULL;
    ep_in = ep_out = 0;
    for (uint16_t off = 0; off + 2 <= total && p[off] >= 2; off += p[off]) {
        const uint8_t *d = &p[off];
        if (d[1] == TUSB_DESC_INTERFACE) {
            const tusb_desc_interface_t *itf = (const void *)d;
            dlog("  interface %u alt %u: class=%02X/%02X/%02X endpoints=%u", itf->bInterfaceNumber,
                 itf->bAlternateSetting, itf->bInterfaceClass, itf->bInterfaceSubClass, itf->bInterfaceProtocol,
                 itf->bNumEndpoints);
            in_msc = itf->bInterfaceClass == TUSB_CLASS_MSC && itf->bAlternateSetting == 0;
            if (in_msc) msc_itf = itf->bInterfaceNumber;
        } else if (d[1] == TUSB_DESC_ENDPOINT) {
            const tusb_desc_endpoint_t *ep = (const void *)d;
            uint16_t mps = (uint16_t)(d[4] | d[5] << 8) & 0x7FF;
            dlog("    endpoint %02X attr=%02X maxpacket=%u", ep->bEndpointAddress, d[3], mps);
            if (in_msc && (d[3] & 3) == TUSB_XFER_BULK) {
                if (ep->bEndpointAddress & 0x80) {
                    ep_in = ep->bEndpointAddress;
                    mps_in = mps;
                    d_in = ep;
                } else {
                    ep_out = ep->bEndpointAddress;
                    d_out = ep;
                }
            }
        }
    }
    if (!ep_in || !ep_out) {
        dlog("HOST: no MSC bulk IN/OUT pair found");
        return false;
    }
    if (!tuh_edpt_open(dev, d_out) || !tuh_edpt_open(dev, d_in)) {
        dlog("HOST: opening the bulk endpoints failed");
        return false;
    }
    dlog("HOST: MSC bulk OUT=%02X IN=%02X (max packet %u)", ep_out, ep_in, mps_in);
    return true;
}

static bool probe_inquiry(void) {
    static const uint8_t inquiry[6] = {0x12, 0, 0, 0, 36, 0};
    uint8_t data[36];
    uint32_t got = 0;
    if (!bot_cmd(inquiry, sizeof(inquiry), true, data, 36, &got) || got < 36) {
        dlog("INQUIRY failed (got %lu bytes)", (unsigned long)got);
        return false;
    }
    char vendor[9], product[17], rev[5];
    memcpy(vendor, &data[8], 8);
    vendor[8] = 0;
    memcpy(product, &data[16], 16);
    product[16] = 0;
    memcpy(rev, &data[32], 4);
    rev[4] = 0;
    dlog("INQUIRY OK: vendor=\"%s\" product=\"%s\" rev=\"%s\"", vendor, product, rev);
    return true;
}

static void unmount(void) {
    dlog("HOST: device detached (%.1f ms after mount)", (time_us_64() - mount_us) / 1000.0);
    dev = 0;
    uboot_ready = is_v15 = false;
    inquiry_tries = 0;
    led(LED_BLUE, false);
    jl_reset();
}

void fm1_host_task(void) {
    if (!host_start_us) return;
    uint64_t now = time_us_64();

    if (gone && dev) {
        gone = false;
        unmount();
    }
    uint8_t a = new_addr;
    if (a && !dev) {
        new_addr = 0;
        gone = false;
        dev = a;
        mount_us = now;
        dlog("HOST: device mounted, addr=%u (%.1f ms after host start)", a, (now - host_start_us) / 1000.0);
        jl_reset();
        if (probe_descriptors()) {
            inquiry_tries = INQUIRY_TRIES;
            next_inquiry_us = mount_us + FIRST_CBW_DELAY_MS * 1000ull;
            dlog("HOST: first CBW in %d ms", FIRST_CBW_DELAY_MS);
        }
    }

    if (inquiry_tries > 0 && dev && now >= next_inquiry_us) {
        if (inquiry_tries == INQUIRY_TRIES) {
            uint8_t lun = 0;
            if (ctrl_in_sync(0xA1, 0xFE, 0, msc_itf, &lun, 1)) dlog("GET MAX LUN: %u", lun);
        }
        inquiry_tries--;
        dlog("INQUIRY attempt %d (%.1f ms after mount)", INQUIRY_TRIES - inquiry_tries,
             (time_us_64() - mount_us) / 1000.0);
        if (probe_inquiry()) {
            inquiry_tries = 0;
#ifdef FM1_SELFTEST
            led(LED_BLUE, true);
            dlog("SELFTEST PASSED: enumerated, MSC bulk pair opened, Bulk-Only INQUIRY answered.");
            return;                 // never marks a non-FM-1 device ready: no JieLi command is ever sent to it
#endif
            uboot_ready = true;
            led(LED_BLUE, true);    // blue: the FM-1 is in UBOOT and answering
            dlog("UBOOT READY: enumerated and answered INQUIRY.");
        } else if (inquiry_tries == 0) {
            dlog("UBOOT: INQUIRY never accepted.");
        } else {
            next_inquiry_us = time_us_64() + INQUIRY_RETRY_MS * 1000ull;
        }
    }

    if (!dev && now - last_status_us > ATTACH_STATUS_MS * 1000ull) {
        // HPRT: bit 0 = connect status, bits 11:10 = line state (D+ bit 10, D- bit 11), bit 12 = port power
        uint32_t hprt = *(volatile uint32_t *)(USB_OTG_FS_PERIPH_BASE + USB_OTG_HOST_PORT_BASE);
        dlog("HOST: no device mounted yet (%.1f s after host start) HPRT=%08lX conn=%lu D+=%lu D-=%lu",
             (now - host_start_us) / 1e6, (unsigned long)hprt, (unsigned long)(hprt & 1),
             (unsigned long)((hprt >> 10) & 1), (unsigned long)((hprt >> 11) & 1));
        last_status_us = now;
    }
}
