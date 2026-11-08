# Working conventions

How work is done in this repository. Read this, `README.md`,
`TODO/ACTIVE.md` (where things stand, standing decisions, open work),
`docs/plan-v1.md` (design) and `gateware/README.md` (what the FPGA does)
before starting anything; between them they contain every decision already
made. Do not reopen a decision listed under Standing decisions in
`TODO/ACTIVE.md` without saying so.

## Starting a piece of work

1. Pick the first unchecked item in `TODO/ACTIVE.md` whose dependencies
   are done, unless told otherwise. Items are ordered by dependency.
2. Branch from `develop`: `git checkout -b item/<section>-<short-slug>`,
   for example `item/1-rbprobe`.
3. Build and test on the development Pi, not on the workstation. The Pi
   is reached over SSH as root; its address and access details are kept
   outside this repository. `tools/ci-pi.sh --remote [user@]host` builds
   and tests the working tree there; for hardware tests, copy what you
   need to the Pi and run it there. `docs/platform.md` describes the
   machine and what is installed on it.

## Finishing a piece of work

1. The item's acceptance test in `TODO/ACTIVE.md` passes on the Pi.
2. `tools/ci-pi.sh` passes, run on the Pi or from a workstation with
   `tools/ci-pi.sh --remote [user@]host`. `tools/check-format.sh --fix`
   applies clang-format 19, which the workstation may not have; run it on
   the Pi and copy the result back.
3. Tick the item in `TODO/ACTIVE.md`, move its text to the top of
   `TODO/COMPLETED.md` with the date, and keep `README.md` current if the
   item changed anything a user installs or runs.
4. Squash-merge into `develop`:
   `git checkout develop && git merge --squash item/... && git commit`.
   Delete the item branch. `develop` is merged into `main` only when a
   version is tagged.

## Code

- C++20 for everything except the kernel module and the ioctl ABI header,
  which are C. GNU extensions off.
- Every source file starts with `// SPDX-License-Identifier: GPL-2.0-or-later`
  (`/* ... */` in C and headers shared with the kernel). Code taken from
  an upstream work keeps that work's copyright and licence header beneath
  the SPDX line.
- `.clang-format` is authoritative; run `clang-format -i` before
  committing. Warnings are errors.
- Dependencies come only from Debian trixie packages. Adding one means
  adding it to the README requirements and the attribution table.
- Every component has Catch2 tests aiming at complete coverage. Logic that
  touches hardware, files or sockets sits behind an interface so tests use
  fakes and fixtures. New code without tests is not finished.
- No emoji anywhere. No AI or tool attribution anywhere: not in code,
  comments, commit messages, or documentation.
- No file in the repository may contain machine paths, personal names
  (other than upstream authors in licence attributions), addresses,
  hostnames, serials or any other identifier of the development hardware
  or its network. Describe hosts generically.

## Gateware

- Never edit the gateware source. `tools/build-gateware.sh` rebuilds the
  bitfiles on an x86-64 machine with Quartus Prime Lite
  (`gateware/QUARTUS.md`); installing Quartus requires the person running
  it to accept the Quartus licence agreement, so ask before doing it.
- Commit rebuilt bitfiles together with the `PROVENANCE.md` table the
  script updates, and add new hashes to the `rbprobe` catalogue
  (`tools/common/src/gateware_catalog.cpp`).

## Commits

- One logical change per squash commit on `develop`. Subject line in the
  imperative, under 72 characters, body explaining why when it is not
  obvious. No trailers of any kind.
- Never commit build output, `.rbf` files outside `gateware/`, or
  anything from the Pi's filesystem.

## Documentation

- `README.md` describes only the current state: what exists, how to
  install it, how to use it. No history, no plans.
- `docs/plan-v1.md` is the design; change it when a design decision
  changes, and record the change in `TODO/COMPLETED.md`.
- `docs/platform.md` and `docs/pa.md` are the hardware references; update
  them when a measurement or hardware fact changes.
- `TODO/ACTIVE.md` is the single list of open work. Add new items with an
  acceptance test. `TODO/COMPLETED.md` is append-only at the top.

## Safety

- No transmit test on the development board without the owner's explicit
  permission for that test, asked at the time. Anything that sets PTT
  (command bit 32) or the TX permission bit (bit 42), or writes samples
  while either is set, is a transmit test. Receive tests, and TX-path tests
  with both bits clear, need no permission.
- The PA stage is never enabled by default and never without a dummy load
  on the first test of any new code path that can key the transmitter.
- Any code that can assert MOX, PA enable or drive must have a timeout or
  watchdog and a test that proves the fail-safe path clears it.
- Reboots and configuration changes on the development Pi are allowed at
  any time; record anything persistent in `docs/platform.md`.
