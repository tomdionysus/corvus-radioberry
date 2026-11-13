# Raspberry Pi 4: desktop lost while transmitting

Status: open. Found in the Pi 4 acceptance test of branch `item/rpi4-smi`
on 2026-11-13. Not to be released until closed.

## Symptom

piHPSDR and WSJT-X on the Pi 4's own desktop, used over VNC on the Pi's
Wi-Fi, SMI transport. On each transmission (Protocol 2, and Protocol 1)
the desktop stops responding until the transmission ends; MOX cannot be
released from the desktop meanwhile. Stopping `radioberryd` ended a stuck
transmission and the desktop recovered at once. The owner did not notice
it in the morning's FT8 contacts on the same build, and did not see it
in the 0.4.0 acceptance test on the `cpu` transport.

## What was measured

All on the Pi 4, kernel 6.18.60, gateware 75.3.

| Step | Build | Result |
|---|---|---|
| 1 | daemon with TX gated to MOX and batched sends (`6fc7958`), driver `c3c96c1` | desktop lost on every Protocol 2 transmission; piHPSDR printed `full_tx_buffer ... fexchange0: error`; one transmission dropped 58 TX blocks |
| 2 | code read | `rb_bb_tx()` reported busy while the gateware's TX FIFO was full, so the TX thread (SCHED_FIFO 50) looped on `cond_resched()`; Protocol 2's 100 ms TX latency keeps that 85 ms FIFO full |
| 3 | driver with that fixed (`f05d897`) | desktop still lost. During a transmission the TX thread used 55 to 70% of a core in `rb_bb_tx` (perf: 13.9% of all samples), the system 0 to 8% idle with 10 to 15 runnable |
| 4 | same, driver thread SCHED_OTHER, daemon `realtime: false` | desktop still lost. Across one transmission: a normal-priority task on the Pi woke at most 5 ms late every second, CPU 17 to 60% idle; pings to the Pi's Wi-Fi from the workstation every 0.2 s were lost or answered 0.5 to 4.8 s late from PTT on to PTT off, 2 to 14 ms before and after |
| 5 | morning packages (`0.4.1+git16.gc3c96c1`) reinstalled | owner: desktop still lost on transmit |
| 6 | same, no RF: `rbcat --rate 192 --seconds 15`, SMI receive only | Wi-Fi ping 90/90, slowest 9 ms |
| 7 | same, no RF: `rbcat --rate 192 --seconds 15 --tx-zeros` (TX clocked by the CPU at 48 000 words/s, PTT clear) | Wi-Fi ping 90/90, slowest 9 ms |
| 8 | same: `p1probe --rate 192 --mox --seconds 15` through `radioberryd` (PTT and T/R relay keyed, drive 0, PA enable clear, zero samples) | Wi-Fi ping 100/100, slowest 9 ms; no under-voltage; 48 120 TX samples/s, 0 TX blocks dropped |

The Pi reported under-voltage (`hwmon1: Undervoltage detected!`) during
every transmission with RF in steps 1 to 4, with the CPU clock held at
1.5 GHz; none in step 8.

## What it rules out so far

- The Pi stalling: step 4 shows it responsive while the link is gone.
- The real-time priorities: step 4.
- SMI receive, its DMA, and the CPU clocking TX words, alone or
  together: steps 6 and 7.
- Keying PTT and the T/R relay through the daemon: step 8.

Left: the PA switched on (`envpa`), and the transmitted RF.

## Next steps

1. PA enabled at drive 0, no RF: piHPSDR with drive 0 and the PA enabled,
   or a probe option to set PA enable, with the Wi-Fi ping and the kernel
   log. If the link dies, it follows the PA's supply.
2. Normal drive into the dummy load, same ping. If the link survives,
   it follows the radiated RF.
3. The same transmission with the desktop on a local monitor or the Pi on
   Ethernet, to confirm the radio side is unaffected when the Wi-Fi is not
   in the path.
4. Repeat step 2 on the `cpu` transport (no `radioberry-smi` overlay) to
   compare with the 0.4.0 acceptance test.

Every step that transmits needs the owner's go-ahead at the time.

## Also found

- CPU-clocked TX costs 55 to 70% of a core for the length of a
  transmission (step 3); the TODO item for the `cpu` transport's timer
  sleep does not cover TX.
- A client that reconnects still holding MOX keys the transmitter at
  once: after the daemon restarted, piHPSDR, still in MOX from a stuck
  transmission, transmitted for 6.6 s with nobody keying. The daemon
  should require MOX released before granting it to a new session.
- `6fc7958` (TX gating, batched sends) and `f05d897` (TX thread sleeps on
  a full FIFO) are on the branch; the Pi 4 runs `0.4.1+git16.gc3c96c1`
  again. Whether they stay is decided when this item closes.
