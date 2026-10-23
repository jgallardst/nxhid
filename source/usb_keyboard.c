#include "usb_keyboard.h"
#include <string.h>

#ifndef NXHID_USB_VID
// pid.codes test identity: private testing only, not a unique production allocation.
#define NXHID_USB_VID 0x1209
#endif
#ifndef NXHID_USB_PID
#define NXHID_USB_PID 0x0001
#endif

// Boot-compatible keyboard: modifiers, reserved byte, six key usages; LED output.
static const u8 report_descriptor[] = {
    0x05,0x01, 0x09,0x06, 0xA1,0x01, 0x05,0x07,
    0x19,0xE0, 0x29,0xE7, 0x15,0x00, 0x25,0x01,
    0x75,0x01, 0x95,0x08, 0x81,0x02,
    0x95,0x01, 0x75,0x08, 0x81,0x01,
    0x95,0x05, 0x75,0x01, 0x05,0x08,
    0x19,0x01, 0x29,0x05, 0x91,0x02,
    0x95,0x01, 0x75,0x03, 0x91,0x01,
    0x95,0x06, 0x75,0x08, 0x15,0x00, 0x25,0x73,
    0x05,0x07, 0x19,0x00, 0x29,0x73, 0x81,0x00, 0xC0
};
static const u8 hid_descriptor[] = {
    9, USB_DT_HID, 0x11, 0x01, 0, 1, USB_DT_REPORT,
    sizeof(report_descriptor), 0
};
static UsbDsInterface *interface;
static UsbDsEndpoint *endpoint;
static bool initialized, ready, was_configured, was_ready, descriptor_valid;
// USB DMA buffers must remain page aligned and alive until session teardown.
static u8 control_buffer[0x1000] __attribute__((aligned(0x1000)));
static u8 input_buffer[0x1000] __attribute__((aligned(0x1000)));
static u8 last_report[8], idle_rate, protocol, leds;
static u64 last_sent, configured_at, control_started;
static unsigned long host_resets, soft_errors;
static Result last_error;
static UsbState state_now = UsbState_Detached;

// Control transfers are asynchronous: a new SETUP legally supersedes an earlier
// request, so stages retire through completion events and a superseded transfer
// is dropped in silence. Only a transfer stuck past the deadline is stalled;
// stalling on every host abort sends the Switch 2 into a reset loop.
static bool ctrl_in_pending, ctrl_out_pending, led_data_pending, status_out_after_in;
static bool control_read, descriptor_request, descriptor_in_done, descriptor_out_done;
static u32 ctrl_in_id, ctrl_out_id, ctrl_in_expected, ctrl_out_expected;

enum {
    CONTROL_TIMEOUT_MS = 1500,
    // Reports block no longer than the sender's reply timeout; a report the
    // host cannot poll in time is cancelled and retried by the server.
    REPORT_TIMEOUT_MS = 1500,
    EVENT_POLL_NS = 1000000,
    // The Switch 2 host re-enumerates or suspends right after configuring;
    // only trust Configured once the report descriptor round-trip is done and settled.
    CONFIGURED_SETTLE_MS = 300,
};

static u64 milliseconds(void) { return armTicksToNs(armGetSystemTick()) / 1000000; }
static Result io_error(void) { return MAKERESULT(Module_Libnx, LibnxError_IoError); }

// Runtime service and kernel errors (0xfa01 invalid-state during host resets,
// session hiccups, IPC races) are transient: record for diagnostics and keep
// polling. Only initialization failures end the session.
static void note(Result rc) {
    if (R_FAILED(rc)) {
        soft_errors++;
        last_error = rc;
    }
}

unsigned long usb_keyboard_resets(void) { return host_resets; }
unsigned long usb_keyboard_errors(void) { return soft_errors; }
Result usb_keyboard_last_error(void) { return last_error; }

static void clear_control_state(void) {
    ctrl_in_pending = ctrl_out_pending = led_data_pending = false;
    status_out_after_in = false;
    descriptor_request = descriptor_in_done = descriptor_out_done = false;
}

void usb_keyboard_exit(void) {
    if (initialized) usbDsExit(); // Cancels transfers before buffers can be reused.
    initialized = ready = was_configured = was_ready = descriptor_valid = false;
    state_now = UsbState_Detached;
    clear_control_state();
    interface = NULL;
    endpoint = NULL;
}

Result usb_keyboard_init(void) {
    if (hosversionBefore(5, 0, 0)) return MAKERESULT(Module_Libnx, LibnxError_IncompatSysVer);
    Result rc = usbDsInitialize();
    if (R_FAILED(rc)) return rc;
    initialized = true;
#define TRY(expr) do { rc = (expr); if (R_FAILED(rc)) goto fail; } while (0)
    TRY(usbDsClearDeviceData());
    u8 language, manufacturer, product;
    const u16 langs[] = {0x0409};
    TRY(usbDsAddUsbLanguageStringDescriptor(&language, langs, 1));
    TRY(usbDsAddUsbStringDescriptor(&manufacturer, "NXHID"));
    TRY(usbDsAddUsbStringDescriptor(&product, "NXHID Wi-Fi Keyboard"));
    struct usb_device_descriptor device = {
        .bLength = USB_DT_DEVICE_SIZE, .bDescriptorType = USB_DT_DEVICE,
        .bcdUSB = 0x0200, .bMaxPacketSize0 = 64,
        .idVendor = NXHID_USB_VID, .idProduct = NXHID_USB_PID,
        .bcdDevice = 0x0100, .iManufacturer = manufacturer, .iProduct = product,
        .bNumConfigurations = 1
    };
    TRY(usbDsSetUsbDeviceDescriptor(UsbDeviceSpeed_Full, &device));
    TRY(usbDsSetUsbDeviceDescriptor(UsbDeviceSpeed_High, &device));
    device.bcdUSB = 0x0300;
    device.bMaxPacketSize0 = 9;
    TRY(usbDsSetUsbDeviceDescriptor(UsbDeviceSpeed_Super, &device));
    const u8 bos[] = {
        5,USB_DT_BOS,22,0,2,
        7,USB_DT_DEVICE_CAPABILITY,2,2,0,0,0,
        10,USB_DT_DEVICE_CAPABILITY,3,0,0x0e,0,1,0,0,0
    };
    TRY(usbDsSetBinaryObjectStore(bos, sizeof(bos)));
    TRY(usbDsRegisterInterfaceEx(&interface, 0));
    struct usb_interface_descriptor intf = {
        .bLength = USB_DT_INTERFACE_SIZE, .bDescriptorType = USB_DT_INTERFACE,
        .bInterfaceNumber = 0, .bNumEndpoints = 1,
        .bInterfaceClass = USB_CLASS_HID, .bInterfaceSubClass = 1, .bInterfaceProtocol = 1
    };
    struct usb_endpoint_descriptor ep = {
        .bLength = USB_DT_ENDPOINT_SIZE, .bDescriptorType = USB_DT_ENDPOINT,
        .bEndpointAddress = 0x81, .bmAttributes = USB_TRANSFER_TYPE_INTERRUPT,
        .wMaxPacketSize = 8, .bInterval = 1
    };
    struct usb_ss_endpoint_companion_descriptor companion = {
        .bLength = 6, .bDescriptorType = USB_DT_SS_ENDPOINT_COMPANION, .wBytesPerInterval = 8
    };
    for (UsbDeviceSpeed speed = UsbDeviceSpeed_Full; speed <= UsbDeviceSpeed_Super; speed++) {
        // Request 1 ms polling everywhere: full-speed frames, 2^(4-1) microframes.
        ep.bInterval = speed == UsbDeviceSpeed_Full ? 1 : 4;
        TRY(usbDsInterface_AppendConfigurationData(interface, speed, &intf, sizeof(intf)));
        TRY(usbDsInterface_AppendConfigurationData(interface, speed, hid_descriptor, sizeof(hid_descriptor)));
        TRY(usbDsInterface_AppendConfigurationData(interface, speed, &ep, sizeof(ep)));
        if (speed == UsbDeviceSpeed_Super)
            TRY(usbDsInterface_AppendConfigurationData(interface, speed, &companion, sizeof(companion)));
    }
    TRY(usbDsInterface_RegisterEndpoint(interface, &endpoint, 0x81));
    TRY(usbDsInterface_EnableInterface(interface));
    memset(last_report, 0, sizeof(last_report));
    idle_rate = leds = 0;
    protocol = 1;
    last_sent = configured_at = milliseconds();
    was_ready = was_configured = descriptor_valid = false;
    ready = false;
    host_resets = soft_errors = 0;
    last_error = 0;
    clear_control_state();
    TRY(usbDsEnable());
#undef TRY
    return 0;
fail:
    usb_keyboard_exit();
    return rc;
}

bool usb_keyboard_ready(void) { return initialized && ready; }

uint8_t usb_keyboard_leds(void) { return leds; }

UsbState usb_keyboard_state(void) { return state_now; }

static bool signalled(Event *event) {
    if (R_FAILED(eventWait(event, 0))) return false;
    eventClear(event);
    return true;
}

static void control_stall(void) {
    usbDsInterface_StallCtrl(interface);
    ctrl_in_pending = ctrl_out_pending = led_data_pending = false;
    status_out_after_in = false;
    descriptor_request = false;
}

static void queue_status_out(void) {
    if (R_FAILED(usbDsInterface_CtrlOutPostBufferAsync(interface, control_buffer, 0, &ctrl_out_id))) {
        note(io_error());
        control_stall();
        return;
    }
    ctrl_out_pending = true;
    ctrl_out_expected = 0;
    control_started = milliseconds();
}

static void control_reply(const void *data, size_t size, u16 requested) {
    if (size > requested) size = requested;
    if (size) memcpy(control_buffer, data, size);
    // usb:ds rejects the OUT status while the IN data is still posted; the IN
    // completion handler queues it instead.
    status_out_after_in = control_read;
    if (R_FAILED(usbDsInterface_CtrlInPostBufferAsync(interface, control_buffer, size, &ctrl_in_id))) {
        note(io_error());
        control_stall();
        return;
    }
    ctrl_in_pending = true;
    ctrl_in_expected = size;
    control_started = milliseconds();
}

static void handle_setup(void) {
    struct usb_control_setup s;
    if (R_FAILED(usbDsInterface_GetSetupPacket(interface, &s, sizeof(s)))) {
        note(io_error());
        return;
    }
    // A new SETUP legally supersedes any earlier control request.
    clear_control_state();
    control_read = (s.bmRequestType & 0x80) != 0;
    const u8 zero[2] = {0};
    if (s.wIndex != 0) {
        control_stall();
    } else if (s.bmRequestType == 0x81 && s.bRequest == USB_REQUEST_GET_STATUS && s.wValue == 0) {
        control_reply(zero, 2, s.wLength);
    } else if (s.bmRequestType == 0x81 && s.bRequest == USB_REQUEST_GET_INTERFACE && s.wValue == 0) {
        control_reply(zero, 1, s.wLength);
    } else if (s.bmRequestType == 0x01 && s.bRequest == USB_REQUEST_SET_INTERFACE && s.wValue == 0 && s.wLength == 0) {
        control_reply(NULL, 0, 0);
    } else if (s.bmRequestType == 0x81 && s.bRequest == USB_REQUEST_GET_DESCRIPTOR && (s.wValue & 0xff) == 0) {
        if ((s.wValue >> 8) == USB_DT_REPORT) {
            descriptor_request = s.wLength >= sizeof(report_descriptor);
            if (descriptor_request) descriptor_in_done = descriptor_out_done = false;
            control_reply(report_descriptor, sizeof(report_descriptor), s.wLength);
        } else if ((s.wValue >> 8) == USB_DT_HID) {
            control_reply(hid_descriptor, sizeof(hid_descriptor), s.wLength);
        } else {
            control_stall();
        }
    } else if (s.bmRequestType == 0xA1 && s.bRequest == 1 && (s.wValue & 0xff) == 0) {
        if ((s.wValue >> 8) == 1) control_reply(last_report, sizeof(last_report), s.wLength);
        else if ((s.wValue >> 8) == 2) control_reply(&leds, 1, s.wLength);
        else control_stall();
    } else if (s.bmRequestType == 0xA1 && s.bRequest == 2 && s.wValue == 0) {
        control_reply(&idle_rate, 1, s.wLength);
    } else if (s.bmRequestType == 0xA1 && s.bRequest == 3 && s.wValue == 0) {
        control_reply(&protocol, 1, s.wLength);
    } else if (s.bmRequestType == 0x21 && s.bRequest == 10 && (s.wValue & 0xff) == 0 && s.wLength == 0) {
        idle_rate = s.wValue >> 8;
        control_reply(NULL, 0, 0);
    } else if (s.bmRequestType == 0x21 && s.bRequest == 11 && s.wValue <= 1 && s.wLength == 0) {
        protocol = s.wValue;
        control_reply(NULL, 0, 0);
    } else if (s.bmRequestType == 0x21 && s.bRequest == 9 && s.wValue == 0x0200 && s.wLength == 1) {
        if (R_FAILED(usbDsInterface_CtrlOutPostBufferAsync(interface, control_buffer, 1, &ctrl_out_id))) {
            note(io_error());
            control_stall();
            return;
        }
        ctrl_out_pending = led_data_pending = true;
        ctrl_out_expected = 1;
        control_started = milliseconds();
    } else {
        control_stall();
    }
}

static void retire_control(void) {
    // Retire old completions before reading a new SETUP packet; an unmatched
    // id means the transfer was superseded and is dropped without a stall.
    if (signalled(&interface->CtrlInCompletionEvent)) {
        UsbDsReportData reports;
        if (R_SUCCEEDED(usbDsInterface_GetCtrlInReportData(interface, &reports))) {
            bool found = false;
            for (u32 i = 0; i < reports.report_count && i < 8; i++)
                if (ctrl_in_pending && reports.report[i].id == ctrl_in_id) found = true;
            if (found) {
                u32 actual = 0;
                Result rc = usbDsParseReportData(&reports, ctrl_in_id, NULL, &actual);
                ctrl_in_pending = false;
                if (R_SUCCEEDED(rc) && actual == ctrl_in_expected) {
                    if (descriptor_request) descriptor_in_done = true;
                    if (status_out_after_in) {
                        status_out_after_in = false;
                        queue_status_out();
                    }
                } else {
                    note(io_error());
                    control_stall();
                }
            }
        } else {
            note(io_error());
        }
    }
    if (signalled(&interface->CtrlOutCompletionEvent)) {
        UsbDsReportData reports;
        if (R_SUCCEEDED(usbDsInterface_GetCtrlOutReportData(interface, &reports))) {
            bool found = false;
            for (u32 i = 0; i < reports.report_count && i < 8; i++)
                if (ctrl_out_pending && reports.report[i].id == ctrl_out_id) found = true;
            if (found) {
                u32 actual = 0;
                Result rc = usbDsParseReportData(&reports, ctrl_out_id, NULL, &actual);
                ctrl_out_pending = false;
                if (R_SUCCEEDED(rc) && actual == ctrl_out_expected) {
                    if (led_data_pending) {
                        led_data_pending = false;
                        armDCacheFlush(control_buffer, sizeof(control_buffer));
                        leds = control_buffer[0] & 0x1f;
                        control_reply(NULL, 0, 0);
                    } else if (descriptor_request) {
                        descriptor_out_done = true;
                    }
                } else {
                    note(io_error());
                    control_stall();
                }
            }
        } else {
            note(io_error());
        }
    }
    if (descriptor_request && descriptor_in_done && descriptor_out_done && !descriptor_valid)
        descriptor_valid = true;
    if (signalled(&interface->SetupEvent)) handle_setup();
    if ((ctrl_in_pending || ctrl_out_pending) && milliseconds() - control_started > CONTROL_TIMEOUT_MS) {
        note(io_error());
        control_stall();
    }
}

// A report the host stopped polling (reset or suspend mid-transfer) is cancelled,
// not torn down: the session survives and the buffer is reusable once Cancel returns.
static void cancel_write(void) {
    usbDsEndpoint_Cancel(endpoint);
    eventClear(&endpoint->CompletionEvent);
    UsbDsReportData reports;
    usbDsEndpoint_GetReportData(endpoint, &reports);
}

Result usb_keyboard_write(const uint8_t report[8]) {
    if (!initialized) return MAKERESULT(Module_Libnx, LibnxError_NotInitialized);
    UsbState state;
    Result rc = usbDsGetState(&state);
    if (R_FAILED(rc)) return rc;
    if (state != UsbState_Configured) return io_error();
    memcpy(input_buffer, report, 8);
    // Drain any stale completion signal (e.g. a cancelled prior transfer)
    // without svcResetSignal: resetting an unsignaled event fails with
    // kernel InvalidState on hardware, which would kill every write.
    if (R_SUCCEEDED(eventWait(&endpoint->CompletionEvent, 0))) {
        eventClear(&endpoint->CompletionEvent);
        UsbDsReportData stale;
        usbDsEndpoint_GetReportData(endpoint, &stale);
    }
    u32 id, transferred;
    rc = usbDsEndpoint_PostBufferAsync(endpoint, input_buffer, 8, &id);
    if (R_FAILED(rc)) return rc;
    u64 deadline = milliseconds() + REPORT_TIMEOUT_MS;
    for (;;) {
        rc = eventWait(&endpoint->CompletionEvent, EVENT_POLL_NS);
        if (R_SUCCEEDED(rc)) break;
        if (rc != KERNELRESULT(TimedOut)) return rc;
        retire_control();
        if (milliseconds() >= deadline) {
            cancel_write();
            return KERNELRESULT(TimedOut);
        }
    }
    rc = eventClear(&endpoint->CompletionEvent);
    if (R_FAILED(rc)) return rc; // Signaled when we get here; keep the real code.
    UsbDsReportData reports;
    rc = usbDsEndpoint_GetReportData(endpoint, &reports);
    if (R_FAILED(rc)) return rc;
    rc = usbDsParseReportData(&reports, id, NULL, &transferred);
    if (R_FAILED(rc)) { // Superseded or cancelled in flight.
        cancel_write();
        return io_error();
    }
    if (transferred != 8) return io_error();
    memmove(last_report, report, 8);
    last_sent = milliseconds();
    return 0;
}

Result usb_keyboard_tick(void) {
    if (!initialized) return MAKERESULT(Module_Libnx, LibnxError_NotInitialized);
    retire_control();
    UsbState state;
    Result rc = usbDsGetState(&state);
    note(rc);
    if (R_FAILED(rc)) return 0;
    bool configured = state == UsbState_Configured;
    state_now = state;
    if (configured && !was_configured) configured_at = milliseconds();
    if (!configured && was_configured) {
        // Host reset, suspend, or unplug: session bookkeeping only, never a fatal error.
        host_resets += state != UsbState_Suspended;
        ready = was_ready = false;
        memset(last_report, 0, sizeof(last_report));
        if (state != UsbState_Suspended) {
            // Everything re-negotiated on the next enumeration; suspend keeps it.
            descriptor_valid = false;
            clear_control_state();
            leds = idle_rate = 0;
            protocol = 1;
        }
    }
    was_configured = configured;
    ready = configured && descriptor_valid &&
            milliseconds() - configured_at >= CONFIGURED_SETTLE_MS;
    if (ready && !was_ready) {
        was_ready = true;
        note(usb_keyboard_write((const uint8_t[8]){0}));
    } else if (ready && idle_rate && milliseconds() - last_sent >= (u64)idle_rate * 4)
        note(usb_keyboard_write(last_report));
    return 0;
}
