# Reporting an issue

Open an issue at
https://github.com/tomdionysus/corvus-radioberry/issues. Check
[Troubleshooting](troubleshooting.md) first.

Include:

1. What you did, what you expected, and what happened.
2. The client and protocol (for example piHPSDR, Protocol 1), and the
   receiver count and sample rate.
3. The output of:

   ```
   rbprobe --yaml
   dpkg -l 'radioberry*'
   journalctl -u radioberry -b --no-pager | tail -n 200
   sudo dmesg | grep -i radioberry
   ```

4. `/etc/radioberry/config.yaml`, if you changed it.

Remove anything you'd rather not publish before posting, such as the Pi's
serial number and network addresses in the `rbprobe` output.

Paste output in a code block (between lines of three backticks).
