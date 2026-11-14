# Daemon design

- The device layer (`src/device/`) sits behind an interface over
  `/dev/radioberry`, so protocol and policy tests run against a fake
  device. Two device threads (`rb-rx-device`, `rb-tx-device`) move data
  between the driver and rings; a sender thread hands RX blocks to both
  protocol servers; the main thread serves Protocol 1 and runs the session
  and policy timers; a separate thread serves the Protocol 2 sockets.
- Protocol code sets command registers (`src/device/control.*`); the RX
  device thread sends them between its reads, so it always knows the frame
  layout it reads, and stamps PTT, run, the TX permission bit and PA
  allowed onto each from one transmit state.
- The monitor (`src/device/monitor.*`) turns successive driver statistics
  into faults: every counter that moved, and an RX stall when no frame
  arrives for 250 ms while frames are expected.
- Protocol 1 (`src/p1/`), Hermes-Lite 2 dialect: discovery, start and stop,
  1032-byte frames at 48 to 384 ksps within the board's RX capacity, the
  C0 command set including PA enable and drive, UDP and TCP.
- Protocol 2 (`src/p2/`): discovery, General, DDC and DUC specific, High
  Priority both ways, as many DDCs as the gateware has receivers, DDC I/Q
  at 48 to 384 ksps, DUC I/Q at 192 ksps decimated to 48 ksps with
  liquid-dsp (`src/dsp/`), sequence checking.
- Session (`src/session.*`): one client owns the radio. Timeout or any
  stream fault clears MOX, zeroes drive, latches a fault the client clears
  by releasing MOX, and logs the cause.
- Transmit policy (`src/tx_policy.*`): transmit and PA enable only when the
  configuration allows them and the client asks; a drive ceiling and a
  transmit watchdog; every PA state change logged. No interlock is claimed.
  The gateware's own levers are the permission bit, PA allowed and the
  KEYING register, which the driver keeps alive only while the daemon
  keys, and the Hermes-Lite 2 core's PTT hang-time watchdog, which is
  never disabled.
- Idle: with no client every thread blocks until a packet arrives, a
  command is queued or it is stopped; nothing polls.
