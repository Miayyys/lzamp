#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Batch test for the Zephyr-owned RK3588 GPIO3 lines.
#
# Run on the board and call the allowlisted V7 Agent directly:
#   gpio3-loopback-test.sh --read-all
#
# A physical loopback needs an explicit opt-in and a 3.3 V-safe,
# high-impedance connection, preferably with a 1--10 kOhm series resistor:
#   gpio3-loopback-test.sh --allow-output --loopback 25:28
#   gpio3-loopback-test.sh --allow-output --all-pairs --pause-between-pairs
#
# The script cannot detect how a header is wired.  --pause-between-pairs lets
# one jumper be moved between pairs.  Cleanup always attempts to drive any
# output low and restore all three lines to inputs.

set -Eeuo pipefail
IFS=$'\n\t'

readonly SCRIPT_NAME=${0##*/}
readonly AMP=${AMP_DEVICE:-/sys/bus/platform/devices/amp-cpu-on-heartbeat}
readonly DEFAULT_AGENT=/userdata/lzamp/agent/mailmsg_agent-v7.py
readonly GPIO_LINES=(25 28 29)

PYTHON=${PYTHON:-/usr/bin/python3}
AGENT=${AGENT:-$DEFAULT_AGENT}
DEVICE_ROOT=${DEVICE_ROOT:-/dev}
TIMEOUT_MS=${TIMEOUT_MS:-2000}
SETTLE_MS=${SETTLE_MS:-10}

MODE=read-all
ALLOW_OUTPUT=0
PAUSE_BETWEEN_PAIRS=0
PAIRS=()

usage()
{
	cat <<EOF
Usage:
  $SCRIPT_NAME [--read-all]
  $SCRIPT_NAME --allow-output --loopback OUT:IN [--loopback OUT:IN ...]
  $SCRIPT_NAME --allow-output --all-pairs [--pause-between-pairs]

Allowed GPIO3 lines: 25, 28, 29.

Options:
  --read-all                 Configure all three lines as input and read them.
  --loopback OUT:IN          Test 0 -> 1 -> 0 on OUT and read IN; repeatable.
  --all-pairs                Use 25:28, 28:29 and 29:25.
  --allow-output             Required before any operation that drives a line.
  --pause-between-pairs      Wait for Enter before each pair after the first,
                             allowing one physical jumper to be moved.
  --agent PATH               Agent script (default: $DEFAULT_AGENT).
  --python PATH              Python interpreter (default: $PYTHON).
  --device-root PATH         Device directory (default: $DEVICE_ROOT).
  --timeout-ms N             MailMsg operation timeout (default: $TIMEOUT_MS).
  --settle-ms N              Delay after writes before reads (default: $SETTLE_MS).

Loopback requires confirmed 3.3 V logic, a high-impedance input and safe
series protection.  It does not prove GPIO electrical limits or pin muxing
outside lines 25, 28 and 29.
EOF
}

fail()
{
	printf 'FAIL: %s\n' "$*" >&2
	return 1
}

is_owned_line()
{
	case $1 in
	25|28|29) return 0 ;;
	*) return 1 ;;
	esac
}

parse_pair()
{
	local spec=$1 out in
	if [[ ! $spec =~ ^([0-9]+):([0-9]+)$ ]]; then
		fail "loopback pair must be OUT:IN, got: $spec"
		return 1
	fi
	out=${BASH_REMATCH[1]}
	in=${BASH_REMATCH[2]}
	is_owned_line "$out" || { fail "output line is not allowlisted: $out"; return 1; }
	is_owned_line "$in" || { fail "input line is not allowlisted: $in"; return 1; }
	(( out != in )) || { fail "output and input must be different: $spec"; return 1; }
	PAIRS+=("$out:$in")
}

json_get()
{
	local path=$1 json=$2
	"$PYTHON" -c '
import json
import sys

value = json.load(sys.stdin)
for part in sys.argv[1].split("."):
    value = value[part]
if isinstance(value, bool):
    print("true" if value else "false")
elif value is None:
    print("null")
else:
    print(value)
' "$path" <<<"$json"
}

sleep_settle()
{
	if (( SETTLE_MS > 0 )); then
		"$PYTHON" -c 'import sys, time; time.sleep(float(sys.argv[1]) / 1000.0)' \
			"$SETTLE_MS"
	fi
}

TMP_DIR=
cleanup_done=0
config_touched=0
LAST_READ_VALUE=
declare -A output_touched=()

agent_call()
{
	local decision=$1 output error_file
	error_file=$TMP_DIR/agent.stderr
	: >"$error_file"
	if ! output=$("$PYTHON" "$AGENT" --decision "$decision" \
		--device-root "$DEVICE_ROOT" --timeout-ms "$TIMEOUT_MS" \
		2>"$error_file"); then
		cat "$error_file" >&2
		[[ -n $output ]] && printf '%s\n' "$output" >&2
		return 1
	fi
	printf '%s\n' "$output"
}

run_config()
{
	local line=$1 direction=$2 value=${3:-0} decision result ok tool
	decision=$(printf '{"name":"zephyr_gpio_config","arguments":{"line":%d,"direction":"%s","value":%d}}' \
		"$line" "$direction" "$value")
	result=$(agent_call "$decision") || return 1
	ok=$(json_get ok "$result") || return 1
	tool=$(json_get tool "$result") || return 1
	[[ $ok == true && $tool == zephyr_gpio_config ]] || {
		printf 'unexpected GPIO config response: %s\n' "$result" >&2
		return 1
	}
	config_touched=1
	printf 'gpio-config line=%d direction=%s\n' "$line" "$direction"
}

run_write()
{
	local line=$1 value=$2 decision result ok tool
	decision=$(printf '{"name":"zephyr_gpio_write","arguments":{"line":%d,"value":%d}}' \
		"$line" "$value")
	result=$(agent_call "$decision") || return 1
	ok=$(json_get ok "$result") || return 1
	tool=$(json_get tool "$result") || return 1
	[[ $ok == true && $tool == zephyr_gpio_write ]] || {
		printf 'unexpected GPIO write response: %s\n' "$result" >&2
		return 1
	}
	output_touched[$line]=1
	printf 'gpio-write line=%d value=%d\n' "$line" "$value"
}

run_read()
{
	local line=$1 decision result ok tool value
	decision=$(printf '{"name":"zephyr_gpio_read","arguments":{"line":%d}}' "$line")
	result=$(agent_call "$decision") || return 1
	ok=$(json_get ok "$result") || return 1
	tool=$(json_get tool "$result") || return 1
	value=$(json_get result.value "$result") || return 1
	[[ $ok == true && $tool == zephyr_gpio_read && $value =~ ^[01]$ ]] || {
		printf 'unexpected GPIO read response: %s\n' "$result" >&2
		return 1
	}
	LAST_READ_VALUE=$value
	printf 'gpio-read line=%d value=%s\n' "$line" "$value"
}

restore_gpio()
{
	local line rc=0
	set +e
	for line in "${!output_touched[@]}"; do
		run_write "$line" 0 >/dev/null 2>&1 || rc=1
	done
	if (( config_touched )); then
		for line in "${GPIO_LINES[@]}"; do
			run_config "$line" input 0 >/dev/null 2>&1 || rc=1
		done
	fi
	set -e
	return "$rc"
}

cleanup()
{
	local saved=$? cleanup_rc=0
	(( cleanup_done )) && return "$saved"
	cleanup_done=1
	trap - EXIT INT TERM
	if ! restore_gpio; then
		cleanup_rc=1
		printf 'WARN: GPIO cleanup did not complete for every line\n' >&2
	fi
	if [[ -n ${TMP_DIR:-} && -d $TMP_DIR ]]; then
		rm -rf -- "$TMP_DIR"
	fi
	if (( saved == 0 && cleanup_rc != 0 )); then
		saved=1
	fi
	exit "$saved"
}

while (($#)); do
	case $1 in
	--read-all)
		MODE=read-all
		shift
		;;
	--loopback)
		(($# >= 2)) || { usage >&2; exit 2; }
		MODE=loopback
		parse_pair "$2"
		shift 2
		;;
	--all-pairs)
		MODE=loopback
		PAIRS=(25:28 28:29 29:25)
		shift
		;;
	--allow-output)
		ALLOW_OUTPUT=1
		shift
		;;
	--pause-between-pairs)
		PAUSE_BETWEEN_PAIRS=1
		shift
		;;
	--agent)
		(($# >= 2)) || { usage >&2; exit 2; }
		AGENT=$2
		shift 2
		;;
	--python)
		(($# >= 2)) || { usage >&2; exit 2; }
		PYTHON=$2
		shift 2
		;;
	--device-root)
		(($# >= 2)) || { usage >&2; exit 2; }
		DEVICE_ROOT=$2
		shift 2
		;;
	--timeout-ms)
		(($# >= 2)) || { usage >&2; exit 2; }
		TIMEOUT_MS=$2
		shift 2
		;;
	--settle-ms)
		(($# >= 2)) || { usage >&2; exit 2; }
		SETTLE_MS=$2
		shift 2
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

[[ $TIMEOUT_MS =~ ^[1-9][0-9]*$ ]] || { fail "timeout must be a positive integer"; exit 2; }
[[ $SETTLE_MS =~ ^[0-9]+$ ]] || { fail "settle delay must be a non-negative integer"; exit 2; }
[[ -x $PYTHON ]] || { fail "Python interpreter is not executable: $PYTHON"; exit 2; }
[[ -r $AGENT ]] || { fail "Agent script is not readable: $AGENT"; exit 2; }
[[ -d $DEVICE_ROOT ]] || { fail "device root is not a directory: $DEVICE_ROOT"; exit 2; }
[[ -r $AMP/status && -r $AMP/affinity_state ]] || {
	fail "MailMsg CPU3 sysfs interface is missing: $AMP"
	exit 1
}

status=$(cat "$AMP/status")
grep -q 'mailmsg_state=active' <<<"$status" || {
	fail "MailMsg is not active; start the verified Zephyr image first"
	exit 1
}
grep -q 'state=on (0)' "$AMP/affinity_state" || {
	fail "CPU3 is not online; refusing GPIO RPC"
	exit 1
}

TMP_DIR=$(mktemp -d /tmp/lzamp-gpio3.XXXXXX)
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

if [[ $MODE == read-all ]]; then
	(( ${#PAIRS[@]} == 0 )) || { fail "loopback pair cannot be combined with --read-all"; exit 2; }
	printf 'mode=read-all lines=25,28,29\n'
	for line in "${GPIO_LINES[@]}"; do
		run_config "$line" input 0 || { fail "failed to configure line $line as input"; exit 1; }
		run_read "$line" || { fail "failed to read line $line"; exit 1; }
		value=$LAST_READ_VALUE
		[[ $value =~ ^[01]$ ]] || { fail "invalid value for line $line: $value"; exit 1; }
	done
	printf 'PASS: read-only GPIO3 matrix\n'
	exit 0
fi

(( ALLOW_OUTPUT )) || {
	fail "loopback mode drives GPIO; add --allow-output after checking wiring"
	exit 2
}
(( ${#PAIRS[@]} > 0 )) || { fail "loopback mode needs --loopback OUT:IN or --all-pairs"; exit 2; }

printf 'mode=loopback pairs=%s\n' "${PAIRS[*]}"
pair_index=0
for pair in "${PAIRS[@]}"; do
	if (( PAUSE_BETWEEN_PAIRS && pair_index > 0 )); then
		printf 'Move the jumper for pair %s, then press Enter to continue: ' "$pair"
		read -r _
	fi
	out=${pair%%:*}
	in=${pair##*:}
	printf 'pair=%s begin\n' "$pair"
	for line in "${GPIO_LINES[@]}"; do
		run_config "$line" input 0 || { fail "failed to prepare line $line"; exit 1; }
	done
	run_config "$out" output 0 || { fail "failed to configure output line $out"; exit 1; }
	run_write "$out" 0 || { fail "failed to write zero on line $out"; exit 1; }
	sleep_settle
	run_read "$in" || { fail "failed to read input line $in"; exit 1; }
	value=$LAST_READ_VALUE
	[[ $value == 0 ]] || { fail "pair $pair expected 0, read $value"; exit 1; }
	run_write "$out" 1 || { fail "failed to write one on line $out"; exit 1; }
	sleep_settle
	run_read "$in" || { fail "failed to read input line $in"; exit 1; }
	value=$LAST_READ_VALUE
	[[ $value == 1 ]] || { fail "pair $pair expected 1, read $value"; exit 1; }
	run_write "$out" 0 || { fail "failed to restore zero on line $out"; exit 1; }
	sleep_settle
	run_read "$in" || { fail "failed to read input line $in"; exit 1; }
	value=$LAST_READ_VALUE
	[[ $value == 0 ]] || { fail "pair $pair expected final 0, read $value"; exit 1; }
	printf 'PASS: pair=%s samples=0,1,0\n' "$pair"
	pair_index=$((pair_index + 1))
done

printf 'PASS: GPIO3 loopback matrix completed; cleanup will restore all lines to input\n'
exit 0
