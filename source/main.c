#include <switch.h>
#include <arpa/inet.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include "server.h"
#include "usb_keyboard.h"

#ifndef NXHID_VERSION
#define NXHID_VERSION "dev"
#endif

static unsigned long seen_resets, seen_errors;
static FILE *log_file;
static u64 log_origin;
static bool was_ready;
static char last_event[96] = "events start";

static u64 milliseconds(void) { return armTicksToNs(armGetSystemTick()) / 1000000; }

// Every notable session and USB event lands in nxhid.log with a millisecond
// timeline; failures on real hardware are diagnosed from this trace.
static void log_line(const char *format, ...) {
    if (!log_file) return;
    fprintf(log_file, "[%8llu ms] ", (unsigned long long)(milliseconds() - log_origin));
    va_list args;
    va_start(args, format);
    vfprintf(log_file, format, args);
    va_end(args);
    fputc('\n', log_file);
    fflush(log_file);
}

static const char *usb_state_name(UsbState state) {
    switch (state) {
        case UsbState_Detached: return "detached";
        case UsbState_Attached: return "attached";
        case UsbState_Powered: return "powered";
        case UsbState_Default: return "default";
        case UsbState_Address: return "address";
        case UsbState_Configured: return "configured";
        case UsbState_Suspended: return "suspended";
    }
    return "?";
}

static void on_server_event(Server *s, ServerEvent event) {
    const char *name;
    switch (event) {
        case ServerEventAccepted: name = "sender accepted"; break;
        case ServerEventRejected: name = "sender rejected (busy)"; break;
        case ServerEventDropPeer: name = "dropped: peer closed"; break;
        case ServerEventDropSend: name = "dropped: send failed"; break;
        case ServerEventDropIdle: name = "dropped: heartbeat timeout"; break;
        case ServerEventDropPause: name = "dropped: USB paused too long"; break;
        case ServerEventDropRetry: name = "dropped: report undeliverable"; break;
        case ServerEventDropInvalid: name = "dropped: invalid input"; break;
        case ServerEventReportHeld: name = "report held for retry"; break;
        case ServerEventReportSent: name = "report delivered"; break;
        default: name = "?"; break;
    }
    snprintf(last_event, sizeof(last_event), "%s", name);
    if (event == ServerEventAccepted)
        log_line("%s from %s", name, s->peer);
    else
        log_line("%s", name);
}

static bool write_report(const uint8_t report[8]) {
    // A failed write drops this report and the sender session; the USB session
    // itself is never torn down at runtime (only initialization can end it).
    static bool failing;
    Result rc = usb_keyboard_write(report);
    if (R_FAILED(rc) && !failing) {
        failing = true;
        log_line("report writes failing: %08x", rc);
    } else if (R_SUCCEEDED(rc)) {
        failing = false;
    }
    return R_SUCCEEDED(rc);
}

int main(void) {
    consoleInit(NULL);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);
    bool sockets = false, network = false, usb = false;
    Server server = {.listener = -1, .client = -1};
    char message[160] = "Connect the Switch USB-C port directly to the target device.";
    Result rc = 0;

    if (appletGetAppletType() != AppletType_Application) {
        snprintf(message, sizeof(message), "Full application mode required. Hold R while launching a game, then open NXHID.");
        goto error;
    }
    rc = appletSetFocusHandlingMode(AppletFocusHandlingMode_NoSuspend);
    if (R_FAILED(rc)) goto error;
    rc = socketInitializeDefault();
    if (R_FAILED(rc)) goto error;
    sockets = true;
    rc = nifmInitialize(NifmServiceType_User);
    if (R_FAILED(rc)) goto error;
    network = true;
    rc = usb_keyboard_init();
    if (R_FAILED(rc)) goto error;
    usb = true;
    log_file = fopen("sdmc:/switch/nxhid/nxhid.log", "w");
    log_origin = milliseconds();
    log_line("NXHID %s starting (HOS %u.%u.%u)", NXHID_VERSION,
             (unsigned)HOSVER_MAJOR(hosversionGet()), (unsigned)HOSVER_MINOR(hosversionGet()),
             (unsigned)HOSVER_MICRO(hosversionGet()));
    if (!server_init(&server, NXHID_PORT, write_report, usb_keyboard_leds, on_server_event)) {
        snprintf(message, sizeof(message), "Cannot listen on TCP %d: %s", NXHID_PORT, strerror(errno));
        goto error;
    }
    appletSetAutoSleepDisabled(true);
    u64 next_draw = 0;
    while (appletMainLoop()) {
        padUpdate(&pad);
        u64 down = padGetButtonsDown(&pad);
        if (down & HidNpadButton_Plus) break;
        if (down & HidNpadButton_B) {
            log_line("sender dropped by user");
            server_drop(&server);
            snprintf(message, sizeof(message), "Sender disconnected. Waiting for a new connection.");
        }
        bool focused = appletGetFocusState() == AppletFocusState_InFocus;
        static bool was_focused = true;
        if (focused != was_focused) {
            was_focused = focused;
            log_line("focus %s", focused ? "gained" : "lost");
        }
        usb_keyboard_tick();
        bool ready = usb_keyboard_ready();
        if (ready != was_ready) {
            was_ready = ready;
            log_line("keyboard %s (usb %s)", ready ? "ready" : "lost", usb_state_name(usb_keyboard_state()));
        }
        if (ready && seen_resets != usb_keyboard_resets()) {
            seen_resets = usb_keyboard_resets();
            log_line("host reset the bus");
            snprintf(message, sizeof(message), "Host reset the bus; keyboard re-ready.");
        }
        if (seen_errors != usb_keyboard_errors()) {
            seen_errors = usb_keyboard_errors();
            log_line("usb error %08x (usb %s)", usb_keyboard_last_error(), usb_state_name(usb_keyboard_state()));
            // Superseded control transfers are expected host re-enumeration noise.
            if (!ready)
                snprintf(message, sizeof(message), "USB hiccup (%08x); continuing.", usb_keyboard_last_error());
        }
        server_tick(&server, milliseconds(), ready && focused);
        u64 now = milliseconds();
        if (focused && now >= next_draw) {
            next_draw = now + 250;
            u32 ip = 0;
            char address[32] = "Wi-Fi not connected";
            if (R_SUCCEEDED(nifmGetCurrentIpAddress(&ip)) && ip)
                inet_ntop(AF_INET, &ip, address, sizeof(address));
            printf("\x1b[2J\x1b[HNXHID %s - Wi-Fi to USB keyboard\n\n", NXHID_VERSION);
            printf("Switch IP:  %s\nTCP port:   %d\n\n", address, NXHID_PORT);
            printf("USB:        %s\nSender:     %s\nReports:    %lu\n", ready ? "Keyboard ready" : "Waiting for USB host", server.client >= 0 ? server.peer : "Waiting", server.reports);
            printf("Resets:     %lu   Hiccups: %lu (last %08x)\n\n", usb_keyboard_resets(), usb_keyboard_errors(), usb_keyboard_last_error());
            u8 leds = usb_keyboard_leds();
            printf("Host LEDs:  Num %s  Caps %s  Scroll %s\n\n", leds & 1 ? "on" : "off", leds & 2 ? "on" : "off", leds & 4 ? "on" : "off");
            printf("B: disconnect sender    +: exit\n\n%s\n\n", message);
            printf("Event: %s\n", last_event);
            printf("Log: sdmc:/switch/nxhid/nxhid.log\n");
            printf("Computer -> Wi-Fi -> Switch -> USB -> target device\n");
            consoleUpdate(NULL);
        }
        svcSleepThread(1000000);
    }
    goto cleanup;
error:
    server_close(&server);
    if (usb) { usb_keyboard_exit(); usb = false; }
    printf("\x1b[2J\x1b[HNXHID could not continue.\nResult: %08x\n%s\n\nPress + to exit.\n", rc, message);
    while (appletMainLoop()) {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & HidNpadButton_Plus) break;
        consoleUpdate(NULL);
    }
cleanup:
    log_line("exiting");
    server_close(&server);
    if (usb) usb_keyboard_exit();
    if (network) nifmExit();
    if (sockets) socketExit();
    if (log_file) fclose(log_file);
    appletSetAutoSleepDisabled(false);
    consoleExit(NULL);
    return 0;
}
