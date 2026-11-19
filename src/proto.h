#pragma once

// The Mac-facing data channel: FM-1 Transporter's line protocol (src/transporter_proto.h, tools/fm1t.py,
// MvaveFM1Unbricker fm1_transporter_recover.py), on SEGGER RTT channel 0 over the Discovery board's ST-LINK
// (OpenOCD's RTT server turns it into a TCP port) instead of a USB CDC port or a UART.
//   ping, status, uboot, rekey, info, read, wsec, ldr, memw, memr, jump, runapp: as the Transporter
//   log -> LOG <n>\n <n bytes of the board's log> END\n   (this board's own, for debugging)
// As fm1-esp32-unbrick: `status` is held back until the FM-1 is in UBOOT (or 20 s after the host started).

void proto_start(void);     // RTT channel 0
void proto_rx_poll(void);   // takes what arrived on RTT: answers ping / log / rekey at once, queues the rest
void proto_poll(void);      // one pending request, else the loader keep-alive
