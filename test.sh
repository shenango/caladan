#!/bin/bash

set -euo pipefail
set -x

REPO_ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
cd "${REPO_ROOT}"

IOK_LOG="/tmp/iokernel_${USER}.log"

ensure_cargo() {
  if command -v cargo >/dev/null 2>&1; then
    CARGO_BIN=$(command -v cargo)
    return
  fi

  export CARGO_HOME="${REPO_ROOT}/.cargo-local"
  export RUSTUP_HOME="${REPO_ROOT}/.rustup-local"
  export RUSTUP_INIT_SKIP_PATH_CHECK=yes
  RUSTUP_INIT="${CARGO_HOME}/rustup-init"

  mkdir -p "${CARGO_HOME}" "${RUSTUP_HOME}"

  if [[ ! -x "${RUSTUP_INIT}" ]]; then
    curl https://sh.rustup.rs -sSf -o "${RUSTUP_INIT}"
    chmod +x "${RUSTUP_INIT}"
  fi

  if [[ ! -x "${CARGO_HOME}/bin/cargo" ]]; then
    "${RUSTUP_INIT}" -y --default-toolchain=nightly --profile=minimal --no-modify-path
  fi

  export PATH="${CARGO_HOME}/bin:${PATH}"
  CARGO_BIN="${CARGO_HOME}/bin/cargo"
}

cleanup() {
  set +e
  if [[ -n "${LG_SRV_PID:-}" ]]; then
    kill "${LG_SRV_PID}" >/dev/null 2>&1 || true
    wait "${LG_SRV_PID}" >/dev/null 2>&1 || true
  fi
  if [[ -n "${NP_PID:-}" ]]; then
    kill "${NP_PID}" >/dev/null 2>&1 || true
    wait "${NP_PID}" >/dev/null 2>&1 || true
  fi
  if [[ -n "${IOK_PID:-}" ]]; then
    sudo kill "${IOK_PID}" >/dev/null 2>&1 || true
    wait "${IOK_PID}" >/dev/null 2>&1 || true
  fi
}
trap cleanup EXIT

(sudo pkill iokerneld && sleep 2) || true
sudo pkill -9 netperf || true
sudo scripts/setup_machine.sh "${NOUINTR:-}"
sudo ./iokerneld ias nobw noht no_hw_qdel numanode -1 -- --allow 00:00.0 --vdev=net_tap0 > "${IOK_LOG}" 2>&1 &
IOK_PID=$!

while ! grep -q 'running dataplan' "${IOK_LOG}"; do
  sleep 0.3
  # make sure it is still alive
  pgrep iokerneld >/dev/null || exit 1
  cat "${IOK_LOG}"
done

CORES=$(getconf _NPROCESSORS_ONLN)

gen_configs() {
  exec {test_fd}<<EOF
host_addr 192.168.1.5
host_netmask 255.255.255.0
host_gateway 192.168.1.1
runtime_kthreads $((CORES-2))
runtime_guaranteed_kthreads 0
runtime_priority lc
EOF

  exec {test_fd2}<<EOF
host_addr 192.168.1.6
host_netmask 255.255.255.0
host_gateway 192.168.1.1
runtime_kthreads $((CORES-2))
runtime_guaranteed_kthreads 0
runtime_priority lc
EOF
}

for test in $(find tests -name 'test_*' -executable | grep -v storage); do
  gen_configs
  "${test}" /proc/self/fd/"${test_fd}" /proc/self/fd/"${test_fd2}"
done

make -C bindings/cc/ -j librt++.a
make -C apps/bench/ netperf -j

gen_configs
apps/bench/netperf /proc/self/fd/"${test_fd}" server &
NP_PID=$!
sleep 1

apps/bench/netperf /proc/self/fd/"${test_fd2}" tcprr 192.168.1.5 10 10000 4096

gen_configs
apps/bench/netperf /proc/self/fd/"${test_fd2}" tcpstream 192.168.1.5 10 10000 4096

kill $NP_PID || true
wait $NP_PID || true
NP_PID=

pushd apps/loadgen
ensure_cargo
"${CARGO_BIN}" build --release
popd

stop_loadgen_server() {
  if [[ -n "${LG_SRV_PID:-}" ]]; then
    kill "${LG_SRV_PID}" >/dev/null 2>&1 || true
    wait "${LG_SRV_PID}" >/dev/null 2>&1 || true
    LG_SRV_PID=
  fi
}

LOADGEN_RUNTIME_S="${LOADGEN_RUNTIME_S:-1}"
LOADGEN_SAMPLES="${LOADGEN_SAMPLES:-2}"
LOADGEN_RAMPUP_S="${LOADGEN_RAMPUP_S:-0}"
LOADGEN_MPPS="${LOADGEN_MPPS:-0.001}"
LOADGEN_CONNS="${LOADGEN_CONNS:-1}"
LOADGEN_TIMEOUT_S="${LOADGEN_TIMEOUT_S:-30}"

for LOADGEN_TRANSPORT in tcp udp; do
  gen_configs
  apps/loadgen/target/release/loadgen 192.168.1.5:5000 \
    --config /proc/self/fd/"${test_fd}" \
    --mode spawner-server \
    --transport "${LOADGEN_TRANSPORT}" &
  LG_SRV_PID=$!
  sleep 1

  if ! timeout "${LOADGEN_TIMEOUT_S}"s \
    apps/loadgen/target/release/loadgen 192.168.1.5:5000 \
    --config /proc/self/fd/"${test_fd2}" \
    --mode runtime-client \
    --runtime "${LOADGEN_RUNTIME_S}" \
    --samples "${LOADGEN_SAMPLES}" \
    --rampup "${LOADGEN_RAMPUP_S}" \
    --mpps "${LOADGEN_MPPS}" \
    --conns "${LOADGEN_CONNS}" \
    --transport "${LOADGEN_TRANSPORT}" \
    --output normal; then
    echo "loadgen runtime-client failed or timed out after ${LOADGEN_TIMEOUT_S}s (${LOADGEN_TRANSPORT})" >&2
    stop_loadgen_server
    exit 1
  fi

  stop_loadgen_server
done
