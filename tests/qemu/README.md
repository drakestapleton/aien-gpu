# QEMU module tests

Loads `aien_gpu.ko` inside a throwaway QEMU guest that boots the Spark's own
kernel image (7.0.0-1019-nvidia), so the module's vermagic and modversions
match. Nothing is loaded on the host.

## Run

```sh
KO=path/to/aien_gpu.ko   # built against 7.0.0-1019-nvidia
sh tests/qemu/run.sh "$KO" absent
sh tests/qemu/run.sh "$KO" wrongdev
sh tests/qemu/run.sh "$KO" nobind
sh tests/qemu/run.sh "$KO" selftest-trap
```

Each run prints one line and exits 0 only on PASS:

```
AIEN-QEMU <case> PASS|FAIL|TIMEOUT <detail> [accel=kvm|tcg boot_s=... wall_ms=... qemu_rc=... log=/tmp/aien-qemu.XXXXXX/console.log]
```

`boot_s` is guest uptime when `/init` starts. `wall_ms` is the host wall
time of the qemu run. The log holds the full guest console, a dump of the
guest `dmesg` between `AIEN-QEMU-DMESG-BEGIN/END`, the guest taint value and
the guest's own verdict line.

Guest: `-M virt,gic-version=3 -enable-kvm -cpu host -smp 2 -m 1024
-nodefaults -no-reboot`, kernel cmdline `console=ttyAMA0 panic=-1
oops=panic`. An oops panics the guest and QEMU exits without a verdict
line, which counts as FAIL. If KVM cannot start (qemu exits before
`Booting Linux`), the run retries under TCG `-cpu max` and reports
`accel=tcg`. `AIEN_QEMU_ACCEL=tcg` forces TCG. The guard is
`AIEN_QEMU_TIMEOUT` (default 180 s) and reports TIMEOUT. Extra arguments
after CASE go to qemu. A case can declare its own qemu arguments in a
`# qemu-args:` header line.

The kernel image: `/boot/vmlinuz-*` is root-only on the Spark and the
harness never uses sudo. `fetch-kernel.sh` downloads the installed
`linux-image-<krel>` version with `apt-get download` and accepts
`boot/vmlinuz-<krel>` only when its md5 equals the digest dpkg recorded
for the installed file. The copy is cached in `/tmp/aien-qemu-kernel/`.
`AIEN_QEMU_KERNEL` overrides the image.

## Cases

Every case fails on any `Oops`, `BUG:`, `WARNING:`, `Call trace`,
`Unable to handle`, `Kernel panic` or `general protection` line in dmesg
after the case starts. It also fails on any new D/W/B/L taint bit.
`rmmod` must succeed and leave nothing in lsmod or under
`/sys/bus/pci/drivers/aien-gpu`.

| case | guest | proves |
|---|---|---|
| `absent` | no GB10 | Ten load/unload cycles. Each load prints `aien-gpu: omega self-test PASS`, registers `/sys/bus/pci/drivers/aien-gpu`, binds nothing and creates no `/dev/aien-gpu`. |
| `wrongdev` | `-device pci-testdev` (1b36:0005) | With `allow_bind=1`, after `1b36 0005` is written to `new_id`, the probe refuses: the device is not bound, dmesg has a line with `aien-gpu` and `refus`, and there is no `/dev/aien-gpu`. |
| `nobind` | `-device pci-testdev` | Same as wrongdev with the default `allow_bind=0`. The function's `enable` count must also be 0 after the refusal. |
| `selftest-trap` | no GB10 | With `selftest_trap=1`, `aien-gpu: omega trap contained code=` appears, then `aien-gpu: omega self-test PASS`, with no splat. |

## What this does not prove

- There is no GB10 or any NVIDIA device in the guest. Nothing here touches
  real GPU registers, BARs, firmware or DMA.
- The guest is a KVM (or TCG) virtual machine running the host kernel
  image. It is not the Spark's bare-metal boot, and its device tree, IOMMU
  and PCI topology are QEMU's.
- `nobind` checks `enable == 0` after the refusal. That is necessary for
  "did not touch the device" but not sufficient: config-space reads are
  not observed.
- A PASS here is EXERCISED in QEMU. It is not hardware qualification.
