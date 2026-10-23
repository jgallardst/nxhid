#include "usb_keyboard.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static UsbDsInterface intf;
static UsbDsEndpoint endpoint;
static UsbState state;
static Result state_error;
static u64 now_ns;
static u32 next_id;
static unsigned writes, stalls, cancels;
static bool suppress_completion, short_write, active;
static u8 scratch[0x1000] __attribute__((aligned(0x1000)));
static struct usb_control_setup setup;
static struct usb_device_descriptor devices[5];
static u8 configurations[5][64], input[8], control_in[64], led_output;
static size_t config_sizes[5], control_size;
static UsbDsReportData in_report, out_report, key_report;

bool hosversionBefore(int major, int minor, int micro) {
    (void)major; (void)minor; (void)micro;
    return false;
}
u64 armGetSystemTick(void) { return now_ns; }
u64 armTicksToNs(u64 ticks) { return ticks; }
void armDCacheFlush(void *buffer, size_t size) { (void)buffer; (void)size; }
Result eventClear(Event *event) {
    // Faithful to hardware: svcResetSignal on an unsignaled event fails.
    if (!event->signaled) return KERNELRESULT(InvalidState);
    event->signaled = false;
    return 0;
}
Result eventWait(Event *event, u64 timeout) {
    if (event->error) return event->error;
    if (event->signaled) return 0;
    now_ns += timeout;
    return KERNELRESULT(TimedOut);
}
Result usbDsInitialize(void) { active = true; return 0; }
void usbDsExit(void) { active = false; }
Result usbDsClearDeviceData(void) { memset(config_sizes, 0, sizeof(config_sizes)); return 0; }
Result usbDsAddUsbLanguageStringDescriptor(u8 *index, const u16 *languages, u16 count) {
    assert(count == 1 && languages[0] == 0x0409);
    *index = 0;
    return 0;
}
Result usbDsAddUsbStringDescriptor(u8 *index, const char *text) { assert(*text); *index = 1; return 0; }
Result usbDsSetUsbDeviceDescriptor(UsbDeviceSpeed speed, struct usb_device_descriptor *descriptor) {
    devices[speed] = *descriptor;
    return 0;
}
Result usbDsSetBinaryObjectStore(const void *data, size_t size) {
    const u8 *bytes = data;
    assert(size == bytes[2] && bytes[1] == USB_DT_BOS);
    return 0;
}
Result usbDsRegisterInterfaceEx(UsbDsInterface **out, u8 index) {
    memset(&intf, 0, sizeof(intf));
    intf.interface_index = index;
    *out = &intf;
    return 0;
}
Result usbDsInterface_AppendConfigurationData(UsbDsInterface *interface, UsbDeviceSpeed speed, const void *data, size_t size) {
    assert(interface == &intf && config_sizes[speed] + size <= sizeof(configurations[speed]));
    memcpy(configurations[speed] + config_sizes[speed], data, size);
    config_sizes[speed] += size;
    return 0;
}
Result usbDsInterface_RegisterEndpoint(UsbDsInterface *interface, UsbDsEndpoint **out, u8 address) {
    assert(interface == &intf && address == 0x81);
    memset(&endpoint, 0, sizeof(endpoint));
    *out = &endpoint;
    return 0;
}
Result usbDsInterface_EnableInterface(UsbDsInterface *interface) { assert(interface == &intf); return 0; }
Result usbDsEnable(void) { assert(active); return 0; }
Result usbDsGetState(UsbState *out) { *out = state; return state_error; }

static void post(Event *event, void *data, size_t size, u32 *id, UsbDsReportData *report) {
    assert((uintptr_t)data % 0x1000 == 0);
    *id = ++next_id;
    *report = (UsbDsReportData){.report_count = 1, .report = {{*id, size, size, 3}}};
    event->signaled = true;
}
Result usbDsInterface_CtrlInPostBufferAsync(UsbDsInterface *interface, void *data, size_t size, u32 *id) {
    assert(size <= sizeof(control_in));
    if (size) {
        memcpy(control_in, data, size);
        control_size = size;
    }
    post(&interface->CtrlInCompletionEvent, data, size, id, &in_report);
    return 0;
}
Result usbDsInterface_CtrlOutPostBufferAsync(UsbDsInterface *interface, void *data, size_t size, u32 *id) {
    assert(size <= 1);
    if (size) *(u8 *)data = led_output;
    post(&interface->CtrlOutCompletionEvent, data, size, id, &out_report);
    return 0;
}
Result usbDsInterface_GetCtrlInReportData(UsbDsInterface *interface, UsbDsReportData *out) {
    assert(interface == &intf); *out = in_report; return 0;
}
Result usbDsInterface_GetCtrlOutReportData(UsbDsInterface *interface, UsbDsReportData *out) {
    assert(interface == &intf); *out = out_report; return 0;
}
Result usbDsParseReportData(UsbDsReportData *report, u32 id, u32 *requested, u32 *transferred) {
    assert(report->report_count == 1 && report->report[0].id == id);
    if (requested) *requested = report->report[0].requestedSize;
    if (transferred) *transferred = report->report[0].transferredSize;
    return 0;
}
Result usbDsInterface_StallCtrl(UsbDsInterface *interface) { assert(interface == &intf); stalls++; return 0; }
Result usbDsInterface_GetSetupPacket(UsbDsInterface *interface, void *data, size_t size) {
    assert(interface == &intf && size == sizeof(setup));
    memcpy(data, &setup, size);
    return 0;
}
Result usbDsEndpoint_PostBufferAsync(UsbDsEndpoint *ep, void *data, size_t size, u32 *id) {
    assert(ep == &endpoint && size == sizeof(input));
    memcpy(input, data, size);
    writes++;
    post(&ep->CompletionEvent, data, size, id, &key_report);
    if (short_write) key_report.report[0].transferredSize--;
    if (suppress_completion) ep->CompletionEvent.signaled = false;
    return 0;
}
Result usbDsEndpoint_Cancel(UsbDsEndpoint *ep) { assert(ep == &endpoint); cancels++; return 0; }
Result usbDsEndpoint_GetReportData(UsbDsEndpoint *ep, UsbDsReportData *out) {
    assert(ep == &endpoint); *out = key_report; return 0;
}

static Result request(u8 type, u8 code, u16 value, u16 length) {
    setup = (struct usb_control_setup){type, code, value, 0, length};
    intf.SetupEvent.signaled = true;
    for (int i = 0; i < 16; i++) usb_keyboard_tick(); // Pump the async control stages.
    return 0;
}
static void descriptors(void) {
    for (int speed = 2; speed <= 4; speed++) {
        assert(devices[speed].bNumConfigurations == 1);
        assert(devices[speed].bLength == 18);
        assert(config_sizes[speed] == (speed == 4 ? 31 : 25));
        const u8 *config = configurations[speed];
        assert(config[4] == 1 && config[5] == 3 && config[6] == 1 && config[7] == 1);
        assert(config[10] == USB_DT_HID && config[15] == USB_DT_REPORT);
        assert(config[20] == 0x81 && config[21] == USB_TRANSFER_TYPE_INTERRUPT);
        assert(config[22] == 8 && config[24] == (speed == 2 ? 1 : 4));
    }
    assert(devices[4].bMaxPacketSize0 == 9);
    assert(request(0x81, 6, USB_DT_REPORT << 8, 255) == 0);
    assert(control_size == configurations[2][16]);
    assert(control_in[0] == 5 && control_in[control_size - 1] == 0xC0);
    assert(request(0x81, 6, USB_DT_REPORT << 8, 8) == 0);
    assert(control_size == 8);
    assert(request(0x81, 6, USB_DT_REPORT << 8, 0) == 0 && stalls == 0);
}
static void controls(void) {
    led_output = 0xE7;
    assert(request(0x21, 9, 0x0200, 1) == 0);
    assert(usb_keyboard_leds() == 7);
    assert(request(0xA1, 1, 0x0200, 1) == 0 && control_in[0] == 7);
    assert(request(0x21, 11, 0, 0) == 0);
    assert(request(0xA1, 3, 0, 1) == 0 && control_in[0] == 0);
    assert(request(0x21, 10, 0x0300, 0) == 0);
    assert(request(0xA1, 2, 0, 1) == 0 && control_in[0] == 3);
    assert(request(0x21, 9, 0x0200, 4096) == 0 && stalls == 1);
    state = UsbState_Configured;
    assert(usb_keyboard_tick() == 0 && !usb_keyboard_ready()); // settle not elapsed
    now_ns += 300000000;
    assert(usb_keyboard_tick() == 0 && !usb_keyboard_ready()); // descriptor not fetched
    assert(request(0x81, 6, USB_DT_REPORT << 8, 255) == 0);
    assert(usb_keyboard_tick() == 0 && usb_keyboard_ready());
    assert(!memcmp(input, (u8[8]){0}, 8));
    assert(usb_keyboard_write((u8[8]){2, 0, 4}) == 0);
    assert(request(0xA1, 1, 0x0100, 8) == 0 && control_in[0] == 2 && control_in[2] == 4);
    unsigned before = writes;
    now_ns += 12000000;
    assert(usb_keyboard_tick() == 0 && writes == before + 1);
    state = UsbState_Suspended; // Host suspend: session survives, host state kept.
    assert(usb_keyboard_tick() == 0 && !usb_keyboard_ready());
    assert(usb_keyboard_leds() == 7 && usb_keyboard_resets() == 0);
    state = UsbState_Configured; // Resume settles again, descriptor still valid.
    assert(usb_keyboard_tick() == 0 && !usb_keyboard_ready());
    now_ns += 300000000;
    assert(usb_keyboard_tick() == 0 && usb_keyboard_ready());
    assert(!memcmp(input, (u8[8]){0}, 8));
    state = UsbState_Detached; // Host reset or unplug: no fatal error, re-enumerate.
    assert(usb_keyboard_tick() == 0 && !usb_keyboard_ready());
    assert(usb_keyboard_resets() == 1 && usb_keyboard_leds() == 0);
    usb_keyboard_exit();
    assert(usb_keyboard_init() == 0 && usb_keyboard_leds() == 0 && usb_keyboard_resets() == 0);
    state = UsbState_Configured;
    assert(usb_keyboard_tick() == 0);
    assert(request(0x81, 6, USB_DT_REPORT << 8, 255) == 0); // Host re-fetches descriptor.
    now_ns += 300000000;
    assert(usb_keyboard_tick() == 0 && usb_keyboard_ready());
    assert(!memcmp(input, (u8[8]){0}, 8));
}
static void errors(void) {
    state_error = 900;
    assert(usb_keyboard_tick() == 0); // Service errors are soft; the session survives.
    assert(usb_keyboard_write((u8[8]){0}) == 900);
    assert(usb_keyboard_errors() == 1 && usb_keyboard_last_error() == 900);
    state_error = 0;
    // A wedged control transfer stalls at the deadline; the session continues.
    setup = (struct usb_control_setup){0x81, 6, USB_DT_REPORT << 8, 0, 8};
    intf.SetupEvent.signaled = true;
    assert(usb_keyboard_tick() == 0);
    intf.CtrlInCompletionEvent.signaled = false; // Host never completes the data stage.
    unsigned before = stalls;
    now_ns += 1600000000;
    assert(usb_keyboard_tick() == 0 && stalls == before + 1 && usb_keyboard_errors() == 2);
    // A superseded completion (host aborted mid-transfer) is dropped silently.
    setup = (struct usb_control_setup){0x81, 6, USB_DT_REPORT << 8, 0, 8};
    intf.SetupEvent.signaled = true;
    assert(usb_keyboard_tick() == 0); // Posts the IN data stage.
    u32 stale;
    usbDsInterface_CtrlInPostBufferAsync(&intf, scratch, 8, &stale); // Host aborted it.
    before = stalls;
    unsigned seen = usb_keyboard_errors();
    assert(usb_keyboard_tick() == 0 && stalls == before && usb_keyboard_errors() == seen);
    request(0x21, 11, 0, 0); // A fresh SETUP clears the abandoned transfer.
    // Endpoint service failures fail the report, never the session.
    usb_keyboard_exit();
    assert(usb_keyboard_init() == 0);
    state = UsbState_Configured;
    endpoint.CompletionEvent.error = 903;
    assert(usb_keyboard_write((u8[8]){0}) == 903);
    u64 frozen = now_ns; // An IPC failure must not be retried as a timeout.
    usb_keyboard_exit();
    assert(usb_keyboard_init() == 0);
    assert(now_ns == frozen);
    short_write = true; // Short transfers fail the report, not the session.
    assert(R_FAILED(usb_keyboard_write((u8[8]){0})));
    short_write = false;
    usb_keyboard_exit();
    assert(usb_keyboard_init() == 0);
    state = UsbState_Configured;
    suppress_completion = true; // Host stops polling mid-transfer: cancel, not teardown.
    assert(usb_keyboard_write((u8[8]){0}) == KERNELRESULT(TimedOut));
    assert(now_ns == frozen + 1500000000); // 1.5 s report deadline, 1 ms per poll.
    assert(cancels == 1);
    suppress_completion = false;
    // Regression for hardware: after a cancelled transfer the completion event
    // is unsignaled, and resetting it used to fail every subsequent write.
    assert(usb_keyboard_write((u8[8]){0}) == 0);
    endpoint.CompletionEvent.signaled = true; // Stale signal: drained, not reset.
    assert(usb_keyboard_write((u8[8]){0}) == 0);
    assert(usb_keyboard_write((u8[8]){0}) == 0);
}
int main(int argc, char **argv) {
    assert(argc == 2);
    assert(R_FAILED(usb_keyboard_tick()));
    assert(R_FAILED(usb_keyboard_write((u8[8]){0})));
    assert(usb_keyboard_init() == 0);
    if (!strcmp(argv[1], "descriptors")) descriptors();
    else if (!strcmp(argv[1], "controls")) controls();
    else if (!strcmp(argv[1], "errors")) errors();
    else return 2;
    usb_keyboard_exit();
    assert(!active && !usb_keyboard_ready());
    return 0;
}
