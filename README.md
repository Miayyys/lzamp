# LZAMP

LZAMP is the maintained RK3588 asymmetric-multiprocessing project extracted
from the earlier board-bring-up experiments. Linux keeps seven application
cores and the NPU; Zephyr owns A55 CPU3 (MPIDR 0x300). MailMsg provides four
priority queues in shared memory and uses Rockchip mailbox0 channels 0-3 only
as notifications.

The current Linux 6.12 source and dependencies are pinned in
`manifests/sources.lock.yaml`. Core MailMsg lifecycle and NPU inference have
been exercised on the current board. This is a board-specific engineering
baseline, not a generic RK3588 firmware or a completed public release.

## Layout

- linux: complete self-contained kernel in an independent Git repository
- zephyr: CPU3 application, drivers, profiles and upstream Zephyr patch
- mailmsg: protocol, endpoint and notification code
- agent: tool dispatcher, unit tests and RKLLM server adaptation
- scripts/linux-support: canonical kernel adaptation sources and preparation patches
- tests: host unit tests and board-side lifecycle tests
- tools: Linux userspace clients and benchmarks
- scripts/lzamp-runtime: explicit Zephyr and RKLLM lifecycle launcher
- scripts/boot/support: optional U-Boot support
- third_party: ignored upstream source checkouts
- build: ignored generated output
- config/peripheral-ownership.yaml: controller and pin ownership map;
  a reservation alone does not demonstrate functional hardware support

## Documentation

- [System architecture](docs/architecture.md): CPU, memory, communication, lifecycle, and safety boundaries.
- [MailMsg V7 protocol](docs/mailmsg.md): rings, frames, reliability feedback, and notifications.
- [Memory and peripheral ownership](docs/memory-and-ownership.md): shared memory and hardware boundaries.
- [Build and deployment](docs/build-and-deploy.md): host builds, TFTP, and target runtime entry points.
- [Agent and peripheral tool API](docs/agent.md): natural-language tools, arguments, and safety limits.
- [Validation status](docs/validation.md): exercised paths and explicitly uncovered areas.

## Quick host check

Run make test from this directory. The tests compile into build/host-tests and
exercise the protocol, endpoint, notification abstraction, and mailbox mapping.

For Zephyr, run `make setup-zephyr` then `make zephyr`. Setup downloads pinned
upstream source and Python dependencies into `third_party/` and `build/`.
The build requires CMake, Ninja, dtc and the AArch64 GNU toolchain. It produces
`build/zephyr-controlled-stop/zephyr/lzamp-zephyr.bin`, with controlled CPU stop
enabled. Image size and memory layout must match the Linux launcher.

## Rebuild the Linux 6.12 candidate

The publication layout is a main repository plus an independent
complete Linux repository linked at `linux/`. The remotes are
`git@github.com:Miayyys/lzamp.git` and `git@github.com:Miayyys/lzamp-linux.git`.
Both repositories use a single source-snapshot commit; upstream revisions
are recorded separately. Clone the complete project with:

```sh
git clone --recurse-submodules git@github.com:Miayyys/lzamp.git
cd lzamp
make linux-6.12-configure
make linux
```

Alternatively reconstruct the kernel from upstream as follows, only when
`linux/` does not already exist. Run from the LZAMP directory:

```sh
git clone --no-checkout --filter=blob:none https://github.com/rockchip-linux/kernel.git linux
git -C linux checkout --detach 470f9dccbdc42e7b8a824d0a5c5640a10e9457d2
make linux-6.12-prepare
make linux-6.12-configure
make linux
```

The prepare step refuses a dirty tree or a commit other than the one recorded
in `manifests/sources.lock.yaml`. It installs the DTS, drivers and a MailMsg
source copy inside the kernel; there are no cross-checkout wrappers. Do not
repeat preparation on an integrated tree. `make linux-6.12-verify-source`
checks those copies against the canonical main-project files. Other upstream
board definitions are retained. Outputs are in `build/linux-rockchip-6.12/`.

## Source policy

Linux is a complete separate source repository; Zephyr upstream is a pinned
dependency, with LZAMP application and drivers kept under `zephyr/`. Models,
runtime binaries and generated images are not tracked in the main repository.
Historical 5.10 artifacts are not the current build recipe.

## Promotion gates

Clean Linux builds, a Zephyr rebuild matching the production image, host C
tests and 26 Agent unit tests have passed. Basic GPIO, UART, PWM, ADC and I2C
operations have been exercised on the current board. SPI physical testing,
exhaustive timing tests and full electrical validation remain pending.
Another board requires its own pinmux, clocks, power and memory-ownership
review; do not reuse this DTS or wiring blindly.

## Prepare the model service

```sh
git clone --no-checkout --filter=blob:none https://github.com/airockchip/rknn-llm.git third_party/rknn-llm
git -C third_party/rknn-llm checkout --detach 878f9361fd3afa7e167b7079918918f78d2c1c2a
make prepare-rkllm-server
```

This creates `build/rkllm-server-prepared/` from committed upstream files,
applies the seven-core CPU-mask patch and retains the upstream license.
It refuses an existing output directory. Install the dependencies listed in
`agent/requirements-server.txt` in the target Python environment. Obtain the
model separately and verify the hash in `manifests/sources.lock.yaml`.
The Flask service is a local demo, not a production API.

## Deployment boundaries

`make deploy` replaces Image and DTB in an existing TFTP directory (`TFTP_ROOT`,
default `/srv/tftp`). It requires write permission and affects subsequent
network boots; it does not reboot or flash. Do not deploy while the board is
downloading these files. Matching modules and wireless firmware must be
installed separately.

`make deploy-runtime BOARD_HOST=root@BOARD_IPV4` uploads only the Agent and
launcher. It does not provision or start the model service. Before `start`,
provide these paths under `/userdata/lzamp/`:

- `zephyr/lzamp-zephyr.bin`: matching controlled-stop image.
- `agent/rkllm-server/`: prepared server directory including `lib/`.
- `agent/site-packages/`: optional target Python dependencies; system Python is also supported.
- `models/Qwen3.5-0.8B_w8a8_rk3588.rkllm`: separately obtained model.

## Board runtime launcher

Install `scripts/lzamp-runtime` on the board and run it explicitly as root. It
does not register a systemd service or enable boot-time startup.

```sh
lzamp-runtime start
lzamp-runtime status
lzamp-runtime smoke
lzamp-runtime stop
lzamp-runtime stop --with-zephyr
```

Plain `stop` leaves Zephyr running. The `--with-zephyr` form requests the
MailMsg controlled-stop path after stopping RKLLM. Paths can be overridden by
the environment variables declared at the top of the script.

LZAMP-authored files default to the root MIT license; existing per-file
identifiers and third-party terms are retained. See `NOTICE` for component
boundaries. This does not put the complete Linux kernel, RKLLM runtime or
models under MIT. The static notice review does not constitute an exhaustive
legal audit of upstream projects.

Run `make publication-check` for a read-only list of publication metadata
still missing. It checks the pinned kernel gitlink and license metadata.
`manifests/publication.json` records the publication choices without assigning
a blanket license. This check is not a legal review or a secrets scanner.
