#!/usr/bin/env bash
set -euo pipefail
CMP_VENDOR="0x10de"; CMP_DEVICE="0x1e09"
log() { echo "cmp50hx-gen2-rescan: $*"; }
get_gpus() {
  for dev in /sys/bus/pci/devices/*; do
    [[ -f "$dev/vendor" && -f "$dev/device" ]] || continue
    [[ "$(<"$dev/vendor")" == "$CMP_VENDOR" && "$(<"$dev/device")" == "$CMP_DEVICE" ]] && basename "$dev"
  done | sort
}
mapfile -t GPUS < <(get_gpus)
[[ ${#GPUS[@]} -gt 0 ]] || { log "no CMP 50HX"; exit 1; }
log "pre-driver rescan for ${#GPUS[@]} CMP: ${GPUS[*]}"
for gpu in "${GPUS[@]}"; do echo 1 > "/sys/bus/pci/devices/$gpu/remove"; done
echo 1 > /sys/bus/pci/rescan
sleep 3
for gpu in "${GPUS[@]}"; do
  upstream=$(basename "$(dirname "$(readlink -f "/sys/bus/pci/devices/$gpu")")")
  setpci -s "$gpu" CAP_EXP+30.W=0002:000f
  setpci -s "$upstream" CAP_EXP+30.W=0002:000f
  ctl=$(setpci -s "$upstream" CAP_EXP+10.W)
  setpci -s "$upstream" CAP_EXP+10.W="$(printf '%04x' $(( 0x${ctl} | 0x20 )))"
  for _ in $(seq 1 50); do
    speed=$(cat "/sys/bus/pci/devices/$gpu/current_link_speed" 2>/dev/null || :)
    [[ "$speed" == *"5.0 GT/s"* ]] && break
    sleep 0.1
  done
  log "$gpu: ${speed:-unknown} x$(cat "/sys/bus/pci/devices/$gpu/current_link_width")"
done
modprobe nvidia
modprobe nvidia_uvm
modprobe nvidia_modeset
modprobe nvidia_drm
log "driver loaded after rescan"
