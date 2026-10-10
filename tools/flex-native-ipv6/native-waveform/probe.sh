#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 J. J. Boyd
case "$0" in
  */*) probe_dir=${0%/*} ;;
  *) probe_dir=. ;;
esac
case "$(uname -m)" in
  aarch64|arm64) exec "$probe_dir/probe-arm64" ;;
  armv7l|armv7|arm) exec "$probe_dir/probe-armv7" ;;
  *) echo "native probe architecture unsupported" ;;
esac
