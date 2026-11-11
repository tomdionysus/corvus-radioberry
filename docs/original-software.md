# Coming from the original software

The [original RadioBerry software](https://github.com/pa3gsb/Radioberry-2.x)
uses the same names as this one
(`radioberry.ko`, `radioberry.dtbo`, `/dev/radioberry`,
`radioberry.service`), so the two cannot share a Pi. A Pi that has had
both ends up with no `/dev/radioberry`, or with the original daemon
running against this driver.

Remove the original's files before installing this software, or after,
if `rbprobe` reports the driver missing:

```
sudo systemctl disable --now radioberry
sudo rm -f /etc/systemd/system/radioberry.service /etc/init.d/radioberryd \
    /usr/local/bin/radioberry /usr/local/include/radioberry_ioctl.h
sudo rm -f /lib/modules/*/kernel/drivers/sdr/radioberry.ko
sudo depmod
sudo systemctl daemon-reload
```

Its overlay and gateware (`/boot/firmware/overlays/radioberry.dtbo`,
`/lib/firmware/radioberry.rbf`) are replaced when this software is
installed. Keep `dtoverlay=radioberry` in `config.txt` and add
`dtparam=i2c_arm=on`.

Then install this software ([Quickstart](quickstart.md)), reboot, and
check that `rbprobe` exits 0.
