#!/usr/bin/env bash
  # Flash an ELF to the MSPM0G3507 over the LaunchPad's onboard XDS110 probe.
  # Uses the .ccxml copy in this repo, NOT the one inside the CCS install, so
  # this works on a machine that has never had CCS.
  set -euo pipefail

  ELF="${1:?usage: tools/flash.sh <firmware.elf>}"
  HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

  DSLITE="${DSLITE:-/Applications/ti/ccs2040/ccs/ccs_base/DebugServer/bin/DSLite}"
  CCXML="$HERE/mspm0g3507.ccxml"

  [[ -x "$DSLITE" ]] || { echo "DSLite not found at $DSLITE (override with DSLITE=...)" >&2; exit 1; }
  [[ -f "$ELF"    ]] || { echo "no such file: $ELF" >&2; exit 1; }

  exec "$DSLITE" flash --config="$CCXML" "$ELF"