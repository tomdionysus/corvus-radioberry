# Toolchain: Quartus Prime Lite

The gateware is compiled with Quartus Prime Lite Edition 23.1std.1, the
free edition of Altera's FPGA toolchain (some documentation still carries
Intel's name). Lite supports the Cyclone 10 LP and needs no licence file
or server. It does need acceptance of the Quartus Prime end-user licence
agreement, which each person building the gateware gives themselves. This
repository does not redistribute Quartus, and its scripts never accept the
agreement on anyone's behalf.

Quartus is x86-64 Linux (or Windows) software. It does not run on a
Raspberry Pi or on an ARM Mac.

## What to download

From the Altera download centre (https://www.altera.com/downloads,
Quartus Prime Lite Edition, version 23.1std.1, Linux), the individual files
rather than the combined archive:

| File | Contents |
|---|---|
| `QuartusLiteSetup-23.1std.1.993-linux.run` | the Quartus Prime Lite installer |
| `cyclone10lp-23.1std.1.993.qdz` | Cyclone 10 LP device support |

Questa, the help files and the other device families are not needed.
Another Lite release with Cyclone 10 LP support should build the projects
too, but gives different bytes from the hashes in
[provenance.md](provenance.md).

## With Docker

Put both files in one directory, then, from `gateware/`:

```
tools/quartus-image.sh --installers DIR --accept-quartus-eula
```

`--accept-quartus-eula` states that you have read and accepted the
licence agreement; without it the script stops before installing
anything. It builds `radioberry-quartus:23.1std.1` from
[tools/quartus.Dockerfile](../tools/quartus.Dockerfile) with only those two
files as the build context. [tools/build.sh](../tools/build.sh) and
[tools/worst.sh](../tools/worst.sh) run in that image.

## Without Docker

On an x86-64 Linux machine, put the `.qdz` next to the installer and run:

```
chmod +x QuartusLiteSetup-23.1std.1.993-linux.run
./QuartusLiteSetup-23.1std.1.993-linux.run --mode unattended \
    --unattendedmodeui none --installdir "$HOME/altera_lite/23.1std" \
    --accept_eula 1
```

(`--accept_eula 1` records your own acceptance; leave it out to read and
accept the agreement interactively.) Then compile a project the way
`tools/build.sh` does: copy `projects/<image>/` to a working directory two
levels below `gateware/`, unpack the source archive there as `hl2/`, and
run `quartus_sh --flow compile <image>` with Quartus's `bin` directory on
`PATH`.
