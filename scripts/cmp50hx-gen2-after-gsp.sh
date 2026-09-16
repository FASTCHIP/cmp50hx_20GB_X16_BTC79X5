#!/usr/bin/env bash
# Offline artifact. Do not install or execute on ai100gb without approval.
#
# Safe ordering: wait until the NVIDIA driver completed its first GSP init,
# then perform only bounded PCIe target-speed/retrain writes.  This script
# intentionally never removes/rescans a PCI device, reloads NVIDIA modules,
# changes IOMMU/ACS, or touches GPU-private BAR0 registers.
set -Eeuo pipefail

PATH=/usr/sbin:/usr/bin:/sbin:/bin
readonly EXPECTED_VENDOR_DEVICE='10de:'
readonly -a GPU_BDFS=("0000:01:00.0" "0000:04:00.0")
readonly -a ROOT_PORT_BDFS=("0000:00:01.0" "0000:00:03.0")
readonly MAX_RETRAIN_ATTEMPTS="${CMP50_GEN2_MAX_ATTEMPTS:-3}"
readonly READY_TIMEOUT_SEC="${CMP50_GEN2_READY_TIMEOUT_SEC:-90}"
readonly POLL_INTERVAL_SEC=1

log() { logger -t cmp50hx-gen2-after-gsp -- "$*"; printf '%s\n' "$*"; }
die() { log "ERROR: $*"; exit 1; }

need_cmd() { command -v "$1" >/dev/null 2>&1 || die "required command unavailable: $1"; }
need_root() { [[ ${EUID} -eq 0 ]] || die "must run as root"; }

require_apply_switch() {
    [[ ${CMP50_GEN2_APPLY:-0} == 1 ]] || die "refusing writes: set CMP50_GEN2_APPLY=1 explicitly"
}

validate_static_topology() {
    [[ ${#GPU_BDFS[@]} -eq ${#ROOT_PORT_BDFS[@]} ]] || die "internal BDF map mismatch"
    local bdf id
    for bdf in "${GPU_BDFS[@]}"; do
        [[ -r "/sys/bus/pci/devices/${bdf}/vendor" ]] || die "GPU BDF absent: ${bdf}"
        id="$(<"/sys/bus/pci/devices/${bdf}/vendor")"
        [[ ${id} == 0x10de ]] || die "unexpected GPU vendor at ${bdf}: ${id}"
    done
    for bdf in "${ROOT_PORT_BDFS[@]}"; do
        [[ -r "/sys/bus/pci/devices/${bdf}/vendor" ]] || die "root-port BDF absent: ${bdf}"
    done
}

wait_for_first_gsp_init() {
    local deadline=$((SECONDS + READY_TIMEOUT_SEC)) count
    while (( SECONDS < deadline )); do
        # nvidia-smi is the readiness probe; it does not trigger any PCI remove/rescan.
        if nvidia-smi -L >/dev/null 2>&1; then
            count="$(nvidia-smi -L 2>/dev/null | grep -c '^GPU ' || true)"
            if [[ ${count} == 2 ]]; then
                log "NVIDIA ready: two GPUs enumerated after first GSP initialization"
                return 0
            fi
        fi
        sleep "${POLL_INTERVAL_SEC}"
    done
    die "NVIDIA/GSP readiness timeout (${READY_TIMEOUT_SEC}s)"
}

ensure_no_compute_workload() {
    local apps
    apps="$(nvidia-smi --query-compute-apps=pid --format=csv,noheader,nounits 2>/dev/null || true)"
    [[ -z ${apps//[[:space:]]/} ]] || die "compute workload already active; do not retrain an in-use link: ${apps}"
    for unit in llama-qwen.service llama-ornith.service; do
        if systemctl is-active --quiet "${unit}"; then
            die "${unit} is active; unit ordering is broken, refusing link retrain"
        fi
    done
}

read_word() {
    local bdf=$1 reg=$2 value
    value="$(setpci -s "${bdf}" "${reg}")"
    [[ ${value} =~ ^[0-9A-Fa-f]{4}$ ]] || die "bad PCI word ${bdf} ${reg}: ${value}"
    printf '%s\n' "${value}"
}

set_target_gen2_and_retrain() {
    local bdf=$1 ctl2 ctl2_new ctl ctl_new
    # PCIe capability offsets: Link Control=0x10, Link Control 2=0x30.
    # Preserve all fields except Target Link Speed (low nibble), then set
    # Retrain Link (bit 5). Both writes are config-space only.
    ctl2="$(read_word "${bdf}" 'CAP_EXP+0x30.w')"
    ctl2_new="$(printf '%04x' $(( (16#${ctl2} & ~0x000f) | 0x0002 )))"
    ctl="$(read_word "${bdf}" 'CAP_EXP+0x10.w')"
    ctl_new="$(printf '%04x' $(( 16#${ctl} | 0x0020 )) )"
    log "${bdf}: LnkCtl2 ${ctl2}->${ctl2_new}; LnkCtl ${ctl}->${ctl_new} (Gen2 retrain)"
    setpci -s "${bdf}" "CAP_EXP+0x30.w=${ctl2_new}"
    setpci -s "${bdf}" "CAP_EXP+0x10.w=${ctl_new}"
}

link_is_gen2_x8() {
    local bdf=$1 line
    line="$(lspci -D -vv -s "${bdf}" 2>/dev/null | grep -m1 'LnkSta:' || true)"
    [[ ${line} == *'Speed 5GT/s'* && ${line} == *'Width x8'* ]]
}

log_link_state() {
    local bdf=$1 line
    line="$(lspci -D -vv -s "${bdf}" 2>/dev/null | grep -m1 'LnkSta:' || true)"
    log "${bdf}: ${line:-LnkSta unavailable}"
}

main() {
    need_root
    need_cmd nvidia-smi
    need_cmd setpci
    need_cmd lspci
    need_cmd systemctl
    [[ ${MAX_RETRAIN_ATTEMPTS} =~ ^[1-9][0-9]*$ ]] || die "invalid CMP50_GEN2_MAX_ATTEMPTS=${MAX_RETRAIN_ATTEMPTS}"
    validate_static_topology
    wait_for_first_gsp_init
    ensure_no_compute_workload
    require_apply_switch

    local attempt idx gpu rp all_ok
    for ((attempt = 1; attempt <= MAX_RETRAIN_ATTEMPTS; attempt++)); do
        log "retrain attempt ${attempt}/${MAX_RETRAIN_ATTEMPTS}"
        for ((idx = 0; idx < ${#GPU_BDFS[@]}; idx++)); do
            gpu="${GPU_BDFS[idx]}"; rp="${ROOT_PORT_BDFS[idx]}"
            # Endpoint target first, root port target/retrain second. No device reset.
            set_target_gen2_and_retrain "${gpu}"
            set_target_gen2_and_retrain "${rp}"
        done
        sleep 2
        all_ok=1
        for gpu in "${GPU_BDFS[@]}"; do
            log_link_state "${gpu}"
            link_is_gen2_x8 "${gpu}" || all_ok=0
        done
        (( all_ok )) && { log "SUCCESS: both GPUs report Gen2 (5GT/s) x8"; exit 0; }
    done
    die "Gen2 x8 not reached after ${MAX_RETRAIN_ATTEMPTS} bounded attempts; no remove/rescan fallback is permitted"
}

main "$@"
