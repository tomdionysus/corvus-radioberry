# Working conventions

How work is done in this repository. Read this, `README.md`,
`docs/plan-v1.md` and `TODO/ACTIVE.md` before starting anything; between
them they contain every decision already made. Do not reopen a decision
listed under Standing decisions in `TODO/ACTIVE.md` without saying so.

## Starting a piece of work

1. Pick the first unchecked item in `TODO/ACTIVE.md` whose dependencies
   are done, unless told otherwise. Items are ordered by dependency.
2. Branch from `develop`: `git checkout -b item/<section>-<short-slug>`,
   for example `item/1-rbprobe`.
3. Build and test on the development Pi, not on the workstation. The Pi
   is reached over SSH; its address and access details are not in this
   repository.

## Finishing a piece of work

1. The item's acceptance test in `TODO/ACTIVE.md` passes on the Pi.
2. `tools/ci-pi.sh` passes (once it exists; until then run the build,
   tests, format check and SPDX check by hand).
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

- The PA stage is never enabled by default and never without a dummy load
  on the first test of any new code path that can key the transmitter.
- Any code that can assert MOX, PA enable or drive must have a timeout or
  watchdog and a test that proves the fail-safe path clears it.
- Reboots and configuration changes on the development Pi are allowed at
  any time; record anything persistent in `docs/platform.md`.
