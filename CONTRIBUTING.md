# Contributing

Read this, [README.md](README.md), [TODO/ACTIVE.md](TODO/ACTIVE.md) (open
work and standing decisions), [docs/design.md](docs/design.md) and
[gateware/README.md](gateware/README.md) before starting. Do not reopen a
standing decision in [TODO/ACTIVE.md](TODO/ACTIVE.md) without saying so.

## Repository layout

```
docs/      design, driver, driver tests, daemon, tools, platform, PA, examples, fault report, licensing
gateware/  gateware source, built bitfiles per FPGA, provenance, toolchain
driver/    kernel module, device tree overlay, ABI header, logic tests
daemon/    radioberryd: device layer, session, transmit policy, Protocols 1 and 2, systemd files
examples/  ft8rx, an example FT8 client (optional build)
tools/     rbprobe, rbcat, rbctl, rbload, p1probe, p2probe, shared library, driver tests, CI and check scripts, gateware build
TODO/      ACTIVE.md (open work) and COMPLETED.md (done work)
```

## Branches and CI

- Work happens on `develop`, one branch per TODO item
  (`item/<section>-<short-slug>`), squash-merged into it. `main` receives
  `develop` only at version tags.
- Build and test on the Pi, not on the workstation. The Pi's address and
  access details are kept outside this repository. Besides the README's
  packages, development needs `sudo apt install clang-format-19 lcov rsync`
  (format check, coverage report, `ci-pi.sh --remote`).
- [tools/ci-pi.sh](tools/ci-pi.sh) is the merge gate. On the Pi it runs
  the format check ([tools/check-format.sh](tools/check-format.sh),
  clang-format 19), the SPDX check
  ([tools/check-spdx.sh](tools/check-spdx.sh)), the release, sanitizer
  and coverage builds with their tests, the driver hardware tests (skipped
  while another program holds `/dev/radioberry`), and the coverage
  summary; when the ABI header is installed it also builds the tools
  against it and checks that no userspace compile sees `driver/include`.
  From a workstation, `tools/ci-pi.sh --remote [user@]host` copies the
  working tree to the Pi and runs it there (it needs the main checkout,
  not a git worktree).
- GitHub Actions runs `tools/ci-pi.sh --local` in a Debian trixie
  container on every push to `main` and `develop` (no kernel module and no
  hardware there), uploads the coverage report to Codecov, and runs CodeQL.
  The Pi run stays the merge gate.
- `tools/check-format.sh --fix` applies clang-format 19, which the
  workstation may not have; run it on the Pi and copy the result back.
- [CMakePresets.json](CMakePresets.json) has `default`, `asan`
  (AddressSanitizer and UndefinedBehaviorSanitizer) and `coverage`
  presets; `cmake --build --preset coverage --target coverage` writes an
  lcov report to `build-coverage/coverage/html`.

## Finishing a piece of work

1. The item's acceptance test in [TODO/ACTIVE.md](TODO/ACTIVE.md) passes
   on the Pi.
2. [tools/ci-pi.sh](tools/ci-pi.sh) passes.
3. Tick the item, move its text to the top of
   [TODO/COMPLETED.md](TODO/COMPLETED.md), and update the docs it affects.
4. Squash-merge into `develop` and delete the item branch.

## Code

- C++20 for everything except the kernel module and the ioctl ABI header,
  which are C. GNU extensions off. Warnings are errors.
- Every source file starts with `// SPDX-License-Identifier: GPL-2.0-or-later`
  (`/* ... */` in C and headers shared with the kernel). Code taken from
  a third-party work keeps its copyright and licence header beneath it.
- [.clang-format](.clang-format) is authoritative.
- Dependencies come only from Debian trixie packages. Adding one means
  adding it to the README's `apt install` line and
  [docs/licensing.md](docs/licensing.md).
- Every component has Catch2 tests aiming at complete coverage. Logic that
  touches hardware, files or sockets sits behind an interface so tests use
  fakes and fixtures. New code without tests is not finished.
- No emoji anywhere. No AI or tool attribution in code, comments or
  commit messages; the README's AI section is the one statement of it.

## Gateware

- Never edit the gateware source.
  [tools/build-gateware.sh](tools/build-gateware.sh) rebuilds the
  bitfiles on an x86-64 machine with Quartus Prime Lite
  ([gateware/QUARTUS.md](gateware/QUARTUS.md)); installing Quartus
  requires the person running it to accept the Quartus licence agreement,
  so ask before doing it.
- Commit rebuilt bitfiles together with the
  [gateware/PROVENANCE.md](gateware/PROVENANCE.md) table the script
  updates, and add new hashes to the `rbprobe` catalogue
  ([tools/common/src/gateware_catalog.cpp](tools/common/src/gateware_catalog.cpp)).

## Commits

- One logical change per squash commit on `develop`. Subject in the
  imperative, under 72 characters; a body explaining why when it is not
  obvious. No trailers.
- Never commit build output, `.rbf` files outside `gateware/`, or
  anything from the Pi's filesystem.

## Documentation

- Docs state the current facts: what exists, how to install and use it,
  what was measured. No history, no plans (those belong in `TODO/`).
- No dates, personal names, callsigns, machine paths, addresses,
  hostnames, serials or other identifiers of the development hardware or
  its network. Describe hosts generically.
- [README.md](README.md) holds only the overview, status, installation,
  build options, roadmap, the AI statement and the licence;
  everything else goes in `docs/`.
- Name other files in the repository with relative Markdown links.
- [TODO/ACTIVE.md](TODO/ACTIVE.md) is the single list of open work; add
  items with an acceptance test. [TODO/COMPLETED.md](TODO/COMPLETED.md) is
  append-only at the top.

## Safety

- No transmit test on the development board without the board owner's
  explicit permission for that test, asked at the time. Anything that
  sets PTT (command bit 32) or the TX permission bit (bit 42), or writes
  samples while either is set, is a transmit test. Receive tests, and
  TX-path tests with both bits clear, need no permission.
- The PA stage is never enabled by default and never without a dummy load
  on the first test of any new code path that can key the transmitter.
- Any code that can assert MOX, PA enable or drive must have a timeout or
  watchdog and a test that proves the fail-safe path clears it.
- Reboots and configuration changes on the development Pi are allowed at
  any time; record anything persistent in
  [docs/platform.md](docs/platform.md).
