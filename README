Xiaomi Pad 5 (nabu) Linux kernel
================================

This is a Linux kernel tree for Xiaomi Pad 5 (`qcom/sm8150-xiaomi-nabu`).

Base
----

- Upstream: https://gitlab.com/sm8150-mainline/linux.git
- Branch: `sm8150/6.14.11`
- Commit: `5181e1358ddd6ea8028e841d928942373e6aebc8`

Board support from that baseline is kept. Additional commits on this
branch add the Android 17 bring-up pieces that mainline does not ship
(ashmem, QSEECOM/SCM compatibility, Android fstab/ramoops, netfilter
stubs, and related nabu hardware fixes).

Build
-----

```sh
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- \
  defconfig xiaomi_nabu_defconfig
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- -j$(nproc) \
  Image.gz dtbs
```

Device tree: `arch/arm64/boot/dts/qcom/sm8150-xiaomi-nabu.dts`
Defconfig: `arch/arm64/configs/xiaomi_nabu_defconfig`

Out-of-tree GPU
---------------

Qualcomm KGSL is imported under `drivers/gpu/msm-kgsl/` from
`https://github.com/qualcomm-linux/kgsl` commit
`72383caaef29830f77c5314945cddcd293d817f2`, with nabu IOMMU probe
fixes. Build it as a module against this tree:

```sh
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- \
  M=drivers/gpu/msm-kgsl modules
```

License
-------

The Linux kernel is licensed under GPL-2.0-only. See `COPYING` and
`LICENSES/preferred/GPL-2.0`. Do not relicense this tree as GPL-3.0.
