#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later
# run_all.sh — run every integration suite; fail if any fails.
set -u
here="$(dirname "$0")"
rc=0

bash "$here/run.sh"     || rc=1
echo
bash "$here/run_web.sh" || rc=1

echo
if [ "$rc" -eq 0 ]; then echo "ALL INTEGRATION SUITES PASSED"; else echo "SOME SUITES FAILED"; fi
exit $rc
