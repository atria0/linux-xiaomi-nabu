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

Hardware status
---------------

Checked 2026-08-24 on this tree with Android userspace
(`6.14.11-nabu-android17-diag+`).

| Tag | Meaning |
| --- | --- |
| ok | Verified working on device |
| partial | Probes or some function works; not complete |
| broken | Confirmed not working |
| untested | Present in DT/config; no functional test |

| Block | Tag | Notes |
| --- | --- | --- |
| SoC / boot | ok | SM8150 boots to Android; `sys.boot_completed=1` |
| UFS storage | ok | System and userdata mount read-write |
| Display | partial | NT36523 dual-DSI + KTZ8866 backlight. drm/msm DPU lights the panel. Mesa compositor still fails GPU textures (UBWC/EGL); the UI can go black |
| GPU (Adreno 640) | partial | KGSL probes, `/dev/kgsl-3d0` exists, GMU binds. SMMU/IOMMU is not closed (DMA range / `adreno_smmu`) |
| Touchscreen | ok | NT36523 SPI; `NVTCapacitiveTouchScreen` is present |
| Power key | ok | PM8941 pwrkey |
| Volume up | ok | PM8150 GPIO |
| Volume down | broken | XBL arms PMIC resin as S2 hard-reset. Linux maps `KEY_VOLUMEDOWN`, but the key never reaches the kernel. Do not poke S2 registers from probe |
| Lid / hall | ok | gpio-keys `SW_LID` |
| USB-C data / ADB | partial | ADB works while the display is on; s2idle drops the gadget |
| USB-C PD / charge path | untested | DT has `usb-c-connector` and `xiaomi,usbpd-pm` |
| Charger (LN8000) | untested | Enabled in DTS (upstream limited the charge voltage); charging behaviour not tested |
| Wi-Fi (WCN3990) | broken | Userspace wifi/cnd HAL crashes; scan/associate not verified |
| Bluetooth (WCN3991) | partial | BT UART on the same QUP is up; pairing / BT audio not verified |
| Audio (CS35L41) | untested | Four amps + `qcom,sm8150-sndcard` in DT; playback/capture not tested |
| Camera | broken | Camera bring-up is not done on this tree; userspace camera HAL crashes |
| Video (Venus) | untested | |
| TEE / KeyMint / fingerprint | broken | QSEECOM `app_send` fails; KeyMint `-10003` (`HARDWARE_TYPE_UNAVAILABLE`) |
| ADSP / CDSP / MPSS / SLPI | untested | remoteproc not started |
| Keyboard folio | untested | DT has `xiaomi,keyboard` |

License
-------

The Linux kernel is licensed under GPL-2.0-only. See `COPYING` and
`LICENSES/preferred/GPL-2.0`. Do not relicense this tree as GPL-3.0.
