# Building the gateware: Quartus Prime Lite

The gateware is compiled with Quartus Prime Lite Edition, the free edition
of Altera's FPGA toolchain (some documentation still carries Intel's
name). Lite supports the Cyclone 10 LP devices on the Radioberry and needs no licence
file or licence server. What it does need is acceptance of the Quartus
Prime end-user licence agreement, which each person building the gateware
gives themselves when downloading and installing. This repository does not
redistribute Quartus and its scripts never accept the agreement on anyone's
behalf.

## What to download

From the Altera FPGA download centre
(https://www.altera.com/downloads, Quartus Prime Lite Edition, version
23.1std.1, Linux), the individual files rather than the combined archive:

| File | Contents |
|---|---|
| `QuartusLiteSetup-23.1std.1.993-linux.run` | the Quartus Prime Lite installer |
| `cyclone10lp-23.1std.1.993.qdz` | Cyclone 10 LP device support |

Questa, the help files and the other device families are not needed.
Downloading requires accepting the licence agreement on the download page;
an Altera account may be requested. Version 23.1std.1 is the one the
upstream project files were last saved with; a later Lite release with
Cyclone 10 LP support is expected to build them too, but the hashes in
[gateware/PROVENANCE.md](PROVENANCE.md) are for 23.1std.1.

Quartus is x86-64 Linux (or Windows) software. It does not run on the
Raspberry Pi or natively on macOS.

## Building with Docker (any x86-64 host, including Intel macOS)

1. Put the two files above in one directory, for example
   `~/Downloads/quartus-23.1std.1`.
2. Run, from the repository root:

   ```
   tools/build-gateware.sh --installers ~/Downloads/quartus-23.1std.1 \
       --accept-quartus-eula
   ```

   `--accept-quartus-eula` states that you have read and accepted the
   Quartus Prime licence agreement; without it the script stops before
   installing anything. The first run builds a Docker image
   `radioberry-quartus:23.1std.1` with Quartus installed (about 10 GB of disk); later
   runs reuse it.
3. The script compiles `radioberry_pio_cl025` and `radioberry_pio_cl016`
   from `gateware/source/` with the fitter seeds recorded in
   [gateware/PROVENANCE.md](PROVENANCE.md), checks timing, copies each
   `radioberry.rbf` to `gateware/rpi-5/<type>/`, and updates the table in
   [gateware/PROVENANCE.md](PROVENANCE.md) with
   seeds, sizes and hashes. Each compile takes about four minutes on a
   six-core machine. Logs and timing reports are left in
   `build-gateware/`. `--seed-search N` tries seeds 1 to N instead, for a
   new Quartus version or source commit; `--only CL025|CL016` builds one
   variant.

## Building on an x86-64 Linux machine without Docker

Install Quartus yourself:

```
chmod +x QuartusLiteSetup-23.1std.1.993-linux.run
./QuartusLiteSetup-23.1std.1.993-linux.run --mode unattended \
    --unattendedmodeui none --installdir "$HOME/altera_lite/23.1std" \
    --accept_eula 1
```

(`--accept_eula 1` records your own acceptance of the agreement; leave
it out to read and accept it interactively.) Put the `.qdz` next to the
installer before running it so the Cyclone 10 LP support is installed with
it. Then:

```
tools/build-gateware.sh --quartus "$HOME/altera_lite/23.1std/quartus"
```

## After building

Commit the two `.rbf` files and the updated
[gateware/PROVENANCE.md](PROVENANCE.md) together.
`cmake --install` then installs both images to
`/usr/local/share/radioberry/gateware/rpi-5/` and the one for `FPGATYPE`
as `/lib/firmware/radioberry.rbf`.
