#!/usr/bin/env bash
# Host-side checks for the ring-buffer logic (runs in WSL, no kernel needed):
#   1. clippy with the panic-free lint set, warnings are errors
#   2. unit tests + proptest model test
set -euo pipefail
source "$(dirname "$0")/../env/common.sh"

# Keep Cargo's build output on the Linux filesystem, not in the repo.
export CARGO_TARGET_DIR=$WORK/cargo-target
cd "$REPO/ringbuf-core"

echo "== clippy"
cargo clippy --all-targets --quiet -- -D warnings
echo "== tests"
cargo test --quiet
