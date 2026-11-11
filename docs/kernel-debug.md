# Driver under a debug kernel

Run on 2026-11-09, with driver 0.4.0 and gateware 75.3 (CL025).

`radioberry.ko` was run on a kernel built with the kernel's runtime
checkers, to find what static checks (`checkpatch.pl`, `sparse`) and the
userspace tests cannot: memory errors, locking mistakes, sleeping in atomic
context, DMA API misuse and leaks. The driver drew no reports. Two bugs
were found in the Raspberry Pi kernel itself.

## Kernel

The Raspberry Pi kernel, branch `rpi-6.18.y` at commit
`2dd09405644471652fa15ff93c2c27c384cf6135` (6.18.60, the same version as
the stock Raspberry Pi OS kernel), configured as the stock kernel
(`/boot/config-$(uname -r)`) with these changes:

| Option | Checks |
|---|---|
| `KASAN`, `KASAN_GENERIC`, `KASAN_INLINE` | out-of-bounds and use-after-free accesses |
| `PROVE_LOCKING`, `DEBUG_LOCKDEP`, `LOCK_STAT`, `DEBUG_SPINLOCK`, `DEBUG_MUTEXES` | lock ordering, deadlocks, lock misuse |
| `DEBUG_ATOMIC_SLEEP` | sleeping while atomic |
| `DMA_API_DEBUG` | DMA mapping misuse |
| `DEBUG_KMEMLEAK` (on at boot) | unreferenced allocations |
| `UBSAN`, `UBSAN_BOUNDS` | undefined behaviour, array bounds |
| `SLUB_DEBUG`, `SLUB_DEBUG_ON`, `DEBUG_LIST` | allocator and list corruption |
| `LOCALVERSION="-rbdebug"`; `DEBUG_INFO_BTF` and module signing off | a separate release |

## Method

1. Build the kernel, its modules and device trees on the Pi
   (`make olddefconfig`, then `make Image modules dtbs`; about an hour on
   four cores).
2. Install beside the stock kernel: `make INSTALL_MOD_STRIP=1
   modules_install` (its own `/lib/modules/6.18.60-rbdebug+`), and the
   image as `/boot/firmware/kernel_rbdebug.img`.
3. Build the driver against it
   (`make -C /lib/modules/6.18.60-rbdebug+/build M=<copy of driver/kernel>
   RB_INCLUDE=<driver/include> modules`), install it in that release's
   `updates/` and run `depmod`.
4. Write `/boot/firmware/tryboot.txt`: `config.txt` with
   `kernel=kernel_rbdebug.img` and `auto_initramfs=0` (the stock kernel
   has NVMe and ext4 built in, so no initramfs is needed). Boot it once
   with `reboot "0 tryboot"`; any later reboot or power cycle returns to
   the stock kernel, so a debug kernel that fails cannot strand the Pi.
5. Run the session below, read the kernel log and kmemleak's report, then
   `reboot` to the stock kernel.

Nothing in the session sets PTT or the TX permission bit.

## Session

| Step | Result |
|---|---|
| Boot: the driver loads the gateware and binds | gateware 75.3, 10CL025, 6 receivers |
| `rbdrivertest` (all 11 tests), three runs and one more after the session | passed every time |
| Gateware reload through sysfs, twice | both succeeded, identity read back |
| Module unload and load, three cycles | all succeeded |
| `rbload` with the driver unloaded, then `modprobe radioberry load_gateware=0` | configured (23.4 s under KASAN, 4.7 s on the stock kernel); the driver bound to the loaded image |
| `rbcat`: three receivers at 384 ksps, 5 s | 380 467 frames/s (expected 384 000), no resyncs or overruns |
| `rbcat`: one receiver at 48 ksps with zero TX samples, 5 s | 47 992 frames/s, TX 48 018 words/s |
| `radioberryd` with `p1probe`: three receivers at 384 ksps; two at 192 ksps | 7 592 and 2 652 packets/s (expected 7 680 and 2 667), no sequence breaks, passed |
| `radioberryd` with `p2probe`: six DDCs at 48 ksps; three at 192 ksps | 1 205 and 2 407 packets/s (expected 1 210 and 2 420), no sequence breaks, passed |
| `rbcat` at six receivers, 192 ksps, killed after 1 s, five times; then the close test | device closed cleanly each time; the gateware reported no transmit state |
| kmemleak scan | 21 records from `rp1_pio_sm_xfer_data()` (below) |

Rates under KASAN run about 1% below nominal because the kernel is
slower; on the stock kernel they are exact ([Test results](testing.md)). The probes
were run with a 10% tolerance for that reason.

## Findings

No KASAN, lockdep, sleep-in-atomic, DMA API or UBSAN report named the
driver at any point.

### `rp1-pio` leaks transfer records at teardown

kmemleak reported 21 unreferenced 64-byte objects, each allocated in
`rp1_pio_sm_xfer_data()` called from the driver's worker. In
`drivers/misc/rp1-pio.c`, that function allocates a `struct
dma_xfer_state` for each kernel transfer and frees it only in the
transfer's completion callback. When a client releases its DMA,
`rp1-pio` terminates the channel (`dmaengine_terminate_sync()`), and
transfers still in flight never get their callback, so their records are
never freed. Two error paths after the allocation (`dmaengine_prep_slave_sg()`
or `dmaengine_submit()` failing) also return without freeing it.

The count fits: the driver keeps four receive transfers in flight, and
the session stopped its streams five times (two gateware reloads, three
module unloads): 20, plus one transmit transfer. The cost is up to about
256 bytes each time the streams stop; streaming itself does not leak. The
driver cannot avoid it by draining first: it stops its streams after the
device is closed, when the gateware's run bit is clear and no receive
data arrives to complete the transfers. The fix belongs in `rp1-pio`:
free the records of terminated transfers, and free on its error paths.

Driver 0.4.1 gives receive transfers its own DMA buffers
([Kernels](kernels.md)). Under the same kernel it drew no KASAN, DMA API
or UBSAN report, and kmemleak reported 4 records from
`rp1_pio_sm_xfer_data()`, from transmit transfers in flight at teardown.

### The Pi 5 IOMMU driver sleeps under a spinlock

During boot, before the driver loaded and again from the desktop
compositor, the kernel reported "BUG: sleeping function called from
invalid context" in `bcm2712_iommu_map()`: it allocates memory that may
sleep while holding its `hw_lock` spinlock. It is unrelated to this
driver.
