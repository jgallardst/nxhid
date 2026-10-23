#pragma once
#include "protocol.h"
#include <stddef.h>

typedef bool (*ReportWriter)(const uint8_t report[8]);
typedef uint8_t (*LedReader)(void);
typedef enum {
    ServerEventAccepted,    // Sender connected (server->peer holds the address).
    ServerEventRejected,    // Connection refused: a sender is already active.
    ServerEventDropPeer,    // Peer closed the socket or a receive error hit.
    ServerEventDropSend,    // Sending the reply failed.
    ServerEventDropIdle,    // Heartbeat watchdog expired.
    ServerEventDropPause,   // USB/focus stayed paused past the grace window.
    ServerEventDropRetry,   // Held report undeliverable past the retry window.
    ServerEventDropInvalid, // Protocol violation in a command line.
    ServerEventReportHeld,  // Delivery failed mid-report; retrying.
    ServerEventReportSent,  // Report delivered (first try or after retry).
} ServerEvent;
typedef struct Server Server;
typedef void (*ServerLogger)(Server *server, ServerEvent event);
typedef struct Server {
    int listener, client;
    uint64_t last_activity;
    uint64_t disabled_since, pending_since;
    bool pending;
    uint8_t pending_report[8];
    char line[NXHID_LINE_SIZE], output[64], peer[48];
    size_t used, output_size, output_sent;
    ReportWriter write_report;
    LedReader read_leds;
    ServerLogger log;
    unsigned long reports;
} Server;

bool server_init(Server *s, uint16_t port, ReportWriter writer, LedReader reader, ServerLogger logger);
void server_tick(Server *s, uint64_t now_ms, bool enabled);
void server_drop(Server *s);
void server_close(Server *s);
