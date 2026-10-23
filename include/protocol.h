#pragma once
#include <stdbool.h>
#include <stdint.h>

#define NXHID_PORT 24872
#define NXHID_LINE_SIZE 96
#define NXHID_TIMEOUT_MS 3000
#ifndef NXHID_PAUSE_MS
// How long a USB or focus blink may pause a sender before its session drops.
#define NXHID_PAUSE_MS 5000
#endif
#ifndef NXHID_RETRY_MS
// How long an undelivered report is retried before the session drops.
#define NXHID_RETRY_MS 1500
#endif

typedef enum { CMD_INVALID, CMD_PING, CMD_REPORT, CMD_LEDS } CommandType;
typedef struct {
    CommandType type;
    uint8_t report[8];
} Command;

Command protocol_parse(const char *line);
