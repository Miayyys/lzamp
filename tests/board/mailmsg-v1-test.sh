#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# One entry point for MailMsg V1 revision 7 artifact upload and board tests.
# Host mode validates and uploads the RAM-only artifacts over SSH.  Remote
# mode runs exactly one profile because timeout profiles require a fresh
# U-Boot RAM boot before another profile can be attempted.

set -Eeuo pipefail
IFS=$'\n\t'

readonly SCRIPT_NAME=${0##*/}
readonly DEFAULT_R1_HOST=10.42.0.193
readonly DEFAULT_R1_USER=root
readonly DEFAULT_REMOTE_ROOT=/userdata/mailmsg-v1-r7
readonly DEFAULT_LLM_ROOT=/userdata/rkllm-api-demo

MODE=host
PROFILE=
UPLOAD_ONLY=0
CHECK_ONLY=0
R1_HOST=${R1_HOST:-$DEFAULT_R1_HOST}
R1_USER=${R1_USER:-$DEFAULT_R1_USER}
R1_IDENTITY=${R1_IDENTITY:-${HOME}/.ssh/id_ed25519}
REMOTE_ROOT=${MAILMSG_REMOTE_ROOT:-$DEFAULT_REMOTE_ROOT}
LLM_ROOT=${RKLLM_DEMO_ROOT:-$DEFAULT_LLM_ROOT}

usage()
{
	cat <<EOF
Usage:
  $SCRIPT_NAME [--host ADDRESS] [--user USER] [--identity KEY] [--upload-only]
  $SCRIPT_NAME [host options] --profile PROFILE
  $SCRIPT_NAME --check-only
  $SCRIPT_NAME --remote --remote-root DIR --profile PROFILE

Profiles (one fresh RAM boot per terminal profile):
  controlled     Full normal/data/error/queue/STOP_READY/rearm regression.
  stop-refused   Normal image must refuse STOP and remain usable.
  stop-timeout   STOP request is deliberately not consumed; ends terminal.
  start-timeout  SESSION_READY is deliberately withheld; ends terminal.
  llm-coexistence  Normal image plus RKLLM generation and concurrent p2 traffic.
  llm-soak       24 RKLLM generations plus sustained round-robin p0-p3 traffic.
  llm-p2-window  RKLLM generation plus stepped p2 in-flight windows 1..32.
  llm-four-priority  RKLLM generation plus simultaneous p0-p3 window=1 traffic.
  llm-four-priority-sweep  RKLLM generation plus simultaneous p0-p3 windows 2/4/7.

Host environment overrides: R1_HOST, R1_USER, R1_IDENTITY,
MAILMSG_REMOTE_ROOT, RKLLM_DEMO_ROOT.  Uploads only to /userdata; never writes
a boot partition.
Before --profile, manually RAM-boot the uploaded FIT from U-Boot; this script
does not automate the U-Boot console or change the persistent boot image.
EOF
}

fail()
{
	printf 'FAIL: %s\n' "$*" >&2
	return 1
}

note()
{
	printf '\n[%s] %s\n' "$(date '+%H:%M:%S')" "$*"
}

pass()
{
	printf 'PASS: %s\n' "$*"
}

while (($#)); do
	case $1 in
	--remote)
		MODE=remote
		shift
		;;
	--profile)
		(($# >= 2)) || { usage >&2; exit 2; }
		PROFILE=$2
		shift 2
		;;
	--host)
		(($# >= 2)) || { usage >&2; exit 2; }
		R1_HOST=$2
		shift 2
		;;
	--user)
		(($# >= 2)) || { usage >&2; exit 2; }
		R1_USER=$2
		shift 2
		;;
	--identity)
		(($# >= 2)) || { usage >&2; exit 2; }
		R1_IDENTITY=$2
		shift 2
		;;
	--remote-root)
		(($# >= 2)) || { usage >&2; exit 2; }
		REMOTE_ROOT=$2
		shift 2
		;;
	--llm-root)
		(($# >= 2)) || { usage >&2; exit 2; }
		LLM_ROOT=$2
		shift 2
		;;
	--upload-only)
		UPLOAD_ONLY=1
		shift
		;;
	--check-only)
		CHECK_ONLY=1
		shift
		;;
	-h|--help)
		usage
		exit 0
		;;
	*)
		printf 'Unknown argument: %s\n' "$1" >&2
		usage >&2
		exit 2
		;;
	esac
done

case ${PROFILE:-none} in
	none|controlled|stop-refused|stop-timeout|start-timeout|llm-coexistence|llm-soak|llm-p2-window|llm-four-priority|llm-four-priority-sweep) ;;
	*) fail "unknown profile: $PROFILE"; exit 2 ;;
esac

if [[ ! $REMOTE_ROOT =~ ^/userdata(/[A-Za-z0-9._-]+)+$ ||
	$REMOTE_ROOT == */./* || $REMOTE_ROOT == */../* ||
	$REMOTE_ROOT == */. || $REMOTE_ROOT == */.. ]]; then
	fail "remote root must be an absolute, simple /userdata path: $REMOTE_ROOT"
	exit 2
fi

if [[ ! $LLM_ROOT =~ ^/userdata(/[A-Za-z0-9._-]+)+$ ||
	$LLM_ROOT == */./* || $LLM_ROOT == */../* ||
	$LLM_ROOT == */. || $LLM_ROOT == */.. ]]; then
	fail "LLM root must be an absolute, simple /userdata path: $LLM_ROOT"
	exit 2
fi

if ((CHECK_ONLY)) && { ((UPLOAD_ONLY)) || [[ -n $PROFILE ]] || [[ $MODE == remote ]]; }; then
	fail "--check-only cannot be combined with upload, profile, or remote mode"
	exit 2
fi
if ((UPLOAD_ONLY)) && [[ -n $PROFILE ]]; then
	fail "--upload-only and --profile are mutually exclusive"
	exit 2
fi

expected_sha256()
{
	case $1 in
	mailmsg-v1-final.img)
		printf '%s\n' 95f453099d363fdf1379130f7585fb2d57043665c48e94f88ac70b433508d61f ;;
	mailmsg-v1-normal.bin)
		printf '%s\n' 29dcd57a4f11fc988a1831352b278be91a60548ea5e9c63b68cbb7b1ced4b038 ;;
	mailmsg-v1-controlled-stop.bin)
		printf '%s\n' 42dd4335af68057c1329554e591fc786b958bf877db9befeb9ffbe4c87c0fa31 ;;
	mailmsg-v1-start-timeout.bin)
		printf '%s\n' 95ecb5e5631700ea02806e29cea9e7deb8fcd7b1dd84fb827bf3480ba7320e38 ;;
	mailmsg-v1-stop-timeout.bin)
		printf '%s\n' fb8e4dbb7b772315223d9dcdd8d4ee96617d12aacd68ae56ecd6ce4b09656f01 ;;
	mailmsg-user-client-aarch64)
		printf '%s\n' c85c829789c15445f665e9ccdbf02ccc9ce30cf0fdc0b499514e50d2c1d0ae55 ;;
	mailmsg-exclusive-reader-test-aarch64)
		printf '%s\n' 36e9582afc62aa0d8b73dfd6118c2fba37812ac40b4ad03150f5eed43a30dbfa ;;
	mailmsg-offline-wait-test-aarch64)
		printf '%s\n' 695462dc3986af841afb35c32da21624c1ff19e7c48f7fc48e86a1c4597dfaca ;;
	*) return 1 ;;
	esac
}

readonly ARTIFACT_NAMES=(
	mailmsg-v1-final.img
	mailmsg-v1-normal.bin
	mailmsg-v1-controlled-stop.bin
	mailmsg-v1-start-timeout.bin
	mailmsg-v1-stop-timeout.bin
	mailmsg-user-client-aarch64
	mailmsg-exclusive-reader-test-aarch64
	mailmsg-offline-wait-test-aarch64
)

verify_file_hash()
{
	local path=$1
	local name=${path##*/}
	local expected actual

	expected=$(expected_sha256 "$name") || return 1
	actual=$(sha256sum "$path")
	actual=${actual%% *}
	[[ $actual == "$expected" ]] || {
		printf 'hash mismatch: %s\n  expected %s\n  actual   %s\n' \
			"$path" "$expected" "$actual" >&2
		return 1
	}
}

host_main()
{
	local script_dir repo_root artifact_dir tool_dir bench_source bench_binary
	local target remote_cmd
	local name local_path remote_hash expected local_hash
	local -a files ssh_opts

	script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
	repo_root=$(cd -- "$script_dir/../.." && pwd)
	artifact_dir=$repo_root/artifacts/mailmsg-v1-final-r7
	tool_dir=$repo_root/build/tools
	bench_source=$repo_root/tools/mailmsg_window_bench.c
	bench_binary=$tool_dir/mailmsg-window-bench-aarch64

	command -v aarch64-linux-gnu-gcc >/dev/null || {
		fail "aarch64-linux-gnu-gcc is required"
		return 1
	}
	[[ -f $bench_source ]] || { fail "missing benchmark source: $bench_source"; return 1; }
	mkdir -p -- "$tool_dir"
	aarch64-linux-gnu-gcc -std=c11 -O2 -Wall -Wextra -Werror -static \
		-I"$repo_root/mailmsg" \
		"$bench_source" -o "$bench_binary"

	note "checking local MailMsg V1 revision 7 artifacts"
	for name in "${ARTIFACT_NAMES[@]}"; do
		local_path=$artifact_dir/$name
		[[ -f $local_path ]] || { fail "missing artifact: $local_path"; return 1; }
		verify_file_hash "$local_path" || return 1
		files+=("$local_path")
	done
	[[ -f $artifact_dir/artifact-manifest.txt ]] || {
		fail "missing artifact manifest"
		return 1
	}
	files+=("$artifact_dir/artifact-manifest.txt" "$script_dir/$SCRIPT_NAME" \
		"$bench_binary")
	pass "all local artifact hashes match the revision 7 manifest"

	if ((CHECK_ONLY)); then
		bash -n "$script_dir/$SCRIPT_NAME"
		pass "script syntax and local artifacts"
		return 0
	fi

	command -v ssh >/dev/null || { fail "ssh is not installed"; return 1; }
	command -v scp >/dev/null || { fail "scp is not installed"; return 1; }
	[[ -r $R1_IDENTITY ]] || { fail "SSH identity is not readable: $R1_IDENTITY"; return 1; }

	ssh_opts=(
		-F /dev/null
		-i "$R1_IDENTITY"
		-o IdentitiesOnly=yes
		-o PasswordAuthentication=no
		-o ConnectTimeout=6
		-o StrictHostKeyChecking=accept-new
	)
	target=$R1_USER@$R1_HOST

	note "read-only SSH identity and target preflight: $target"
	ssh "${ssh_opts[@]}" "$target" \
		'id -un; hostname; uname -r; test -d /userdata; printf "userdata-ok\\n"'

	note "uploading isolated test bundle to $target:$REMOTE_ROOT"
	printf -v remote_cmd 'umask 022; mkdir -p -- %q' "$REMOTE_ROOT"
	ssh "${ssh_opts[@]}" "$target" "$remote_cmd"
	scp "${ssh_opts[@]}" -- "${files[@]}" "$target:$REMOTE_ROOT/"

	printf -v remote_cmd 'chmod 0755 -- %q %q %q %q %q' \
		"$REMOTE_ROOT/$SCRIPT_NAME" \
		"$REMOTE_ROOT/mailmsg-user-client-aarch64" \
		"$REMOTE_ROOT/mailmsg-exclusive-reader-test-aarch64" \
		"$REMOTE_ROOT/mailmsg-offline-wait-test-aarch64" \
		"$REMOTE_ROOT/mailmsg-window-bench-aarch64"
	ssh "${ssh_opts[@]}" "$target" "$remote_cmd"

	note "verifying every transferred artifact on R1"
	for name in "${ARTIFACT_NAMES[@]}"; do
		expected=$(expected_sha256 "$name")
		printf -v remote_cmd 'sha256sum -- %q' "$REMOTE_ROOT/$name"
		remote_hash=$(ssh "${ssh_opts[@]}" "$target" "$remote_cmd")
		remote_hash=${remote_hash%% *}
		[[ $remote_hash == "$expected" ]] || {
			fail "R1 hash mismatch for $name"
			return 1
		}
		printf 'verified: %s\n' "$name"
	done
	for local_path in "$artifact_dir/artifact-manifest.txt" "$script_dir/$SCRIPT_NAME" \
		"$bench_binary"; do
		name=${local_path##*/}
		local_hash=$(sha256sum "$local_path")
		local_hash=${local_hash%% *}
		printf -v remote_cmd 'sha256sum -- %q' "$REMOTE_ROOT/$name"
		remote_hash=$(ssh "${ssh_opts[@]}" "$target" "$remote_cmd")
		remote_hash=${remote_hash%% *}
		[[ $remote_hash == "$local_hash" ]] || {
			fail "R1 hash mismatch for $name"
			return 1
		}
		printf 'verified: %s\n' "$name"
	done
	pass "upload and R1-side SHA-256 verification"
	printf 'U-Boot RAM FIT path: %s/mailmsg-v1-final.img\n' "$REMOTE_ROOT"

	if ((UPLOAD_ONLY)) || [[ -z $PROFILE ]]; then
		printf 'Upload complete. No board test was started.\n'
		return 0
	fi

	note "running remote profile: $PROFILE"
	printf -v remote_cmd \
		'exec %q --remote --remote-root %q --llm-root %q --profile %q' \
		"$REMOTE_ROOT/$SCRIPT_NAME" "$REMOTE_ROOT" "$LLM_ROOT" "$PROFILE"
	ssh -tt "${ssh_opts[@]}" "$target" "$remote_cmd"
}

# Remote board-test implementation starts here.
AMP=
REPORT=
CLIENT=
EXCLUSIVE_TEST=
WINDOW_BENCH=
SESSION_GENERATION=0
FOUR_PRIORITY_LLM_PID=
FOUR_PRIORITY_BENCH_PIDS=()

cleanup_four_priority_jobs()
{
	local pid

	if [[ ${FOUR_PRIORITY_LLM_PID:-} =~ ^[0-9]+$ ]]; then
		kill "$FOUR_PRIORITY_LLM_PID" 2>/dev/null || true
	fi
	for pid in "${FOUR_PRIORITY_BENCH_PIDS[@]:-}"; do
		[[ $pid =~ ^[0-9]+$ ]] || continue
		kill "$pid" 2>/dev/null || true
	done
}

remote_snapshot()
{
	set +e
	printf '\n--- diagnostic snapshot ---\n'
	[[ -n ${AMP:-} && -r $AMP/status ]] && cat "$AMP/status"
	[[ -n ${AMP:-} && -r $AMP/mailmsg_stats ]] && cat "$AMP/mailmsg_stats"
	[[ -n ${AMP:-} && -r $AMP/affinity_state ]] && cat "$AMP/affinity_state"
	printf '%s\n' '--- end snapshot ---'
	set -e
}

remote_fail()
{
	printf 'FAIL: %s\n' "$*" >&2
	remote_snapshot
	exit 1
}

discover_amp()
{
	local candidate
	local -a matches=()

	for candidate in /sys/bus/platform/devices/*; do
		[[ -f $candidate/status && -f $candidate/mailmsg_stats &&
		   -f $candidate/start && -f $candidate/image ]] || continue
		matches+=("$candidate")
	done
	((${#matches[@]} == 1)) || remote_fail \
		"expected one MailMsg AMP platform device, found ${#matches[@]}"
	AMP=${matches[0]}
	[[ -r $AMP/of_node/compatible ]] ||
		remote_fail "MailMsg platform device has no readable OF compatible"
	grep -aq 'youyeetoo,r1-amp-psci-cpu-on-heartbeat' \
		"$AMP/of_node/compatible" ||
		remote_fail "unexpected MailMsg platform-device compatible"
}

mailmsg_status()
{
	cat "$AMP/status"
}

mailmsg_state()
{
	local status
	status=$(mailmsg_status)
	status=${status#*mailmsg_state=}
	printf '%s\n' "${status%% *}"
}

mailmsg_worker_field()
{
	local wanted=$1
	local status worker part
	local -a fields

	status=$(mailmsg_status)
	worker=${status#*mailmsg_worker=}
	[[ $worker != "$status" ]] || remote_fail "status has no mailmsg_worker observation"
	worker=${worker%% *}
	IFS=/ read -r -a fields <<<"$worker"
	for part in "${fields[@]}"; do
		if [[ ${part%%:*} == "$wanted" ]]; then
			printf '%s\n' "${part#*:}"
			return 0
		fi
	done
	remote_fail "mailmsg_worker observation has no $wanted field"
}

mailmsg_tx_full_field()
{
	local wanted=$1
	local status observation part
	local -a fields

	status=$(mailmsg_status)
	observation=${status#*mailmsg_tx_full=}
	[[ $observation != "$status" ]] ||
		remote_fail "status has no mailmsg_tx_full observation"
	observation=${observation%% *}
	IFS=/ read -r -a fields <<<"$observation"
	for part in "${fields[@]}"; do
		if [[ ${part%%:*} == "$wanted" ]]; then
			printf '%s\n' "${part#*:}"
			return 0
		fi
	done
	remote_fail "mailmsg_tx_full observation has no $wanted field"
}

wait_state()
{
	local expected=$1
	local attempts=${2:-40}
	local index state

	for ((index = 0; index < attempts; index++)); do
		state=$(mailmsg_state)
		[[ $state == "$expected" ]] && return 0
		sleep 0.2
	done
	remote_fail "expected state $expected, got $(mailmsg_state)"
}

assert_session_active()
{
	local status pair local_generation peer_generation

	wait_state active 45
	status=$(mailmsg_status)
	pair=${status#*session=}
	pair=${pair%% *}
	local_generation=${pair%/*}
	peer_generation=${pair#*/}
	[[ $local_generation =~ ^[0-9]+$ && $peer_generation =~ ^[0-9]+$ ]] ||
		remote_fail "cannot parse session generation from status"
	((local_generation > 0 && local_generation == peer_generation)) ||
		remote_fail "session mismatch: $local_generation/$peer_generation"
	((local_generation > SESSION_GENERATION)) ||
		remote_fail "session generation did not advance: $SESSION_GENERATION -> $local_generation"
	SESSION_GENERATION=$local_generation
	grep -q 'session_result=0' <<<"$status" || remote_fail "SESSION_READY result is not zero"
	grep -q 'mailmsg_worker=valid:1/' <<<"$status" ||
		remote_fail "event-driven MailMsg worker observation is invalid"
	grep -q 'state=on (0)' "$AMP/affinity_state" || remote_fail "CPU3 is not ON"
	pass "SESSION_READY generation $local_generation and ACTIVE state"
}

prepare_unarmed()
{
	local state
	state=$(mailmsg_state)
	if [[ $state == offline ]]; then
		grep -q 'state=off (1)' "$AMP/affinity_state" ||
			remote_fail "OFFLINE state without fresh PSCI OFF"
		printf 'rearm\n' >"$AMP/rearm"
		state=$(mailmsg_state)
	fi
	[[ $state == unarmed ]] || remote_fail \
		"profile needs fresh unarmed state; current state is $state"
	grep -q 'state=off (1)' "$AMP/affinity_state" ||
		remote_fail "CPU3 must be OFF before image load"
}

start_image()
{
	local image=$1
	local image_size
	local status

	prepare_unarmed
	[[ -f $image ]] || remote_fail "missing Zephyr image: $image"
	verify_file_hash "$image" || remote_fail "bad Zephyr image hash: $image"
	image_size=$(stat -c %s "$image")
	note "loading ${image##*/} into the reserved CPU3 image window"
	command cat "$image" >"$AMP/image"
	status=$(mailmsg_status)
	grep -q "image=$image_size/$image_size" <<<"$status" ||
		remote_fail "kernel did not accept the complete ${image_size}-byte image"
	printf 'start\n' >"$AMP/start"
}

assert_all_depths_zero()
{
	local stats
	stats=$(cat "$AMP/mailmsg_stats")
	awk '
		/^p[0-3] / {
			seen++
			depths = 0
			for (i = 1; i <= NF; i++)
				if ($i == "depth=0") depths++
			if (depths != 2) bad = 1
		}
		END { exit !(seen == 4 && !bad) }
	' <<<"$stats" || remote_fail "one or more MailMsg rings are not empty"
}

run_client()
{
	local priority=$1
	local value=$2
	local expected=$((value + 1))
	local output

	if ! output=$("$CLIENT" "$priority" "$value" 2>&1); then
		printf '%s\n' "$output" >&2
		remote_fail "priority $priority user client failed"
	fi
	printf '%s\n' "$output"
	grep -q "value=$expected" <<<"$output" ||
		remote_fail "priority $priority did not return PONG value $expected"
	if ((priority <= 1)); then
		grep -Eq 'type=3 .*status=0' <<<"$output" ||
			remote_fail "reliable priority $priority did not return ACK"
	fi
}

run_four_priorities()
{
	local priority
	local -a values=(100 200 300 400)

	note "four-priority user ABI regression"
	for priority in 0 1 2 3; do
		run_client "$priority" "${values[$priority]}"
	done
	assert_all_depths_zero
	pass "p0/p1 ACK+PONG and p2/p3 PONG"
}

run_exclusive_reader()
{
	note "single-reader ownership and handoff"
	"$EXCLUSIVE_TEST" || remote_fail "exclusive-reader test failed"
	pass "second reader EBUSY and handoff EAGAIN"
}

run_crc_fault()
{
	local index output

	note "reliable p0 CRC rejection"
	printf '0 600\n' >"$AMP/mailmsg_crc_inject"
	for ((index = 0; index < 20; index++)); do
		output=$(cat "$AMP/mailmsg_response")
		if grep -Eq 'priority=0 valid=1 type=4 .*status=1' <<<"$output"; then
			printf '%s\n' "$output"
			grep -q 'type=2' <<<"$output" &&
				remote_fail "bad CRC unexpectedly produced a PONG"
			assert_all_depths_zero
			pass "bad CRC produced NACK_BAD_CRC"
			return 0
		fi
		sleep 0.1
	done
	remote_fail "CRC fault did not produce a p0 NACK"
}

p3_notify_failed_count()
{
	awk '
		$1 == "p3" {
			split($6, first, "=")
			split(first[2], count, "/")
			print count[3]
		}
	' "$AMP/mailmsg_stats"
}

run_notify_failure()
{
	local before after index output collected=

	note "p3 notification failure, retained frame, and recovery doorbell"
	before=$(p3_notify_failed_count)
	printf '3 -5 1\n' >"$AMP/mailmsg_notify_inject"
	printf '3 710\n' >"$AMP/mailmsg_ping"
	after=$(p3_notify_failed_count)
	[[ $after =~ ^[0-9]+$ && $before =~ ^[0-9]+$ && $after -eq $((before + 1)) ]] ||
		remote_fail "p3 notify_failed counter did not increment"
	printf 'clear\n' >"$AMP/mailmsg_notify_inject"
	printf '3 711\n' >"$AMP/mailmsg_ping"

	for ((index = 0; index < 30; index++)); do
		output=$(cat "$AMP/mailmsg_response")
		[[ $output == 'valid=0 reason=empty' ]] || collected+=$'\n'$output
		if grep -q 'priority=3 valid=1 type=2 .*value=711' <<<"$collected" &&
		   grep -q 'priority=3 valid=1 type=2 .*value=712' <<<"$collected"; then
			printf '%s\n' "$collected"
			assert_all_depths_zero
			pass "failed doorbell was observable; next doorbell drained both frames"
			return 0
		fi
		sleep 0.1
	done
	remote_fail "notification recovery did not return both p3 PONG frames"
}

run_queue_full_and_recover()
{
	local value index output expected count total depth collected=
	local before_irq before_wake before_drain before_msg
	local after_irq after_wake after_drain after_msg pending

	note "p3 seven-slot queue boundary and explicit recovery doorbell"
	before_irq=$(mailmsg_worker_field irq)
	before_wake=$(mailmsg_worker_field wake)
	before_drain=$(mailmsg_worker_field drain)
	before_msg=$(mailmsg_worker_field msg)
	for value in 800 801 802 803 804 805 806; do
		printf '3 %s\n' "$value" >"$AMP/mailmsg_queue_push" ||
			remote_fail "p3 queue became full before seven usable slots"
	done
	depth=$(awk '$1 == "p3" { split($5, value, "="); print value[2] }' \
		"$AMP/mailmsg_stats")
	[[ $depth == 7 ]] || remote_fail "p3 depth is $depth after seven queue-only writes"
	if printf '3 807\n' >"$AMP/mailmsg_queue_push" 2>/dev/null; then
		remote_fail "p3 eighth frame was unexpectedly accepted"
	fi
	depth=$(awk '$1 == "p3" { split($5, value, "="); print value[2] }' \
		"$AMP/mailmsg_stats")
	[[ $depth == 7 ]] || remote_fail "rejected eighth frame changed p3 depth to $depth"
	pass "eighth p3 frame was rejected while the seven-slot ring remained full"

	# The queue-only hook deliberately omits notification.  One raw ch3
	# doorbell wakes CPU3; the test service then drains the seven committed
	# frames.  This is test plumbing, not part of the MailMsg wire protocol.
	printf '3 0\n' >"$AMP/doorbell"
	for ((index = 0; index < 30; index++)); do
		output=$(cat "$AMP/mailmsg_response")
		[[ $output == 'valid=0 reason=empty' ]] || collected+=$'\n'$output
		if grep -q 'priority=3 valid=1 type=2 .*value=807' <<<"$collected"; then
			total=$(grep -Ec '^priority=3 valid=1 type=2 .*value=' <<<"$collected")
			[[ $total == 7 ]] || remote_fail "expected seven p3 PONGs, collected $total"
			for expected in 801 802 803 804 805 806 807; do
				count=$(grep -Ec "^priority=3 valid=1 type=2 .*value=$expected$" \
					<<<"$collected")
				[[ $count == 1 ]] || remote_fail \
					"expected one p3 PONG value=$expected, collected $count"
			done
			printf '%s\n' "$collected"
			assert_all_depths_zero
			after_irq=$(mailmsg_worker_field irq)
			after_wake=$(mailmsg_worker_field wake)
			after_drain=$(mailmsg_worker_field drain)
			after_msg=$(mailmsg_worker_field msg)
			pending=$(mailmsg_worker_field pending)
			((after_irq == before_irq + 1)) ||
				remote_fail "seven-frame drain used $((after_irq - before_irq)) IRQs"
			((after_wake == before_wake + 1)) ||
				remote_fail "seven-frame drain used $((after_wake - before_wake)) wakes"
			((after_drain == before_drain + 1)) ||
				remote_fail "seven-frame drain used $((after_drain - before_drain)) drain passes"
			((after_msg == before_msg + 7)) ||
				remote_fail "seven-frame drain processed $((after_msg - before_msg)) messages"
			[[ $pending == 0 || $pending == 0x0 ]] ||
				remote_fail "worker pending bitmap is not empty: $pending"
			pass "seven queued p3 frames drained without overwrite"
			return 0
		fi
		sleep 0.1
	done
	remote_fail "p3 queue recovery did not drain all seven frames"
}

run_stop_refused()
{
	local index status

	note "STOP_REFUSED and active data-plane recovery"
	assert_all_depths_zero
	printf 'stop\n' >"$AMP/mailmsg_stop"
	for ((index = 0; index < 35; index++)); do
		status=$(mailmsg_status)
		if grep -q 'mailmsg_state=active' <<<"$status" &&
		   grep -q 'stop_reply=7' <<<"$status" &&
		   grep -q 'stop_result=-125' <<<"$status"; then
			printf '%s\n' "$status"
			run_client 0 900
			pass "STOP_REFUSED preserved ACTIVE and p0 usability"
			return 0
		fi
		sleep 0.2
	done
	remote_fail "normal image did not return STOP_REFUSED"
}

run_controlled_stop()
{
	local index status

	note "STOP_READY, CPU_OFF, and OFFLINE"
	assert_all_depths_zero
	grep -q 'a2b_now=m0:0' <<<"$(mailmsg_status)" ||
		remote_fail "mailbox0 still has an A2B pending bit before STOP"
	printf 'stop\n' >"$AMP/mailmsg_stop"
	for ((index = 0; index < 45; index++)); do
		status=$(mailmsg_status)
		if grep -q 'mailmsg_state=offline' <<<"$status" &&
		   grep -q 'stop_reply=6' <<<"$status" &&
		   grep -q 'stop_result=0' <<<"$status"; then
			grep -q 'state=off (1)' "$AMP/affinity_state" ||
				remote_fail "STOP_READY completed but CPU3 affinity is not OFF"
			grep -Eq 'a2b_now=m0:0x0( |$)' <<<"$status" ||
				remote_fail "CPU3 is OFF but an old A2B doorbell remains pending"
			printf '%s\n' "$status"
			pass "controlled STOP reached OFFLINE"
			return 0
		fi
		sleep 0.2
	done
	remote_fail "controlled STOP did not reach OFFLINE"
}

run_controlled_profile()
{
	local image=$REMOTE_ROOT/mailmsg-v1-controlled-stop.bin
	local image_size
	image_size=$(stat -c %s "$image")

	start_image "$image"
	assert_session_active
	# This must precede the first Linux A2B doorbell.  The current Zephyr
	# Run before the first doorbell so this deterministic queue-only probe is
	# independent of the event-driven responder.
	run_queue_full_and_recover
	run_four_priorities
	run_exclusive_reader
	run_crc_fault
	run_notify_failure
	note "per-priority final statistics"
	cat "$AMP/mailmsg_stats"
	run_controlled_stop

	note "guarded rearm and second-session regression"
	printf 'rearm\n' >"$AMP/rearm"
	[[ $(mailmsg_state) == unarmed ]] || remote_fail "rearm did not return UNARMED"
	grep -q "image=0/$image_size" <<<"$(mailmsg_status)" ||
		remote_fail "rearm did not clear image progress"
	start_image "$image"
	assert_session_active
	run_client 0 1000
	assert_all_depths_zero
	run_controlled_stop
	pass "controlled profile complete; CPU3 is OFF"
}

run_stop_refused_profile()
{
	start_image "$REMOTE_ROOT/mailmsg-v1-normal.bin"
	assert_session_active
	run_client 0 1100
	run_stop_refused
	remote_snapshot
	pass "stop-refused profile complete; CPU3 intentionally remains ON"
}

run_start_timeout_profile()
{
	local status output

	start_image "$REMOTE_ROOT/mailmsg-v1-start-timeout.bin"
	wait_state start-timeout 45
	status=$(mailmsg_status)
	grep -q 'session_result=-110' <<<"$status" ||
		remote_fail "START timeout did not report ETIMEDOUT"
	printf '%s\n' "$status"
	if output=$("$CLIENT" 0 1200 --no-read 2>&1); then
		remote_fail "data plane accepted a write after START timeout"
	fi
	printf '%s\n' "$output"
	grep -q 'Connection timed out' <<<"$output" ||
		remote_fail "post-START-timeout write failed for an unexpected reason"
	pass "START timeout is terminal and data plane remains closed"
	printf 'A fresh U-Boot RAM boot is required before another profile.\n'
}

run_stop_timeout_profile()
{
	local status output

	start_image "$REMOTE_ROOT/mailmsg-v1-stop-timeout.bin"
	assert_session_active
	run_client 1 1300
	assert_all_depths_zero
	printf 'stop\n' >"$AMP/mailmsg_stop"
	wait_state stop-timeout 45
	status=$(mailmsg_status)
	grep -q 'stop_result=-110' <<<"$status" ||
		remote_fail "STOP timeout did not report ETIMEDOUT"
	grep -q 'state=on (0)' "$AMP/affinity_state" ||
		remote_fail "STOP timeout candidate unexpectedly powered CPU3 off"
	printf '%s\n' "$status"
	if output=$("$CLIENT" 1 1301 --no-read 2>&1); then
		remote_fail "data plane accepted a write after STOP timeout"
	fi
	printf '%s\n' "$output"
	grep -q 'Connection timed out' <<<"$output" ||
		remote_fail "post-STOP-timeout write failed for an unexpected reason"
	pass "STOP timeout is terminal and CPU3 remains ON"
	printf 'A fresh U-Boot RAM boot is required before another profile.\n'
}

stats_error_total()
{
	awk '
		/^p[0-3] / {
			for (i = 1; i <= NF; i++) {
				if ($i ~ /^(full|incomplete|crc|invalid|stale)=/) {
					split($i, field, "=")
					total += field[2]
				}
			}
		}
		END { print total + 0 }
	' "$AMP/mailmsg_stats"
}

stats_protocol_error_total()
{
	awk '
		/^p[0-3] / {
			for (i = 1; i <= NF; i++) {
				if ($i ~ /^(incomplete|crc|invalid|stale)=/) {
					split($i, field, "=")
					total += field[2]
				}
			}
		}
		END { print total + 0 }
	' "$AMP/mailmsg_stats"
}

priority_full_count()
{
	local priority=$1

	awk -v wanted="p$priority" '
		$1 == wanted {
			for (i = 1; i <= NF; i++)
				if ($i ~ /^full=/) {
					split($i, field, "=")
					print field[2]
					exit
				}
		}
	' "$AMP/mailmsg_stats"
}

wait_worker_idle_at()
{
	local expected_msg=$1
	local index msg pending

	for ((index = 0; index < 50; index++)); do
		msg=$(mailmsg_worker_field msg)
		pending=$(mailmsg_worker_field pending)
		if [[ $msg == "$expected_msg" && ($pending == 0 || $pending == 0x0) ]]; then
			return 0
		fi
		sleep 0.1
	done
	remote_fail "worker did not settle at msg=$expected_msg pending=0; got msg=$msg pending=$pending"
}

run_llm_coexistence_profile()
{
	local image=$REMOTE_ROOT/mailmsg-v1-normal.bin
	local llm_bin=$LLM_ROOT/llm_demo-amp
	local model=$LLM_ROOT/models/DeepSeek-R1-Distill-Qwen-1.5B_W8A8_RK3588.rkllm
	local llm_log=${REPORT%.log}-rkllm.log
	local llm_pid index value output llm_rc=0 overlap=0
	local before_msg after_msg before_errors after_errors pending

	[[ -x $llm_bin ]] || remote_fail "RKLLM demo is not executable: $llm_bin"
	[[ -r $model ]] || remote_fail "RKLLM model is not readable: $model"
	[[ -d $LLM_ROOT/lib ]] || remote_fail "RKLLM library directory is absent"
	command -v timeout >/dev/null || remote_fail "timeout is required"
	command -v stdbuf >/dev/null || remote_fail "stdbuf is required"
	free -h
	df -h "$LLM_ROOT" "$REMOTE_ROOT"

	start_image "$image"
	assert_session_active
	note "pre-RKLLM p0 regression"
	run_client 0 41
	assert_all_depths_zero
	before_msg=$(mailmsg_worker_field msg)
	before_errors=$(stats_error_total)

	note "starting RKLLM generation with concurrent p2 traffic"
	(
		printf '0\n'
		sleep 15
		printf 'exit\n'
	) | timeout 300 stdbuf -oL -eL env \
		LD_LIBRARY_PATH="$LLM_ROOT/lib" RKLLM_LOG_LEVEL=1 \
		"$llm_bin" "$model" 64 512 >"$llm_log" 2>&1 &
	llm_pid=$!

	for ((index = 0; index < 600; index++)); do
		grep -q 'rkllm init success' "$llm_log" && break
		kill -0 "$llm_pid" 2>/dev/null || break
		sleep 0.1
	done
	grep -q 'rkllm init success' "$llm_log" || {
		set +e
		wait "$llm_pid"
		llm_rc=$?
		set -e
		cat "$llm_log"
		remote_fail "RKLLM did not reach init success, exit=$llm_rc"
	}

	for ((index = 0; index < 16; index++)); do
		kill -0 "$llm_pid" 2>/dev/null ||
			remote_fail "RKLLM exited before concurrent p2 iteration $index"
		value=$((1500 + index))
		if ! output=$("$CLIENT" 2 "$value" 2>&1); then
			printf '%s\n' "$output" >&2
			remote_fail "concurrent p2 iteration $index failed"
		fi
		printf '%s\n' "$output"
		grep -q "value=$((value + 1))" <<<"$output" ||
			remote_fail "concurrent p2 iteration $index returned wrong value"
		((overlap += 1))
		sleep 0.1
	done

	set +e
	wait "$llm_pid"
	llm_rc=$?
	set -e
	cat "$llm_log"
	((llm_rc == 0)) || remote_fail "RKLLM exited with status $llm_rc"
	grep -Fq 'rkllm-runtime version: 1.3.0, rknpu driver version: 0.9.8' \
		"$llm_log" || remote_fail "unexpected RKLLM/RKNPU version"
	grep -Fq 'Enabled cpus: [3, 4, 5, 6]' "$llm_log" ||
		remote_fail "RKLLM did not use the expected Linux CPU mask"
	grep -Fq 'Enabled cpus num: 4' "$llm_log" ||
		remote_fail "RKLLM enabled CPU count is not four"
	grep -Fq 'rkllm init success' "$llm_log" || remote_fail "RKLLM init failed"
	grep -q 'robot:.' "$llm_log" || remote_fail "RKLLM generated no visible text"
	if grep -Eq 'matmul\(w8a8\) run failed|rkllm init failed|Mismatch between enabled CPUs' \
		"$llm_log"; then
		remote_fail "RKLLM log contains a known failure"
	fi

	note "post-RKLLM p0 regression"
	run_client 0 1499
	assert_all_depths_zero
	after_msg=$(mailmsg_worker_field msg)
	after_errors=$(stats_error_total)
	pending=$(mailmsg_worker_field pending)
	((overlap == 16)) || remote_fail "only $overlap p2 requests overlapped RKLLM"
	((after_msg == before_msg + 17)) || remote_fail \
		"worker processed $((after_msg - before_msg)) messages; expected 17"
	((after_errors == before_errors)) || remote_fail \
		"MailMsg error counters changed: $before_errors -> $after_errors"
	[[ $pending == 0 || $pending == 0x0 ]] ||
		remote_fail "worker pending bitmap is not empty: $pending"
	[[ $(mailmsg_state) == active ]] || remote_fail "MailMsg session is no longer active"
	grep -q 'state=on (0)' "$AMP/affinity_state" || remote_fail "CPU3 is no longer ON"
	grep -Eq 'a2b_now=m0:0x0( |$)' <<<"$(mailmsg_status)" ||
		remote_fail "an A2B doorbell remains pending"
	remote_snapshot
	pass "RKLLM generation coexisted with 16 p2 requests and post-run p0"
}

run_llm_soak_profile()
{
	local image=$REMOTE_ROOT/mailmsg-v1-normal.bin
	local llm_bin=$LLM_ROOT/llm_demo-amp
	local model=$LLM_ROOT/models/DeepSeek-R1-Distill-Qwen-1.5B_W8A8_RK3588.rkllm
	local llm_log=${REPORT%.log}-rkllm.log
	local traffic_log=${REPORT%.log}-traffic.csv
	local rounds=24 timeout_seconds=300 traffic_seconds=270
	local llm_pid llm_rc=0 index priority value output
	local start_ns end_ns latency_ns traffic_start total=0
	local generate_count robot_count before_msg after_msg expected_msg
	local before_errors after_errors pending
	local -a success=(0 0 0 0)

	[[ -x $llm_bin ]] || remote_fail "RKLLM demo is not executable: $llm_bin"
	[[ -r $model ]] || remote_fail "RKLLM model is not readable: $model"
	[[ -d $LLM_ROOT/lib ]] || remote_fail "RKLLM library directory is absent"
	command -v timeout >/dev/null || remote_fail "timeout is required"
	command -v stdbuf >/dev/null || remote_fail "stdbuf is required"
	command -v date >/dev/null || remote_fail "date is required"
	free -h
	df -h "$LLM_ROOT" "$REMOTE_ROOT"

	start_image "$image"
	assert_session_active
	assert_all_depths_zero
	before_msg=$(mailmsg_worker_field msg)
	before_errors=$(stats_error_total)
	printf 'priority,request,value,latency_ns\n' >"$traffic_log"

	note "starting $rounds sequential RKLLM generations and sustained p0-p3 traffic"
	(
		for ((index = 0; index < rounds; index++)); do
			printf '0\n'
		done
		printf 'exit\n'
	) | timeout "$timeout_seconds" stdbuf -oL -eL env \
		LD_LIBRARY_PATH="$LLM_ROOT/lib" RKLLM_LOG_LEVEL=1 \
		"$llm_bin" "$model" 64 512 >"$llm_log" 2>&1 &
	llm_pid=$!
	trap "kill $llm_pid 2>/dev/null || true" EXIT

	for ((index = 0; index < 600; index++)); do
		grep -q 'rkllm init success' "$llm_log" && break
		kill -0 "$llm_pid" 2>/dev/null || break
		sleep 0.1
	done
	grep -q 'rkllm init success' "$llm_log" || {
		set +e
		wait "$llm_pid"
		llm_rc=$?
		set -e
		trap - EXIT
		cat "$llm_log"
		remote_fail "RKLLM did not reach init success, exit=$llm_rc"
	}

	traffic_start=$SECONDS
	while kill -0 "$llm_pid" 2>/dev/null &&
	      ((SECONDS - traffic_start < traffic_seconds)); do
		priority=$((total % 4))
		value=$((2000 + total))
		start_ns=$(date +%s%N)
		if ! output=$("$CLIENT" "$priority" "$value" 2>&1); then
			printf '%s\n' "$output" >&2
			remote_fail "soak traffic request $total on p$priority failed"
		fi
		end_ns=$(date +%s%N)
		latency_ns=$((end_ns - start_ns))
		grep -q "value=$((value + 1))" <<<"$output" ||
			remote_fail "soak traffic request $total returned a wrong value"
		if ((priority <= 1)); then
			grep -Eq 'type=3 .*status=0' <<<"$output" ||
				remote_fail "reliable p$priority request $total returned no ACK"
		fi
		printf '%d,%d,%d,%d\n' "$priority" "$total" "$value" "$latency_ns" \
			>>"$traffic_log"
		((success[priority] += 1))
		((total += 1))
		if ((total % 100 == 0)); then
			printf 'traffic_progress=%d elapsed=%ds\n' \
				"$total" "$((SECONDS - traffic_start))"
		fi
		sleep 0.25
	done

	set +e
	wait "$llm_pid"
	llm_rc=$?
	set -e
	trap - EXIT
	((llm_rc == 0)) || {
		cat "$llm_log"
		remote_fail "RKLLM soak exited with status $llm_rc"
	}
	grep -Fq 'rkllm-runtime version: 1.3.0, rknpu driver version: 0.9.8' \
		"$llm_log" || remote_fail "unexpected RKLLM/RKNPU version"
	grep -Fq 'Enabled cpus: [3, 4, 5, 6]' "$llm_log" ||
		remote_fail "RKLLM did not use the expected Linux CPU mask"
	grep -Fq 'Enabled cpus num: 4' "$llm_log" ||
		remote_fail "RKLLM enabled CPU count is not four"
	if grep -Eq 'matmul\(w8a8\) run failed|rkllm init failed|Mismatch between enabled CPUs' \
		"$llm_log"; then
		remote_fail "RKLLM soak log contains a known failure"
	fi
	generate_count=$(grep -Ec '^I rkllm:  Generate[[:space:]]' "$llm_log" || true)
	robot_count=$(grep -c 'robot:' "$llm_log" || true)
	((generate_count == rounds)) || remote_fail \
		"completed $generate_count RKLLM generations; expected $rounds"
	((robot_count >= rounds)) || remote_fail \
		"observed $robot_count robot prompts; expected at least $rounds"
	for priority in 0 1 2 3; do
		((success[priority] > 0)) || remote_fail \
			"priority $priority completed no request during RKLLM"
	done

	note "post-soak four-priority regression"
	run_four_priorities
	after_msg=$(mailmsg_worker_field msg)
	after_errors=$(stats_error_total)
	pending=$(mailmsg_worker_field pending)
	expected_msg=$((before_msg + total + 4))
	((after_msg == expected_msg)) || remote_fail \
		"worker msg=$after_msg, expected $expected_msg from $total soak plus four post checks"
	((after_errors == before_errors)) || remote_fail \
		"MailMsg error counters changed: $before_errors -> $after_errors"
	[[ $pending == 0 || $pending == 0x0 ]] ||
		remote_fail "worker pending bitmap is not empty: $pending"
	[[ $(mailmsg_state) == active ]] || remote_fail "MailMsg session is no longer active"
	grep -q 'state=on (0)' "$AMP/affinity_state" || remote_fail "CPU3 is no longer ON"
	grep -Eq 'a2b_now=m0:0x0( |$)' <<<"$(mailmsg_status)" ||
		remote_fail "an A2B doorbell remains pending"
	assert_all_depths_zero

	printf 'rkllm_generations=%d traffic_total=%d p0=%d p1=%d p2=%d p3=%d\n' \
		"$generate_count" "$total" "${success[0]}" "${success[1]}" \
		"${success[2]}" "${success[3]}"
	awk -F, '
		NR > 1 {
			count[$1]++
			sum[$1] += $4
			if (!(($1) in min) || $4 < min[$1]) min[$1] = $4
			if ($4 > max[$1]) max[$1] = $4
		}
		END {
			for (p = 0; p < 4; p++)
				printf "p%d latency_ms count=%d min=%.3f avg=%.3f max=%.3f\n",
				       p, count[p], min[p] / 1000000,
				       sum[p] / count[p] / 1000000, max[p] / 1000000
		}
	' "$traffic_log"
	free -h
	remote_snapshot
	pass "$rounds RKLLM generations coexisted with $total round-robin MailMsg requests"
}

benchmark_summary_field()
{
	local summary=$1
	local wanted=$2

	awk -v wanted="$wanted" '
		{
			for (i = 1; i <= NF; i++) {
				split($i, field, "=")
				if (field[1] == wanted) {
					print field[2]
					exit
				}
			}
		}
	' <<<"$summary"
}

run_llm_p2_window_profile()
{
	local image=$REMOTE_ROOT/mailmsg-v1-normal.bin
	local llm_bin=$LLM_ROOT/llm_demo-amp
	local model=$LLM_ROOT/models/DeepSeek-R1-Distill-Qwen-1.5B_W8A8_RK3588.rkllm
	local llm_log=${REPORT%.log}-rkllm.log
	local bench_log=${REPORT%.log}-p2-window.log
	local rounds=24 timeout_seconds=300 duration=8
	local llm_pid llm_rc=0 index window base output summary
	local write_count pong_count lost_count enospc_count eagain_count timeout_count max_inflight
	local total_write=0 total_pong=0 total_lost=0 total_timeout=0 total_enospc=0
	local generate_count robot_count before_msg after_msg expected_msg
	local before_protocol_errors after_protocol_errors before_full after_full
	local before_window_msg after_window_msg reverse_full_before reverse_full_after
	local reverse_full_delta reverse_full_priority reverse_full_type reverse_full_result
	local pending
	local -a windows=(1 2 4 7 8 16 32)

	[[ -x $llm_bin ]] || remote_fail "RKLLM demo is not executable: $llm_bin"
	[[ -r $model ]] || remote_fail "RKLLM model is not readable: $model"
	[[ -d $LLM_ROOT/lib ]] || remote_fail "RKLLM library directory is absent"
	[[ -x $WINDOW_BENCH ]] || remote_fail "window benchmark is not executable"
	command -v timeout >/dev/null || remote_fail "timeout is required"
	command -v stdbuf >/dev/null || remote_fail "stdbuf is required"
	free -h
	df -h "$LLM_ROOT" "$REMOTE_ROOT"

	start_image "$image"
	assert_session_active
	assert_all_depths_zero
	before_msg=$(mailmsg_worker_field msg)
	before_protocol_errors=$(stats_protocol_error_total)
	before_full=$(priority_full_count 2)
	[[ $before_full =~ ^[0-9]+$ ]] || remote_fail "cannot parse baseline p2 full count"
	: >"$bench_log"

	note "starting $rounds RKLLM generations and stepped p2 in-flight windows"
	(
		for ((index = 0; index < rounds; index++)); do
			printf '0\n'
		done
		printf 'exit\n'
	) | timeout "$timeout_seconds" stdbuf -oL -eL env \
		LD_LIBRARY_PATH="$LLM_ROOT/lib" RKLLM_LOG_LEVEL=1 \
		"$llm_bin" "$model" 64 512 >"$llm_log" 2>&1 &
	llm_pid=$!
	trap "kill $llm_pid 2>/dev/null || true" EXIT

	for ((index = 0; index < 600; index++)); do
		grep -q 'rkllm init success' "$llm_log" && break
		kill -0 "$llm_pid" 2>/dev/null || break
		sleep 0.1
	done
	grep -q 'rkllm init success' "$llm_log" || {
		set +e
		wait "$llm_pid"
		llm_rc=$?
		set -e
		trap - EXIT
		cat "$llm_log"
		remote_fail "RKLLM did not reach init success, exit=$llm_rc"
	}

	for index in "${!windows[@]}"; do
		window=${windows[$index]}
		base=$((1000000 + index * 200000))
		before_window_msg=$(mailmsg_worker_field msg)
		reverse_full_before=$(mailmsg_tx_full_field count)
		[[ $before_window_msg =~ ^[0-9]+$ && $reverse_full_before =~ ^[0-9]+$ ]] ||
			remote_fail "cannot parse p2 window baseline observations"
		kill -0 "$llm_pid" 2>/dev/null ||
			remote_fail "RKLLM exited before p2 window $window"
		note "p2 window=$window duration=${duration}s under RKLLM"
		if ! output=$(timeout "$((duration + 10))" \
			"$WINDOW_BENCH" 2 "$window" "$duration" "$base" 2>&1); then
			printf '%s\n' "$output" | tee -a "$bench_log"
			remote_fail "p2 window $window benchmark failed"
		fi
		printf '%s\n' "$output" | tee -a "$bench_log"
		summary=$(grep -E '^priority=2 ' <<<"$output" | tail -n 1 || true)
		[[ -n $summary ]] || remote_fail "p2 window $window has no summary"
		write_count=$(benchmark_summary_field "$summary" write_ok)
		pong_count=$(benchmark_summary_field "$summary" pong)
		lost_count=$(benchmark_summary_field "$summary" lost)
		enospc_count=$(benchmark_summary_field "$summary" enospc)
		eagain_count=$(benchmark_summary_field "$summary" eagain)
		timeout_count=$(benchmark_summary_field "$summary" timeout)
		max_inflight=$(benchmark_summary_field "$summary" max_inflight)
		[[ $write_count =~ ^[0-9]+$ && $pong_count =~ ^[0-9]+$ &&
		   $lost_count =~ ^[0-9]+$ &&
		   $enospc_count =~ ^[0-9]+$ && $eagain_count =~ ^[0-9]+$ &&
		   $timeout_count =~ ^[0-9]+$ &&
		   $max_inflight =~ ^[0-9]+$ ]] ||
			remote_fail "cannot parse p2 window $window summary"
		((write_count > 0 && pong_count <= write_count &&
		  lost_count == write_count - pong_count &&
		  timeout_count == lost_count)) ||
			remote_fail "p2 window $window has inconsistent response loss accounting"
		if ((window <= 7)); then
			((pong_count == write_count && timeout_count == 0)) ||
				remote_fail "p2 safe window $window lost an accepted request"
		else
			printf 'p2_window_boundary=%d accepted=%d pong=%d lost=%d timeout=%d\n' \
				"$window" "$write_count" "$pong_count" "$lost_count" "$timeout_count"
		fi
		((eagain_count == 0)) ||
			remote_fail "p2 window $window observed unexpected EAGAIN=$eagain_count"
		((max_inflight <= window)) ||
			remote_fail "p2 window $window exceeded its configured in-flight bound"
		wait_worker_idle_at "$((before_window_msg + write_count))"
		after_window_msg=$(mailmsg_worker_field msg)
		reverse_full_after=$(mailmsg_tx_full_field count)
		reverse_full_delta=$((reverse_full_after - reverse_full_before))
		((after_window_msg == before_window_msg + write_count)) ||
			remote_fail "p2 window $window worker processed $((after_window_msg - before_window_msg)); expected $write_count"
		((reverse_full_after >= reverse_full_before)) ||
			remote_fail "reverse TX-full counter moved backwards at p2 window $window"
		if ((lost_count)); then
			reverse_full_priority=$(mailmsg_tx_full_field priority)
			reverse_full_type=$(mailmsg_tx_full_field type)
			reverse_full_result=$(mailmsg_tx_full_field result)
			printf 'p2_window_response_observation=%d lost=%d reverse_full_delta=%d reverse_full_last=priority:%s/type:%s/result:%s\n' \
				"$window" "$lost_count" "$reverse_full_delta" \
				"$reverse_full_priority" "$reverse_full_type" "$reverse_full_result"
		fi
		((total_write += write_count))
		((total_pong += pong_count))
		total_lost=$((total_lost + lost_count))
		total_timeout=$((total_timeout + timeout_count))
		total_enospc=$((total_enospc + enospc_count))
		kill -0 "$llm_pid" 2>/dev/null ||
			remote_fail "RKLLM exited during p2 window $window"
	done

	set +e
	wait "$llm_pid"
	llm_rc=$?
	set -e
	trap - EXIT
	((llm_rc == 0)) || {
		cat "$llm_log"
		remote_fail "RKLLM window test exited with status $llm_rc"
	}
	grep -Fq 'rkllm-runtime version: 1.3.0, rknpu driver version: 0.9.8' \
		"$llm_log" || remote_fail "unexpected RKLLM/RKNPU version"
	grep -Fq 'Enabled cpus: [3, 4, 5, 6]' "$llm_log" ||
		remote_fail "RKLLM did not use the expected Linux CPU mask"
	grep -Fq 'Enabled cpus num: 4' "$llm_log" ||
		remote_fail "RKLLM enabled CPU count is not four"
	if grep -Eq 'matmul\(w8a8\) run failed|rkllm init failed|Mismatch between enabled CPUs' \
		"$llm_log"; then
		remote_fail "RKLLM window log contains a known failure"
	fi
	generate_count=$(grep -Ec '^I rkllm:  Generate[[:space:]]' "$llm_log" || true)
	robot_count=$(grep -c 'robot:' "$llm_log" || true)
	((generate_count == rounds)) || remote_fail \
		"completed $generate_count RKLLM generations; expected $rounds"
	((robot_count >= rounds)) || remote_fail \
		"observed $robot_count robot prompts; expected at least $rounds"

	note "post-window four-priority regression"
	run_four_priorities
	after_msg=$(mailmsg_worker_field msg)
	after_protocol_errors=$(stats_protocol_error_total)
	after_full=$(priority_full_count 2)
	[[ $after_full =~ ^[0-9]+$ ]] || remote_fail "cannot parse final p2 full count"
	pending=$(mailmsg_worker_field pending)
	expected_msg=$((before_msg + total_write + 4))
	wait_worker_idle_at "$expected_msg"
	after_msg=$(mailmsg_worker_field msg)
	pending=$(mailmsg_worker_field pending)
	((after_msg == expected_msg)) || remote_fail \
		"worker msg=$after_msg, expected $expected_msg from benchmark plus post checks"
	((after_protocol_errors == before_protocol_errors)) || remote_fail \
		"protocol error counters changed: $before_protocol_errors -> $after_protocol_errors"
	((after_full - before_full == total_enospc)) || remote_fail \
		"p2 full delta $((after_full - before_full)) != observed ENOSPC $total_enospc"
	[[ $pending == 0 || $pending == 0x0 ]] ||
		remote_fail "worker pending bitmap is not empty: $pending"
	[[ $(mailmsg_state) == active ]] || remote_fail "MailMsg session is no longer active"
	grep -q 'state=on (0)' "$AMP/affinity_state" || remote_fail "CPU3 is no longer ON"
	grep -Eq 'a2b_now=m0:0x0( |$)' <<<"$(mailmsg_status)" ||
		remote_fail "an A2B doorbell remains pending"
	assert_all_depths_zero

	printf 'rkllm_generations=%d p2_write_ok=%d p2_pong=%d p2_lost=%d p2_timeout=%d p2_enospc=%d\n' \
		"$generate_count" "$total_write" "$total_pong" "$total_lost" \
		"$total_timeout" "$total_enospc"
	free -h
	remote_snapshot
	pass "p2 windows 1,2,4,7,8,16,32 completed during RKLLM generation"
}

run_llm_four_priority_profile()
{
	local image=$REMOTE_ROOT/mailmsg-v1-normal.bin
	local llm_bin=$LLM_ROOT/llm_demo-amp
	local model=$LLM_ROOT/models/DeepSeek-R1-Distill-Qwen-1.5B_W8A8_RK3588.rkllm
	local llm_log=${REPORT%.log}-rkllm.log
	local bench_prefix=${REPORT%.log}-four-p
	local rounds=24 timeout_seconds=300 duration=8 window stage window_list
	local llm_pid llm_rc=0 priority bench_rc base bench_log summary
	local write_count pong_count ack_count lost_count timeout_count
	local enospc_count eagain_count write_other_count nack_count
	local duplicate_count unknown_count max_inflight
	local before_msg after_msg expected_msg before_errors after_errors pending
	local stage_before_msg stage_after_msg stage_before_errors stage_after_errors
	local stage_write total_write=0
	local generate_count robot_count
	local -a bench_pids=() bench_rcs=() bench_logs=()
	local -a stage_before_full=(0 0 0 0) after_full=(0 0 0 0)
	local -a write_counts=(0 0 0 0) pong_counts=(0 0 0 0)
	local -a ack_counts=(0 0 0 0)
	local -a windows=(1)

	if [[ $PROFILE == llm-four-priority-sweep ]]; then
		windows=(2 4 7)
	fi
	printf -v window_list '%s,' "${windows[@]}"
	window_list=${window_list%,}

	[[ -x $llm_bin ]] || remote_fail "RKLLM demo is not executable: $llm_bin"
	[[ -r $model ]] || remote_fail "RKLLM model is not readable: $model"
	[[ -d $LLM_ROOT/lib ]] || remote_fail "RKLLM library directory is absent"
	[[ -x $WINDOW_BENCH ]] || remote_fail "window benchmark is not executable"
	command -v timeout >/dev/null || remote_fail "timeout is required"
	command -v stdbuf >/dev/null || remote_fail "stdbuf is required"
	free -h
	df -h "$LLM_ROOT" "$REMOTE_ROOT"

	start_image "$image"
	assert_session_active
	assert_all_depths_zero
	before_msg=$(mailmsg_worker_field msg)
	before_errors=$(stats_protocol_error_total)

	note "starting $rounds RKLLM generations and simultaneous p0-p3 windows=$window_list traffic"
	(
		for ((priority = 0; priority < rounds; priority++)); do
			printf '0\n'
		done
		printf 'exit\n'
	) | timeout "$timeout_seconds" stdbuf -oL -eL env \
		LD_LIBRARY_PATH="$LLM_ROOT/lib" RKLLM_LOG_LEVEL=1 \
		"$llm_bin" "$model" 64 512 >"$llm_log" 2>&1 &
	llm_pid=$!
	FOUR_PRIORITY_LLM_PID=$llm_pid
	FOUR_PRIORITY_BENCH_PIDS=()
	trap cleanup_four_priority_jobs EXIT

	for ((priority = 0; priority < 600; priority++)); do
		grep -q 'rkllm init success' "$llm_log" && break
		kill -0 "$llm_pid" 2>/dev/null || break
		sleep 0.1
	done
	grep -q 'rkllm init success' "$llm_log" || {
		if wait "$llm_pid"; then llm_rc=0; else llm_rc=$?; fi
		FOUR_PRIORITY_LLM_PID=
		cat "$llm_log"
		remote_fail "RKLLM did not reach init success, exit=$llm_rc"
	}

	# Each process owns one priority-scoped reader.  The four independent
	# processes overlap for each eight-second measurement window; this is a
	# coexistence/load test, not a scheduler fairness measurement.
	for stage in "${!windows[@]}"; do
		window=${windows[$stage]}
		kill -0 "$llm_pid" 2>/dev/null ||
			remote_fail "RKLLM exited before simultaneous window $window"
		assert_all_depths_zero
		stage_before_msg=$(mailmsg_worker_field msg)
		stage_before_errors=$(stats_protocol_error_total)
		for priority in 0 1 2 3; do
			stage_before_full[$priority]=$(priority_full_count "$priority")
			[[ ${stage_before_full[$priority]} =~ ^[0-9]+$ ]] ||
				remote_fail "cannot parse window $window p$priority full baseline"
		done
		stage_write=0
		bench_pids=()
		bench_rcs=()
		bench_logs=()
		FOUR_PRIORITY_BENCH_PIDS=()

		note "launching four priorities window=$window duration=${duration}s"
		for priority in 0 1 2 3; do
			bench_log=${bench_prefix}-w${window}-p${priority}.log
			bench_logs[$priority]=$bench_log
			base=$((stage * 100000000 + priority * 1000000 + 1000000))
			timeout "$((duration + 10))" "$WINDOW_BENCH" \
				"$priority" "$window" "$duration" "$base" >"$bench_log" 2>&1 &
			bench_pids[$priority]=$!
			FOUR_PRIORITY_BENCH_PIDS[$priority]=${bench_pids[$priority]}
		done

		for priority in 0 1 2 3; do
			if wait "${bench_pids[$priority]}"; then
				bench_rc=0
			else
				bench_rc=$?
			fi
			bench_rcs[$priority]=$bench_rc
		done

		for priority in 0 1 2 3; do
			bench_log=${bench_logs[$priority]}
			printf '\n--- window=%d p%d benchmark ---\n' "$window" "$priority"
			cat "$bench_log"
			((bench_rcs[$priority] == 0)) ||
				remote_fail "window $window p$priority benchmark exited ${bench_rcs[$priority]}"
			summary=$(grep -E "^priority=$priority " "$bench_log" | tail -n 1 || true)
			[[ -n $summary ]] ||
				remote_fail "window $window p$priority benchmark has no summary"
			write_count=$(benchmark_summary_field "$summary" write_ok)
			pong_count=$(benchmark_summary_field "$summary" pong)
			ack_count=$(benchmark_summary_field "$summary" ack)
			lost_count=$(benchmark_summary_field "$summary" lost)
			timeout_count=$(benchmark_summary_field "$summary" timeout)
			enospc_count=$(benchmark_summary_field "$summary" enospc)
			eagain_count=$(benchmark_summary_field "$summary" eagain)
			write_other_count=$(benchmark_summary_field "$summary" write_other)
			nack_count=$(benchmark_summary_field "$summary" nack)
			duplicate_count=$(benchmark_summary_field "$summary" duplicate)
			unknown_count=$(benchmark_summary_field "$summary" unknown)
			max_inflight=$(benchmark_summary_field "$summary" max_inflight)
			[[ $write_count =~ ^[0-9]+$ && $pong_count =~ ^[0-9]+$ &&
			   $ack_count =~ ^[0-9]+$ && $lost_count =~ ^[0-9]+$ &&
			   $timeout_count =~ ^[0-9]+$ && $enospc_count =~ ^[0-9]+$ &&
			   $eagain_count =~ ^[0-9]+$ && $write_other_count =~ ^[0-9]+$ &&
			   $nack_count =~ ^[0-9]+$ && $duplicate_count =~ ^[0-9]+$ &&
			   $unknown_count =~ ^[0-9]+$ && $max_inflight =~ ^[0-9]+$ ]] ||
				remote_fail "cannot parse window $window p$priority summary"
			((write_count > 0 && pong_count == write_count &&
			  lost_count == 0 && timeout_count == 0 && enospc_count == 0 &&
			  eagain_count == 0 && write_other_count == 0 && nack_count == 0 &&
			  duplicate_count == 0 && unknown_count == 0 &&
			  max_inflight <= window)) ||
				remote_fail "window $window p$priority lost or malformed a frame"
			if ((priority <= 1)); then
				((ack_count == write_count)) ||
					remote_fail "reliable window $window p$priority ACK count $ack_count != $write_count"
			else
				((ack_count == 0)) ||
					remote_fail "unreliable window $window p$priority unexpectedly returned ACKs"
			fi
			write_counts[$priority]=$((write_counts[$priority] + write_count))
			pong_counts[$priority]=$((pong_counts[$priority] + pong_count))
			ack_counts[$priority]=$((ack_counts[$priority] + ack_count))
			stage_write=$((stage_write + write_count))
		done
		FOUR_PRIORITY_BENCH_PIDS=()

		wait_worker_idle_at "$((stage_before_msg + stage_write))"
		stage_after_msg=$(mailmsg_worker_field msg)
		stage_after_errors=$(stats_protocol_error_total)
		pending=$(mailmsg_worker_field pending)
		((stage_after_msg == stage_before_msg + stage_write)) ||
			remote_fail "window $window worker msg=$stage_after_msg, expected $((stage_before_msg + stage_write))"
		((stage_after_errors == stage_before_errors)) ||
			remote_fail "window $window protocol errors changed: $stage_before_errors -> $stage_after_errors"
		[[ $pending == 0 || $pending == 0x0 ]] ||
			remote_fail "window $window worker pending bitmap is not empty: $pending"
		for priority in 0 1 2 3; do
			after_full[$priority]=$(priority_full_count "$priority")
			[[ ${after_full[$priority]} =~ ^[0-9]+$ ]] ||
				remote_fail "cannot parse window $window p$priority full counter"
			((after_full[$priority] == stage_before_full[$priority])) ||
				remote_fail "window $window p$priority full counter changed"
		done
		[[ $(mailmsg_state) == active ]] ||
			remote_fail "MailMsg session ended after window $window"
		grep -q 'state=on (0)' "$AMP/affinity_state" ||
			remote_fail "CPU3 is no longer ON after window $window"
		grep -Eq 'a2b_now=m0:0x0( |$)' <<<"$(mailmsg_status)" ||
			remote_fail "window $window left an A2B doorbell pending"
		assert_all_depths_zero
		total_write=$((total_write + stage_write))
		printf 'window_stage=%d write_ok=%d p0=%d/%d/%d p1=%d/%d/%d p2=%d/%d/%d p3=%d/%d/%d\n' \
			"$window" "$stage_write" \
			"${write_counts[0]}" "${ack_counts[0]}" "${pong_counts[0]}" \
			"${write_counts[1]}" "${ack_counts[1]}" "${pong_counts[1]}" \
			"${write_counts[2]}" "${ack_counts[2]}" "${pong_counts[2]}" \
			"${write_counts[3]}" "${ack_counts[3]}" "${pong_counts[3]}"
	done

	if wait "$llm_pid"; then
		llm_rc=0
	else
		llm_rc=$?
	fi
	FOUR_PRIORITY_LLM_PID=
	FOUR_PRIORITY_BENCH_PIDS=()
	trap - EXIT
	((llm_rc == 0)) || {
		cat "$llm_log"
		remote_fail "RKLLM four-priority test exited with status $llm_rc"
	}
	grep -Fq 'rkllm-runtime version: 1.3.0, rknpu driver version: 0.9.8' \
		"$llm_log" || remote_fail "unexpected RKLLM/RKNPU version"
	grep -Fq 'Enabled cpus: [3, 4, 5, 6]' "$llm_log" ||
		remote_fail "RKLLM did not use the expected Linux CPU mask"
	grep -Fq 'Enabled cpus num: 4' "$llm_log" ||
		remote_fail "RKLLM enabled CPU count is not four"
	if grep -Eq 'matmul\(w8a8\) run failed|rkllm init failed|Mismatch between enabled CPUs' \
		"$llm_log"; then
		remote_fail "RKLLM four-priority log contains a known failure"
	fi
	local generate_count robot_count
	generate_count=$(grep -Ec '^I rkllm:  Generate[[:space:]]' "$llm_log" || true)
	robot_count=$(grep -c 'robot:' "$llm_log" || true)
	((generate_count == rounds)) || remote_fail \
		"completed $generate_count RKLLM generations; expected $rounds"
	((robot_count >= rounds)) || remote_fail \
		"observed $robot_count robot prompts; expected at least $rounds"

	note "post-window four-priority regression"
	run_four_priorities
	expected_msg=$((before_msg + total_write + 4))
	wait_worker_idle_at "$expected_msg"
	after_msg=$(mailmsg_worker_field msg)
	after_errors=$(stats_protocol_error_total)
	pending=$(mailmsg_worker_field pending)
	((after_msg == expected_msg)) || remote_fail \
		"worker msg=$after_msg, expected $expected_msg after post checks"
	((after_errors == before_errors)) || remote_fail \
		"protocol error counters changed: $before_errors -> $after_errors"
	[[ $pending == 0 || $pending == 0x0 ]] ||
		remote_fail "worker pending bitmap is not empty: $pending"
	[[ $(mailmsg_state) == active ]] || remote_fail "MailMsg session is no longer active"
	grep -q 'state=on (0)' "$AMP/affinity_state" ||
		remote_fail "CPU3 is no longer ON"
	grep -Eq 'a2b_now=m0:0x0( |$)' <<<"$(mailmsg_status)" ||
		remote_fail "an A2B doorbell remains pending"
	assert_all_depths_zero

	printf 'rkllm_generations=%d simultaneous_windows=%s total_write_ok=%d p0_write_ok=%d p0_ack=%d p0_pong=%d p1_write_ok=%d p1_ack=%d p1_pong=%d p2_write_ok=%d p2_ack=%d p2_pong=%d p3_write_ok=%d p3_ack=%d p3_pong=%d\n' \
		"$generate_count" "$window_list" "$total_write" \
		"${write_counts[0]}" "${ack_counts[0]}" "${pong_counts[0]}" \
		"${write_counts[1]}" "${ack_counts[1]}" "${pong_counts[1]}" \
		"${write_counts[2]}" "${ack_counts[2]}" "${pong_counts[2]}" \
		"${write_counts[3]}" "${ack_counts[3]}" "${pong_counts[3]}"
	free -h
	remote_snapshot
	pass "four priorities ran simultaneously at windows=$window_list during RKLLM generation"
}

remote_main()
{
	local name path timestamp

	[[ $(id -u) -eq 0 ]] || { fail "remote tests must run as root"; exit 1; }
	[[ -n $PROFILE ]] || { fail "--profile is required in remote mode"; exit 2; }
	command -v sha256sum >/dev/null || { fail "sha256sum is required"; exit 1; }
	mkdir -p -- "$REMOTE_ROOT/reports"
	timestamp=$(date '+%Y%m%d-%H%M%S')
	REPORT=$REMOTE_ROOT/reports/$PROFILE-$timestamp.log
	exec > >(tee -a "$REPORT") 2>&1

	note "MailMsg V1 revision 7 board profile: $PROFILE"
	printf 'report=%s\n' "$REPORT"
	for name in "${ARTIFACT_NAMES[@]}"; do
		path=$REMOTE_ROOT/$name
		[[ -f $path ]] || remote_fail "missing uploaded artifact: $path"
		verify_file_hash "$path" || remote_fail "artifact hash mismatch: $path"
	done
	pass "R1 artifact hashes"

	discover_amp
	CLIENT=$REMOTE_ROOT/mailmsg-user-client-aarch64
	EXCLUSIVE_TEST=$REMOTE_ROOT/mailmsg-exclusive-reader-test-aarch64
	WINDOW_BENCH=$REMOTE_ROOT/mailmsg-window-bench-aarch64
	for path in status mailmsg_stats affinity_state image start rearm \
		mailmsg_ping mailmsg_stop mailmsg_crc_inject mailmsg_notify_inject \
		mailmsg_queue_push mailmsg_response doorbell; do
		[[ -e $AMP/$path ]] || remote_fail "missing driver interface: $path"
	done
	for path in /dev/mailmsg-p0 /dev/mailmsg-p1 /dev/mailmsg-p2 /dev/mailmsg-p3; do
		[[ -e $path ]] || remote_fail "missing character device: $path"
	done
	[[ $(uname -r) == 5.10.252 ]] || remote_fail "unexpected kernel: $(uname -r)"
	[[ $(nproc) == 7 ]] || remote_fail "expected seven Linux CPUs, got $(nproc)"
	[[ -d /proc/device-tree/reserved-memory/zephyr@50000000 ]] ||
		remote_fail "Zephyr reserved-memory node is absent"
	grep -q 'stale=' "$AMP/mailmsg_stats" ||
		remote_fail "running driver lacks the MailMsg V6 statistics ABI"
	grep -q 'session=' "$AMP/status" ||
		remote_fail "running driver lacks the MailMsg V6 session ABI"
	remote_snapshot
	pass "R7 kernel/DT/sysfs preflight"

	case $PROFILE in
	controlled) run_controlled_profile ;;
	stop-refused) run_stop_refused_profile ;;
	start-timeout) run_start_timeout_profile ;;
	stop-timeout) run_stop_timeout_profile ;;
	llm-coexistence) run_llm_coexistence_profile ;;
	llm-soak) run_llm_soak_profile ;;
	llm-p2-window) run_llm_p2_window_profile ;;
	llm-four-priority|llm-four-priority-sweep) run_llm_four_priority_profile ;;
	esac

	note "profile result: PASS"
	printf 'Full report saved at %s\n' "$REPORT"
}

if [[ $MODE == remote ]]; then
	remote_main
else
	host_main
fi
