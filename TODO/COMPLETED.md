# Completed work

Newest first. Each entry gives the date, what was done, and where the
result lives.

- 2026-11-08: v1 plan written to `docs/plan-v1.md`: fixed constraints,
  component architecture, driver and daemon design with the upstream
  defects each part addresses, performance targets, testing policy and
  order of work. Standing decisions recorded in `TODO/ACTIVE.md`.
- 2026-11-08: FPGA identified as a 10CL025 by loading the upstream rpi-5
  CL016 and CL025 images with the libgpiod 2 loader on the Pi: CL016 left
  CONF_DONE low, CL025 configured. Found that the installed
  `/lib/firmware/radioberry.rbf` is the rpi-4 CL016 image, wrong on both
  counts. Recorded in `docs/platform.md`.
- 2026-11-08: Repository initialised on branch `main` with the GitHub
  remote. GPL-2.0-or-later chosen and `LICENSE` added. README written with
  scope, protocols, DSP choice and third-party attributions. `TODO/`
  created.
- 2026-11-08: I2C enabled on the development Pi (`dtparam=i2c_arm=on`,
  `i2c-dev` module) and the Pi rebooted. `i2cdetect -y 1` shows no devices:
  the PA5W V2 has no MAX11613 and no MCP4662. Recorded in `docs/pa.md` and
  `docs/platform.md`.
- 2026-11-08: Development Pi surveyed over SSH; result in
  `docs/platform.md`. Established that the stock 6.18 `rp1-pio` lacks
  `rp1_pio_sm_buffer_virt`, which is the only API the upstream driver uses
  that does not exist, and that stock `rp1_pio_sm_xfer_data` bounces
  through its own DMA buffers.
- 2026-11-08: Amplifier board identified and documented in `docs/pa.md`:
  AURSINC Radioberry Preamp Board V2 (PA5W V2), manual bias, INA199A1
  current sense, lineage from the PA3GSB 5 W preamp; hish8612 PA30 hat and
  JI1UDD HL2-PA30 schematics read and summarised.
- 2026-11-08: Upstream projects Radioberry-2.x and radioberry-p2 surveyed;
  result in `docs/history.md`.
