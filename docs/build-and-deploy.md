# Build and Deployment

Run the host commands below from the repository root. Arch Linux requires Make, Git, Python, CMake,
Ninja, dtc, and an `aarch64-linux-gnu-*` cross toolchain.

## Clone and Test

```sh
git clone --recurse-submodules git@github.com:Miayyys/lzamp.git
cd lzamp
make test
make publication-check
```

The main repository and `linux/` submodule are single-commit source snapshots. Their upstream
revisions are recorded under `manifests/`.

## Linux 6.12

```sh
make linux-6.12-configure
make linux
```

Outputs are written to `build/linux-rockchip-6.12/`. The primary artifacts are
`arch/arm64/boot/Image` and `arch/arm64/boot/dts/rockchip/rk3588s-lzamp-linux.dtb`. Kernel modules
must come from the same build; replacing Image alone does not deploy matching modules or wireless
firmware.

`linux/` is already an integrated source snapshot. Do not run `linux-6.12-prepare` on it. That target
only reconstructs the integrated tree from the clean Rockchip revision pinned in the manifests.

## Zephyr

```sh
make setup-zephyr
make zephyr
```

The default output is `build/zephyr-controlled-stop/zephyr/lzamp-zephyr.bin`. It includes controlled
CPU stop support and is padded to the size expected by the Linux upload interface. Linux, Zephyr, and
MailMsg versions must match.

## RKLLM Example Server

```sh
git clone --no-checkout --filter=blob:none https://github.com/airockchip/rknn-llm.git third_party/rknn-llm
git -C third_party/rknn-llm checkout --detach 878f9361fd3afa7e167b7079918918f78d2c1c2a
make prepare-rkllm-server
```

The result under `build/rkllm-server-prepared/` includes upstream server files, the AArch64 runtime,
upstream license material, and the seven-core CPU-mask adaptation. Models are not distributed in this
repository; obtain and verify them using `manifests/sources.lock.yaml`. The Flask server is a local
demonstration, not a production service.

## TFTP Development Deployment

```sh
make deploy TFTP_ROOT=/srv/tftp
```

This replaces Image and DTB in the TFTP root and therefore affects the next network boot. It does not
write eMMC, modify the U-Boot environment, or reboot the board. Ensure no board is downloading these
files while they are replaced.

## Userspace Runtime Deployment

```sh
make deploy-runtime BOARD_HOST=root@BOARD_IPV4
```

This uploads only the Agent and `lzamp-runtime`. It does not upload a model, Zephyr image, RKLLM
runtime, or Python packages. After provisioning the prerequisites listed in the root README, use:

```sh
/userdata/lzamp/bin/lzamp-runtime start
/userdata/lzamp/bin/lzamp-runtime status
/userdata/lzamp/bin/lzamp-runtime smoke
```

Plain `stop` stops RKLLM but leaves Zephyr running. `stop --with-zephyr` also requests controlled CPU3
shutdown. The runtime scripts do not overwrite Zephyr images, flash U-Boot, or alter partitions.
