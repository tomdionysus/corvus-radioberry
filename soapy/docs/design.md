# SoapySDR module design

## Reuse

The module's library compiles radioberryd's device layer
(`daemon/src/device/`), `Session`, `TxPolicy` and option parser, and
`tools/common`'s command encoding and Linux device, from the same source
files, position-independent with hidden symbols. The radio is driven as
the daemon drives it, under the same transmit rules. The session has one
owner, the program that opened the device.

spdlog is linked header-only, so the module's logger is its own; it
forwards to SoapySDR's logger.

## Threads

| Thread | Runs | Work |
|---|---|---|
| `rb-soapy-rx` | always | RX pump: commands out, frames into the RX ring |
| `rb-soapy-tx` | always | TX pump: TX ring to the driver |
| `rb-soapy-deliv` | while run is set | RX ring to the read queue |
| `rb-soapy-tick` | while the TX stream is active | watchdog, burst end |

With no stream active run is clear and every thread sleeps without a timer.

## Receive

Run is set while either stream is active. The delivery thread queues RX
blocks of the stream's layout, at most one second of samples; when full,
it drops the queued samples, not the new block, and the next read returns
`SOAPY_SDR_OVERFLOW`. `readStream()` waits until it can fill the request
or the timeout passes, then returns what it has. The channel count is
checked against `max_receivers` at activation and at each rate change.

## Transmit

SoapySDR has no PTT call; stream activation and deactivation are key down
and key up. A burst opens at the first write after activation or after
the last burst and asks `TxPolicy` for MOX; samples are queued only while
MOX is granted. A burst ends on `SOAPY_SDR_END_BURST`, deactivation, close
or destruction, never on a pause in the writes.

MOX is released when the TX ring and the driver's ring are empty and the
image's `tx_samples` counter (sysfs `counters`) has advanced by the
samples written in the burst: until then the module waits for the samples
left at 48 ksps. Limits: 250 ms (`tx_tail_max`), then the burst ends with
a warning; without the counter, a fixed 100 ms (`tx_tail`).

`writeStream()` holds at most 4800 samples (100 ms) between itself and the
gateware and waits, within the caller's timeout, for room.

## Faults

| Fault | Effect |
|---|---|
| device error | every stream call fails; MOX dropped and latched |
| RX overrun, read queue full | `SOAPY_SDR_OVERFLOW` on the next read |
| RX resync, dropped words, stall | logged |
| TX underrun | logged at debug |
| watchdog, stream fault while keyed | writes fail until the burst ends |
