# Debian packages

The `debian/` directory builds Debian packages for a Raspberry Pi 4 or
Raspberry Pi 5 running Raspberry Pi OS (Debian 13 trixie, 64-bit); the
same packages serve both.

## Packages

| Package | Architecture | Contents |
|---|---|---|
| `radioberry` | arm64 | `radioberryd` and `radioberry.service`, `rbprobe`, `rbcat`, `rbctl`, `p1probe`, `p2probe`, sysusers.d entry, udev rule, modules-load.d entry for `i2c-dev`, `/etc/radioberry/config.yaml`, the gateware images, the overlay |
| `radioberry-dkms` | all | the driver source, built by DKMS for every installed kernel |
| `radioberry-dev` | all | `radioberry_ioctl.h` |
| `radioberry-dbgsym` | arm64 | debug symbols for `radioberry` |

`radioberry` depends on `radioberry-dkms` of the same version.
`radioberry-dkms` recommends `linux-headers-rpi-v8 | linux-headers-rpi-2712`,
which apt installs by default; DKMS needs the headers of each kernel it
builds for.

## Building

On the Pi, from the repository root:

```
sudo apt install dpkg-dev
sudo apt build-dep ./
tools/build-deb.sh
```

`apt build-dep ./` installs the build dependencies listed in
`debian/control`. [tools/build-deb.sh](https://github.com/tomdionysus/corvus-radioberry/blob/main/tools/build-deb.sh) writes
`debian/changelog`, runs `dpkg-buildpackage -us -uc -b` (unsigned, binary
only) with the unit tests, and moves the packages to `build-deb/`.
`DEB_BUILD_OPTIONS=nocheck tools/build-deb.sh` skips the tests.

The version comes from `git describe`: tag `v0.2.0` gives `0.2.0`, a build
three commits after it `0.2.0+git3.gaff2d9d`, which sorts after `0.2.0` and
before `0.2.1`. A build with uncommitted changes ends in `.dirty`.

## Installing

```
sudo apt install ./build-deb/radioberry_*_arm64.deb \
    ./build-deb/radioberry-dkms_*_all.deb
```

The install does the following:

| What | Where |
|---|---|
| kernel module, built by DKMS for each installed kernel | `/lib/modules/<kernel>/updates/dkms/radioberry.ko.xz` |
| gateware images | `/usr/share/radioberry/gateware/<image>/CL025/`, linked as `/usr/lib/firmware/radioberry-rpi5-pio.rbf` and `radioberry-rpi4-i2s.rbf` |
| overlay | copied from `/usr/share/radioberry/overlays/` to `/boot/firmware/overlays/radioberry.dtbo` |
| tools | `/usr/bin/` |
| `radioberryd` | `/usr/sbin/` |
| unit, sysusers.d entry, udev rule | `/usr/lib/systemd/system/`, `/usr/lib/sysusers.d/`, `/usr/lib/udev/rules.d/` |
| `i2c-dev` loaded at boot | `/usr/lib/modules-load.d/radioberry.conf` |
| daemon config (conffile) | `/etc/radioberry/config.yaml` |

The install does not edit `/boot/firmware/config.txt`. If it has no
`dtoverlay=radioberry` line the install prints a reminder; add

```
dtparam=i2c_arm=on
dtoverlay=radioberry
```

and reboot. `radioberry.service` is enabled at install and started then if
`/dev/radioberry` exists, otherwise at the next boot. Check with `rbprobe`.

Transmit and the PA are off in the installed `config.yaml`
([Quickstart](quickstart.md#3-configure)).

### Upgrades and kernel updates

An upgrade of `radioberry-dkms` builds the new module for every installed
kernel. `radioberry` then loads it in place of the running one (stopping
`radioberryd` meanwhile) and restarts `radioberryd`. If the old module is
in use by another program, the upgrade says so and the new one loads at
the next reboot.

A kernel update triggers DKMS to build the module for the new kernel,
provided the new kernel's headers are installed (the
`linux-headers-rpi-v8` and `linux-headers-rpi-2712` metapackages follow
the kernel).

dpkg keeps a changed `/etc/radioberry/config.yaml` on upgrade and asks
when the packaged default changed as well.

### Removing

`sudo apt remove radioberry radioberry-dkms` stops the service and removes
the programs, the DKMS modules, the overlay and the firmware links. The
module stays loaded until the next reboot. `config.yaml` and the
`radioberry` user remain; `sudo apt purge radioberry` deletes
`/etc/radioberry`. The `dtoverlay=radioberry` line in `config.txt` is left
for you to remove.

### Replacing a `cmake --install` installation

Remove the files `cmake --install` placed before installing the packages:
the module it put in `/lib/modules/<kernel>/updates/` would compete with
DKMS's copy in `updates/dkms/`, and its unit in `/usr/local` would take
precedence over the package's. From the build directory used for that
install:

```
sudo systemctl disable --now radioberry
xargs -d '\n' sudo rm -f < build/install_manifest.txt
sudo depmod -a
sudo systemctl daemon-reload
```

`install_manifest.txt` does not list `/etc/radioberry/config.yaml`, which
stays. Install the packages with
`sudo apt -o Dpkg::Options::=--force-confold install ...` to keep it
without a prompt.

## Running your own apt repository

An apt repository is a directory of static files: the packages under
`pool/`, and under `dists/` the package indices and a `Release` file
signed with a GPG key. Any HTTP or HTTPS server can host it. The steps
below use `reprepro`; `aptly` does the same job.

### 1. Signing key

Create the key on a machine you keep safe; it signs every update.

```
gpg --quick-gen-key "corvus-radioberry archive key" ed25519 sign 2y
gpg --list-keys "corvus-radioberry archive key"   # note the fingerprint
```

Keep the private key out of the published directory and backed up. Before
the expiry date, extend it (`gpg --quick-set-expire FINGERPRINT 2y`) and
publish the sources file again with the re-exported key (step 5); apt
refuses a repository signed by an expired key.

### 2. Repository configuration

```
sudo apt install reprepro
mkdir -p repo/conf
```

`repo/conf/distributions`:

```
Origin: corvus-radioberry
Label: corvus-radioberry
Codename: trixie
Architectures: arm64
Components: main
Description: corvus-radioberry packages for Raspberry Pi OS
SignWith: FINGERPRINT
```

`repo/` holds reprepro's configuration and database. The published files
go to a separate directory, `public/` below.

### 3. Adding packages

```
reprepro --basedir repo --outdir ./public includedeb trixie build-deb/*.deb
```

Run the same `includedeb` for each new build: reprepro keeps one version
of each package per distribution and replaces the older one.
`reprepro --basedir repo list trixie` shows what is in it.

### 4. Hosting

Copy `public/` to any static web host, for example a web server's
document root, an object storage bucket served over HTTPS, or GitHub
Pages from a separate repository (a file there may be at most 100 MB;
the largest package, `radioberry-dbgsym`, is about 7 MB). Publish only
`public/`, never `repo/` or the private key.

### 5. The sources file

Put a `radioberry.sources` file in `public/`, with the public key inside
it so users fetch one file. With `https://example.org/radioberry/`
standing for where `public/` is served:

```
Types: deb
URIs: https://example.org/radioberry/
Suites: trixie
Components: main
Signed-By:
 -----BEGIN PGP PUBLIC KEY BLOCK-----
 .
 (the output of gpg --export --armor FINGERPRINT, each line indented by
 one space, an empty line written as " .")
 -----END PGP PUBLIC KEY BLOCK-----
```

On each Pi:

```
sudo curl -fsSLo /etc/apt/sources.list.d/radioberry.sources https://example.org/radioberry/radioberry.sources
sudo apt update && sudo apt install radioberry
```

Later builds arrive with `sudo apt update && sudo apt upgrade`.
