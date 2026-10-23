# NXHID

Turn a homebrew-enabled Nintendo Switch into a USB keyboard controlled over Wi-Fi.

Originally made for [ns2jb](https://gitlab.com/pepsipu/ns2jb). This fork adds
Nintendo Switch 2 host-compatibility fixes — see [Switch 2 fixes](#switch-2-fixes).

```text
Computer → Wi-Fi → Switch → USB-C → target device
```

## Switch 2 fixes

The upstream 0.1.0 build died against a Nintendo Switch 2 with
`USB reset (0002b20a)` and closed sender sessions. The Switch 2 host bus-resets,
suspends, and re-enumerates a freshly attached keyboard far more aggressively
than a PC, and upstream turned every abandoned transfer into a fatal error
followed by a full `usb:ds` teardown. This fork keeps the USB session alive
through all of it:

- **Non-fatal USB state machine.** Leaving `Configured` (bus reset, suspend,
  re-enumeration) only resets session bookkeeping; the app never tears down
  the `usb:ds` session at runtime. "Ready" additionally requires the report
  descriptor round-trip plus a 300 ms settle, so the initial report is never
  written into a host reset.
- **Asynchronous control transfers** (approach ported from
  [plume-usb](https://github.com/MichelSama/plume-usb)): completions retire
  through events before new SETUP packets are read, a new SETUP legally
  supersedes an earlier request and is dropped silently instead of stalling
  EP0 (those stalls sent the host into an endless re-enumeration loop), and
  only a transfer stuck for 1.5 s is stalled.
- **Report retry over TCP.** A report whose USB delivery fails mid-transfer is
  held and retried for 1.5 s, and a USB or focus blink pauses the sender
  session (up to 5 s) instead of dropping it, so short re-enumeration hiccups
  no longer kill a typing session. A failed report is never acknowledged.
- **`svcResetSignal` fix.** `eventClear` on a not-yet-signaled endpoint
  completion event returns kernel `InvalidState` on hardware, which made
  *every* keyboard write fail instantly with `LibnxError_IoError` (`0x1559`) —
  the cause of persistent "Switch closed the session" client errors. Writes
  now drain a stale completion with a zero-timeout wait instead of resetting
  the event, and the host-test fake reproduces the kernel behavior so this
  cannot regress unnoticed.

Along the way the app gained on-screen diagnostics (ready state, host reset
and error counters, version, last event), an SD event timeline at
`sdmc:/switch/nxhid/nxhid.log`, and `client/nxhid.py --debug` tracing.

## Setup

You need a homebrew-enabled Switch running Horizon OS 5.0.0+, a USB data cable,
and Python 3.8+ on the sender. The Switch and sender must share a LAN.
Connections use TCP port `24872`.

1. Copy `nxhid.nro` to `/switch/nxhid/nxhid.nro` on the SD card.
2. Hold **R** while launching a game to open Homebrew Menu in full application
   mode, then launch NXHID. Album/applet mode is not supported.
3. Connect the Switch's own USB-C port directly to the target device.
4. Wait for **Keyboard ready**, then use the Switch IP shown on screen.

<p align="center">
  <img src="docs/images/screenshot.jpg" alt="NXHID running on a Switch connected by USB to a Switch 2" width="480">
  <br>
  <em>Switch running NXHID connected to a Switch 2</em>
</p>

Keep NXHID open while forwarding. **B** disconnects the sender and releases keys;
**+** exits. Only one sender can connect at a time.

## Send text

Replace `192.168.1.50` with your Switch IP.

```sh
# Literal text.
python3 client/nxhid.py 192.168.1.50 text 'Hello!'

# File input.
python3 client/nxhid.py 192.168.1.50 text --file notes.txt

# Stream stdin as it arrives.
printf 'Hello\n' | python3 client/nxhid.py 192.168.1.50 text

# Avoid SmileBASIC auto-indent with ENTER,HOME,CTRL+DELETE at each newline.
python3 client/nxhid.py 192.168.1.50 text --delay 0.02 \
  --newline-sequence 'ENTER,HOME,CTRL+DELETE' < notes.txt
```

`--newline-sequence` replaces each newline with the sequence in order. Each step
is held for 50 ms, then released. `--delay` sets the pause after each text
character and sequence step, including the last step; the default is 0.02 seconds.
Without a sequence, a newline sends Enter. CRLF counts as one newline; blank
lines and trailing newlines also run the sequence.

Stdin streams before EOF and keeps the connection alive during pauses. Producers
must flush their output for immediate delivery. `--file -` also selects stdin.
Interactive terminals normally buffer input until Enter.

Text uses a **US keyboard layout** and supports printable ASCII, newlines, tabs,
and backspace. Caps Lock is compensated using the target's latest LED report.
Unsupported characters stop sending; already-streamed characters cannot be undone.

## Live input

```sh
python3 client/nxhid.py 192.168.1.50 live
```

Requires Tkinter. Type in the focused window; Escape stops forwarding and losing
focus releases keys. Live mode forwards physical key states, with lock state
controlled by the target. Run `python3 client/nxhid.py --help` for other commands.

On macOS, use Python with Tk 8.6 or newer. With Homebrew:

```sh
brew install python-tk@3.14
python3.14 client/nxhid.py 192.168.1.50 live
```

## Debugging

The Switch app writes an event timeline to `sdmc:/switch/nxhid/nxhid.log`
(sender accept/drop reasons, reports held and delivered, USB state transitions
and error codes, focus changes). Fetch the file after a failed run. The
on-screen `Event:` line shows the latest event, and the title shows the
running version. On the sender, `--debug` (or `NXHID_DEBUG=1`) traces every
command, reply, and round-trip time to stderr.

## Build

Install devkitPro's Switch toolchain (`devkitA64`, `libnx`, and Switch tools).
The default location is `/opt/devkitpro`; override `DEVKITPRO` if needed.

```sh
make -j4
make test
make package
```

The bundle is `dist/nxhid-0.3.1.zip`. Host tests require a C compiler and local TCP
socket access, but do not need the Switch SDK. Use the bundled `.nro` and Python
sender together.

## License

Licensed under the [MIT License](LICENSE).
