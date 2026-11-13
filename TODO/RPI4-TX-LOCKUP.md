# Raspberry Pi 4: desktop lost while transmitting

Status: open. Found in the Pi 4 acceptance test of branch `item/rpi4-smi`
on 2026-11-13. Blocks the release.

## Symptom

piHPSDR and WSJT-X on the Pi 4's own desktop, used over VNC on the Pi's
Wi-Fi, SMI transport. On transmit the desktop stops responding, at 192
and at 48 ksps; MOX cannot be released from the desktop meanwhile, and
once the owner had to cut the power to end it. Stopping `radioberryd`
ended a transmission and the desktop recovered at once. The 0.4.0
acceptance test on the `cpu` transport had no lock-ups.

## The owner's assessment

The fault is in the TX path, interacting with the SMI and its DMA. The
owner also believes the transmitted data is corrupt. Both are the lines
to follow; the owner directs each step.

## What was measured

All on the Pi 4, kernel 6.18.60, gateware 75.3, SMI transport.

| Step | Build | Result |
|---|---|---|
| 1 | daemon with TX gated to MOX and batched sends (`6fc7958`), driver `c3c96c1` | desktop lost on every Protocol 2 transmission; piHPSDR printed `full_tx_buffer ... fexchange0: error`; one transmission dropped 58 TX blocks |
| 2 | code read | `rb_bb_tx()` reported busy while the gateware's TX FIFO was full, so the TX thread (SCHED_FIFO 50) looped on `cond_resched()`; Protocol 2's 100 ms TX latency keeps that 85 ms FIFO full |
| 3 | driver with that fixed (`f05d897`) | desktop still lost. During a transmission the TX thread used 55 to 70% of a core in `rb_bb_tx` (perf: 13.9% of all samples), the system 0 to 8% idle with 10 to 15 runnable |
| 4 | same, driver thread SCHED_OTHER, daemon `realtime: false` | desktop still lost. A normal-priority task on the Pi woke at most 5 ms late each second, CPU 17 to 60% idle; pings to the Pi's Wi-Fi from the workstation every 0.2 s lost or 0.5 to 4.8 s late from PTT on to PTT off, 2 to 14 ms before and after |
| 5 | morning packages (`0.4.1+git16.gc3c96c1`: driver `c3c96c1`, daemon before `6fc7958`) reinstalled | desktop still lost on transmit |
| 6 | same, no RF: `rbcat --rate 192 --seconds 15`, SMI receive only | Wi-Fi ping 90/90, slowest 9 ms |
| 7 | same, no RF: `rbcat --rate 192 --seconds 15 --tx-zeros` (TX words clocked by the CPU at 48 000 per second, PTT clear) | Wi-Fi ping 90/90, slowest 9 ms |
| 8 | same: `p1probe --rate 192 --mox --seconds 15` through `radioberryd` (PTT and T/R relay keyed, drive 0, PA enable clear, zero samples) | Wi-Fi ping 100/100, slowest 9 ms; no under-voltage; 48 120 TX samples/s, 0 TX blocks dropped |
| 9 | same, piHPSDR and WSJT-X calling on Protocol 2 | six transmissions of 13.6 to 13.9 s, 0 TX blocks dropped, WSJT-X starting one each period; pings lost for 6 s from PTT on in the first, 0.4 to 7 s late in the next four, lost from the sixth onward including the 17 s between transmissions, until the owner cut the power; wake-up lateness on the Pi 1 to 10 ms (one 20.7 ms); the kernel logged `sched: DL replenish lagged too much` once, in a 5 s ping stall |
| 10 | same, piHPSDR and WSJT-X, 192 then 48 ksps (owner) | ten transmissions from 14:32:15, 13.8 to 14.0 s, 47 633 to 48 024 samples/s; the first dropped 100 TX blocks, the rest none. The Pi's own ping to its router, each second: 1 to 11 ms outside transmissions; during each, lost in 1 to 4 seconds and 50 to 981 ms in others. Wi-Fi signal unchanged (-61 to -62 dBm), TX bitrate 72.2 Mb/s dipping to 65; throttle flags 0x0 throughout (no under-voltage) and no kernel message; wake-up lateness 1 to 16 ms, 44.7 to 79.5 ms in the second after PTT on. Owner: the desktop locked at 48 ksps too, and the TX data looked corrupt |
| 11 | released 0.4.1 (driver and daemon), `cpu` transport, SMI overlay removed, same config; piHPSDR and WSJT-X transmitting at 48 ksps and at the higher rate (owner) | owner: noticeable slowdown, but no lock-up of the desktop or the network at either rate |

The Pi reported under-voltage (`hwmon1: Undervoltage detected!`) during
the transmissions in steps 1 to 4; none in steps 8 and 10.

## Facts so far

- The Pi's network fails during transmissions with RF (steps 4, 9, 10),
  seen from the workstation and from the Pi itself, while the Pi's
  scheduling stays responsive.
- Not reproduced without RF: SMI receive with TX words clocked at
  48 000 per second (step 7), or PTT keyed at drive 0 with the PA off
  (step 8).
- Under-voltage was not present in step 10, which still failed.
- The `cpu` transport (released 0.4.1) does not lock up on transmit, at
  48 ksps or at the higher rate, on the same Pi, board, config and
  clients (step 11); it slows the system down. The lock-up comes with the
  SMI transport.
- The owner rules out Wi-Fi as the cause.

## Open

1. TX data corruption: compare what the gateware is given with what the
   client sent (the driver's TX words against the client's samples), and
   look at the transmitted signal.
2. How the TX path interacts with the SMI and its DMA while transmitting:
   what differs between step 7 (TX words clocked, PTT clear) and a real
   transmission on the SMI side.
3. The slowdown on the `cpu` transport while transmitting (step 3: the
   CPU-clocked TX thread holds 55 to 70% of a core).

Every step that transmits needs the owner's go-ahead at the time.

## Also found

- CPU-clocked TX costs 55 to 70% of a core for the length of a
  transmission (step 3); the TODO item for the `cpu` transport's timer
  sleep does not cover TX.
- A client that reconnects still holding MOX keys the transmitter at
  once: after the daemon restarted, piHPSDR, still in MOX, transmitted for
  6.6 s with nobody keying. The daemon should require MOX released before
  granting it to a new session.
- `6fc7958` (TX gating, batched sends) and `f05d897` (TX thread sleeps on
  a full FIFO) are on the branch; the Pi 4 runs the released 0.4.1.
  Whether they stay is decided when this item closes.
