#!/usr/bin/env bash
# Copyright (C) 2026 BlackBearReloaded
# SPDX-License-Identifier: GPL-3.0-or-later
# Compatibility entry point for the relevant standalone C/C++ application suites.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
exec make -C "$root" test-unit "$@"
