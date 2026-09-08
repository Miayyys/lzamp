#!/usr/bin/env bash
# SPDX-License-Identifier: MIT

set -euo pipefail

if (( $# != 3 )); then
	printf 'usage: %s INPUT OUTPUT SIZE\n' "$0" >&2
	exit 2
fi

input=$1
output=$2
target_size=$3

if [[ ! -f $input || ! $target_size =~ ^[0-9]+$ ]]; then
	printf 'error: invalid input or target size\n' >&2
	exit 2
fi

input_size=$(stat -c '%s' "$input")
if (( input_size > target_size )); then
	printf 'error: Zephyr image is %s bytes; slot is only %s bytes\n' \
		"$input_size" "$target_size" >&2
	exit 1
fi

install -m 0644 "$input" "$output"
truncate -s "$target_size" "$output"

printf 'Zephyr deploy image: %s/%s bytes -> %s\n' \
	"$input_size" "$target_size" "$output"
