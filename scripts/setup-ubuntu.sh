#!/usr/bin/env bash
# Installs everything needed to build, test and lint lockstep-exchange on a
# fresh Ubuntu 24.04 (native, WSL2 or CI runner). Idempotent; re-run at will.
#
#   scripts/setup-ubuntu.sh            # full developer setup
#   scripts/setup-ubuntu.sh --ci       # skip interactive-only extras
#   scripts/setup-ubuntu.sh --no-rust  # C++ toolchain only (e.g. C++ Docker image)
#
# Decision record: docs/adr/0007-dependency-management-system-packages.md
set -euo pipefail

BUF_VERSION="1.47.2"

WITH_RUST=1
CI_MODE=0
for arg in "$@"; do
  case "$arg" in
    --ci) CI_MODE=1 ;;
    --no-rust) WITH_RUST=0 ;;
    -h|--help) sed -n '2,9p' "$0"; exit 0 ;;
    *) echo "unknown argument: $arg" >&2; exit 2 ;;
  esac
done

if [[ "$(. /etc/os-release && echo "${VERSION_ID}")" != "24.04" ]]; then
  echo "warning: this script targets Ubuntu 24.04; continuing anyway" >&2
fi

SUDO=""
if [[ "$(id -u)" -ne 0 ]]; then SUDO="sudo"; fi

echo "==> apt packages"
export DEBIAN_FRONTEND=noninteractive
$SUDO apt-get update -qq
$SUDO apt-get install -y -qq --no-install-recommends \
  build-essential gcc-14 g++-14 \
  clang-19 clang-tidy-19 clang-format-19 lld-19 llvm-19 libclang-rt-19-dev \
  cmake ninja-build ccache pkg-config git curl ca-certificates unzip jq python3 \
  libgrpc++-dev libgrpc-dev protobuf-compiler-grpc libprotobuf-dev protobuf-compiler \
  libgtest-dev libgmock-dev libbenchmark-dev

echo "==> sanitizer-friendly ASLR"
# Kernels >= 6.5 default to vm.mmap_rnd_bits=32, which makes TSan (and at times
# ASan) abort with "unexpected memory mapping". 28 bits is the upstream-recommended
# value until the LLVM runtime handles the larger range.
if [[ -w /proc/sys/vm/mmap_rnd_bits || -n "$SUDO" ]]; then
  $SUDO sysctl -q -w vm.mmap_rnd_bits=28 || echo "warning: could not set vm.mmap_rnd_bits" >&2
  if [[ "$CI_MODE" -eq 0 ]]; then
    echo "vm.mmap_rnd_bits=28" | $SUDO tee /etc/sysctl.d/60-sanitizers.conf >/dev/null
  fi
fi

echo "==> buf ${BUF_VERSION}"
if ! command -v buf >/dev/null || [[ "$(buf --version)" != "${BUF_VERSION}" ]]; then
  tmp="$(mktemp -d)"
  base="https://github.com/bufbuild/buf/releases/download/v${BUF_VERSION}"
  bin="buf-Linux-$(uname -m)"
  curl -fsSL -o "${tmp}/${bin}" "${base}/${bin}"
  curl -fsSL -o "${tmp}/sha256.txt" "${base}/sha256.txt"
  (cd "${tmp}" && grep " ${bin}\$" sha256.txt | sha256sum -c --quiet -)
  $SUDO install -m 0755 "${tmp}/${bin}" /usr/local/bin/buf
  rm -rf "${tmp}"
fi

if [[ "$WITH_RUST" -eq 1 ]]; then
  echo "==> rust (version pinned by rust/rust-toolchain.toml)"
  if ! command -v rustup >/dev/null && [[ ! -x "${HOME}/.cargo/bin/rustup" ]]; then
    curl -fsSL https://sh.rustup.rs | sh -s -- -y --profile minimal --default-toolchain none
  fi
  export PATH="${HOME}/.cargo/bin:${PATH}"
  repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
  # `rustup show` installs the toolchain + components listed in rust-toolchain.toml.
  (cd "${repo_root}/rust" && rustup show active-toolchain >/dev/null 2>&1 || rustup toolchain install)
  (cd "${repo_root}/rust" && rustup show >/dev/null)
fi

echo "==> versions"
g++-14 --version | head -1
clang++-19 --version | head -1
cmake --version | head -1
protoc --version
buf --version
if [[ "$WITH_RUST" -eq 1 ]]; then (cd "$(dirname "$0")/../rust" && cargo --version); fi
echo "setup complete"
