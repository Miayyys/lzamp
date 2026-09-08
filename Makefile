# SPDX-License-Identifier: MIT
CC ?= cc
CROSS_CC ?= aarch64-linux-gnu-gcc
CFLAGS ?= -O2 -g
CPPFLAGS += -Imailmsg
CFLAGS += -std=c11 -Wall -Wextra -Werror

TEST_NAMES := mailmsg_protocol_test mailmsg_endpoint_test mailmsg_notify_test mailmsg_mailbox0_test gpio3_polling_test rk3588_peripheral_clock_test rk3588_peripheral_drivers_test
TEST_BINS := $(addprefix build/host-tests/,$(TEST_NAMES))
BOARD_TOOL_NAMES := mailmsg-user-client mailmsg-window-bench mailmsg-exclusive-reader-test mailmsg-offline-wait-test
BOARD_TOOL_BINS := $(addprefix build/board-tools/,$(BOARD_TOOL_NAMES))
LINUX_6_12_SRC ?= $(CURDIR)/linux
LINUX_6_12_OUT ?= $(CURDIR)/build/linux-rockchip-6.12
LINUX_JOBS ?= $(shell nproc)
LINUX_6_12_IMAGE ?= $(LINUX_6_12_OUT)/arch/arm64/boot/Image
LINUX_6_12_DTB ?= $(LINUX_6_12_OUT)/arch/arm64/boot/dts/rockchip/rk3588s-lzamp-linux.dtb
TFTP_BIND ?= 10.42.0.1:69
TFTP_ROOT ?= /srv/tftp
BOARD_HOST ?=
ZEPHYR_SRC ?= $(CURDIR)/third_party/zephyrproject/zephyr
ZEPHYR_WORKSPACE ?= $(dir $(ZEPHYR_SRC))
ZEPHYR_WEST ?= $(CURDIR)/build/zephyr-venv/bin/west
ZEPHYR_BUILD_DIR ?= $(CURDIR)/build/zephyr-smoke
ZEPHYR_DEPLOY_IMAGE ?= $(ZEPHYR_BUILD_DIR)/zephyr/lzamp-zephyr.bin
ZEPHYR_STOP_BUILD_DIR ?= $(CURDIR)/build/zephyr-controlled-stop
ZEPHYR_STOP_DEPLOY_IMAGE ?= $(ZEPHYR_STOP_BUILD_DIR)/zephyr/lzamp-zephyr.bin
ZEPHYR_SPI_BUILD_DIR ?= $(CURDIR)/build/zephyr-controlled-stop-spi0
ZEPHYR_SPI_DEPLOY_IMAGE ?= $(ZEPHYR_SPI_BUILD_DIR)/zephyr/lzamp-zephyr.bin
ZEPHYR_IMAGE_SIZE ?= 131072
UBOOT_SRC ?= $(CURDIR)/third_party/u-boot
UBOOT_OUT ?= $(CURDIR)/build/u-boot-rk3588s-lzamp
RKBIN_SRC ?= $(CURDIR)/third_party/rkbin
UBOOT_BL31 ?= $(RKBIN_SRC)/bin/rk35/rk3588_bl31_v1.54.elf
UBOOT_TPL ?= $(RKBIN_SRC)/bin/rk35/rk3588_ddr_lp4_2112MHz_lp5_2400MHz_v1.21.bin
UBOOT_JOBS ?= $(shell nproc)
UBOOT_PYTHON ?= python3
UBOOT_PYTHONPATH ?= $(firstword $(wildcard $(CURDIR)/build/zephyr-venv/lib/python*/site-packages))
UBOOT_SOURCE_DATE_EPOCH ?= 1783381843
UBOOT_MASKROM_LOADER ?= $(UBOOT_OUT)/lzamp-rk3588s-maskrom-loader.bin

.PHONY: all test agent-test runtime-check board-tools zephyr-prepare zephyr-build zephyr-build-controlled-stop zephyr-build-controlled-stop-spi0 linux-6.12-prepare linux-6.12-configure linux-6.12-verify-source linux-6.12-build linux-6.12-tftp-serve u-boot-prepare u-boot-host-check u-boot-configure u-boot-build u-boot-maskrom-loader clean

all: test

.PHONY: linux zephyr setup-zephyr deploy deploy-runtime prepare-rkllm-server
.PHONY: publication-check
publication-check:
	python3 scripts/check-publication.py

RKNN_LLM_SRC ?= $(CURDIR)/third_party/rknn-llm
RKLLM_SERVER_OUT ?= $(CURDIR)/build/rkllm-server-prepared
prepare-rkllm-server:
	bash scripts/prepare-rkllm-server.sh "$(RKNN_LLM_SRC)" "$(RKLLM_SERVER_OUT)"

deploy:
	bash scripts/deploy-tftp.sh "$(LINUX_6_12_IMAGE)" "$(LINUX_6_12_DTB)" "$(TFTP_ROOT)"
deploy-runtime:
	@test -n "$(BOARD_HOST)" || { echo 'Set BOARD_HOST=root@board-ip' >&2; exit 2; }
	bash scripts/deploy-runtime.sh "$(BOARD_HOST)"
setup-zephyr:
	bash scripts/setup-zephyr.sh
linux: linux-6.12-build
zephyr: zephyr-build-controlled-stop

test: $(TEST_BINS) agent-test runtime-check
	@set -e; for test_bin in $(TEST_BINS); do ./$$test_bin; done
	bash tests/host/peripheral_ownership_test.sh

agent-test:
	python3 -m unittest discover -s agent/tests -p 'test_*.py'

runtime-check:
	bash -n scripts/setup-zephyr.sh scripts/deploy-runtime.sh scripts/deploy-tftp.sh scripts/prepare-rkllm-server.sh
	bash -n scripts/lzamp-runtime
	bash -n scripts/serve-linux-6.12-tftp.sh
	bash -n scripts/pad-zephyr-image.sh
	bash -n tests/board/gpio3-loopback-test.sh
	python3 -m py_compile tests/board/adc-stream-test.py

board-tools: $(BOARD_TOOL_BINS)

zephyr-prepare:
	bash scripts/prepare-zephyr.sh "$(ZEPHYR_SRC)"

zephyr-build:
	cd "$(ZEPHYR_WORKSPACE)" && \
	CCACHE_DIR=/tmp/lzamp-zephyr-ccache \
	ZEPHYR_TOOLCHAIN_VARIANT=cross-compile \
	CROSS_COMPILE=/usr/bin/aarch64-linux-gnu- \
	"$(ZEPHYR_WEST)" build -p always -b roc_rk3588_pc/rk3588 \
		"$(CURDIR)/zephyr/app" -d "$(ZEPHYR_BUILD_DIR)"
	bash scripts/pad-zephyr-image.sh \
		"$(ZEPHYR_BUILD_DIR)/zephyr/zephyr.bin" \
		"$(ZEPHYR_DEPLOY_IMAGE)" "$(ZEPHYR_IMAGE_SIZE)"

zephyr-build-controlled-stop:
	cd "$(ZEPHYR_WORKSPACE)" && \
	CCACHE_DIR=/tmp/lzamp-zephyr-ccache \
	ZEPHYR_TOOLCHAIN_VARIANT=cross-compile \
	CROSS_COMPILE=/usr/bin/aarch64-linux-gnu- \
	"$(ZEPHYR_WEST)" build -p always -b roc_rk3588_pc/rk3588 \
		"$(CURDIR)/zephyr/app" -d "$(ZEPHYR_STOP_BUILD_DIR)" -- \
		-DMAILMSG_ENABLE_STOP_CONTROL=ON \
		-DEXTRA_CONF_FILE="$(CURDIR)/zephyr/profiles/controlled-stop.conf"
	bash scripts/pad-zephyr-image.sh \
		"$(ZEPHYR_STOP_BUILD_DIR)/zephyr/zephyr.bin" \
		"$(ZEPHYR_STOP_DEPLOY_IMAGE)" "$(ZEPHYR_IMAGE_SIZE)"

zephyr-build-controlled-stop-spi0:
	cd "$(ZEPHYR_WORKSPACE)" && \
	CCACHE_DIR=/tmp/lzamp-zephyr-ccache \
	ZEPHYR_TOOLCHAIN_VARIANT=cross-compile \
	CROSS_COMPILE=/usr/bin/aarch64-linux-gnu- \
	"$(ZEPHYR_WEST)" build -p always -b roc_rk3588_pc/rk3588 \
		"$(CURDIR)/zephyr/app" -d "$(ZEPHYR_SPI_BUILD_DIR)" -- \
		-DMAILMSG_ENABLE_STOP_CONTROL=ON -DLZAMP_PIN_PROFILE=spi0 \
		-DEXTRA_CONF_FILE="$(CURDIR)/zephyr/profiles/controlled-stop.conf"
	bash scripts/pad-zephyr-image.sh \
		"$(ZEPHYR_SPI_BUILD_DIR)/zephyr/zephyr.bin" \
		"$(ZEPHYR_SPI_DEPLOY_IMAGE)" "$(ZEPHYR_IMAGE_SIZE)"

linux-6.12-prepare:
	bash scripts/prepare-linux-6.12.sh "$(LINUX_6_12_SRC)"

linux-6.12-configure:
	$(MAKE) -C "$(LINUX_6_12_SRC)" O="$(LINUX_6_12_OUT)" \
		ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- rockchip_linux_defconfig
	"$(LINUX_6_12_SRC)/scripts/kconfig/merge_config.sh" -m -O "$(LINUX_6_12_OUT)" \
		"$(LINUX_6_12_OUT)/.config" scripts/linux-support/config/rockchip-6.12-lzamp.fragment
	$(MAKE) -C "$(LINUX_6_12_SRC)" O="$(LINUX_6_12_OUT)" \
		ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- olddefconfig

linux-6.12-verify-source:
	bash scripts/verify-kernel-mailmsg.sh "$(LINUX_6_12_SRC)"

linux-6.12-build: linux-6.12-verify-source
	$(MAKE) -C "$(LINUX_6_12_SRC)" O="$(LINUX_6_12_OUT)" \
		ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- -j"$(LINUX_JOBS)" \
		Image rockchip/rk3588s-lzamp-linux.dtb modules

linux-6.12-tftp-serve:
	bash scripts/serve-linux-6.12-tftp.sh \
		"$(LINUX_6_12_IMAGE)" "$(LINUX_6_12_DTB)" "$(TFTP_BIND)"

u-boot-prepare:
	bash scripts/prepare-u-boot.sh "$(UBOOT_SRC)"

u-boot-host-check:
	@PYTHONPATH="$(UBOOT_PYTHONPATH):$${PYTHONPATH:-}" "$(UBOOT_PYTHON)" -c \
		'import elftools, setuptools' || { \
		printf '%s\n' 'error: U-Boot host Python needs pyelftools and setuptools; see scripts/boot/support/requirements-host.txt' >&2; \
		exit 2; \
	}

u-boot-configure: u-boot-host-check
	$(MAKE) -C "$(UBOOT_SRC)" O="$(UBOOT_OUT)" \
		CROSS_COMPILE=aarch64-linux-gnu- lzamp-rk3588s_defconfig

u-boot-build: u-boot-host-check
	test -f "$(UBOOT_BL31)"
	test -f "$(UBOOT_TPL)"
	PYTHONPATH="$(UBOOT_PYTHONPATH):$${PYTHONPATH:-}" \
	SOURCE_DATE_EPOCH="$(UBOOT_SOURCE_DATE_EPOCH)" \
	$(MAKE) -C "$(UBOOT_SRC)" O="$(UBOOT_OUT)" \
		CROSS_COMPILE=aarch64-linux-gnu- \
		BL31="$(UBOOT_BL31)" ROCKCHIP_TPL="$(UBOOT_TPL)" \
		-j"$(UBOOT_JOBS)" all

u-boot-maskrom-loader: u-boot-build
	bash scripts/pack-u-boot-maskrom.sh \
		"$(UBOOT_OUT)" "$(RKBIN_SRC)" "$(UBOOT_MASKROM_LOADER)"

build/host-tests:
	mkdir -p $@

build/board-tools:
	mkdir -p $@

build/host-tests/mailmsg_protocol_test: tests/host/mailmsg_protocol_test.c mailmsg/mailmsg.c | build/host-tests
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@

build/host-tests/mailmsg_endpoint_test: tests/host/mailmsg_endpoint_test.c mailmsg/mailmsg.c mailmsg/mailmsg_endpoint.c | build/host-tests
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@

build/host-tests/mailmsg_notify_test: tests/host/mailmsg_notify_test.c | build/host-tests
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@

build/host-tests/mailmsg_mailbox0_test: tests/host/mailmsg_mailbox0_test.c mailmsg/mailmsg_mailbox0.c | build/host-tests
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@

build/host-tests/gpio3_polling_test: tests/host/gpio3_polling_test.c zephyr/drivers/gpio3_polling.c | build/host-tests
	$(CC) $(CPPFLAGS) -Izephyr/drivers $(CFLAGS) $^ -o $@

build/host-tests/rk3588_peripheral_clock_test: tests/host/rk3588_peripheral_clock_test.c zephyr/drivers/rk3588_peripheral_clock.c | build/host-tests
	$(CC) $(CPPFLAGS) -Izephyr/drivers $(CFLAGS) $^ -o $@

build/host-tests/rk3588_peripheral_drivers_test: tests/host/rk3588_peripheral_drivers_test.c zephyr/drivers/gpio3_polling.c zephyr/drivers/rk3588_pinctrl.c zephyr/drivers/rk3588_reset.c zephyr/drivers/rk3588_uart_polling.c zephyr/drivers/rk3588_i2c_polling.c zephyr/drivers/rk3588_spi_polling.c zephyr/drivers/rk3588_pwm.c zephyr/drivers/rk3588_saradc.c zephyr/drivers/rk3588_peripheral_service.c | build/host-tests
	$(CC) $(CPPFLAGS) -Izephyr/drivers $(CFLAGS) $^ -o $@

build/board-tools/mailmsg-user-client: tools/mailmsg_user_client.c | build/board-tools
	$(CROSS_CC) $(CPPFLAGS) -O2 -g -std=gnu11 -Wall -Wextra -Werror -static $< -o $@

build/board-tools/mailmsg-window-bench: tools/mailmsg_window_bench.c | build/board-tools
	$(CROSS_CC) $(CPPFLAGS) -O2 -g -std=gnu11 -Wall -Wextra -Werror -static $< -o $@

build/board-tools/mailmsg-exclusive-reader-test: tests/board/mailmsg_exclusive_reader_test.c | build/board-tools
	$(CROSS_CC) $(CPPFLAGS) -O2 -g -std=gnu11 -Wall -Wextra -Werror -static $< -o $@

build/board-tools/mailmsg-offline-wait-test: tests/board/mailmsg_offline_wait_test.c | build/board-tools
	$(CROSS_CC) $(CPPFLAGS) -O2 -g -std=gnu11 -Wall -Wextra -Werror -static $< -o $@

clean:
	rm -rf build/host-tests build/board-tools
