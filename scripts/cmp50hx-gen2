#!/usr/bin/env bash
# CMP 50HX PCIe Gen2 auto-retrain.
#
# The patched driver fires its retrain at first device open (~8 s after boot),
# but the card only unlocks its PCIe target-speed registers a few minutes
# later; the early attempt fails and the link then sits at Gen1 x4 forever,
# because nothing ever asks again. This service waits for the card's own
# unlock (GPU LNKCTL2.TLS reads 2), then asks the upstream port to retrain
# once. Firing Retrain Link is safe under load and harmless to repeat.
set -u

TIMEOUT_MIN=20      # give up after this many minutes
POLL_SEC=15         # how often to re-check for the unlock
RETRAIN_WAIT_SEC=3  # how long to wait for the link to come back up

log() { echo "cmp50hx-gen2: $*"; }

link_gt_gen1() {
    case "$(cat "/sys/bus/pci/devices/$1/current_link_speed" 2>/dev/null)" in
        *5.0*|*8.0*|*16.0*|*32.0*) return 0 ;;
        *) return 1 ;;
    esac
}

retrain() { # $1 = upstream bridge BDF; TLS stays 2, we only ask to re-negotiate
    local ctl2 ctl
    ctl2=$(setpci -s "$1" CAP_EXP+30.W)
    setpci -s "$1" CAP_EXP+30.W="$(printf '%04x' $(( (0x$ctl2 & ~0xf) | 2 )))"
    ctl=$(setpci -s "$1" CAP_EXP+10.W)
    setpci -s "$1" CAP_EXP+10.W="$(printf '%04x' $(( 0x$ctl | 0x20 )))"
}

bfds=$(lspci -Dnn 2>/dev/null | awk '/10de:1e09/ {print $1}')
if [[ -z "${bfds}" ]]; then
    log "no CMP 50HX (10de:1e09) found; nothing to do"
    exit 0
fi

deadline=$(( $(date +%s) + TIMEOUT_MIN * 60 ))
while :; do
    all_fast=1
    for bdf in ${bfds}; do
        if link_gt_gen1 "${bdf}"; then
            continue
        fi
        all_fast=0
        tls=$(setpci -s "${bdf}" CAP_EXP+30.W 2>/dev/null)
        if [[ $(( 0x${tls:-0} & 0xf )) -eq 2 ]]; then
            upstream=$(basename "$(dirname "$(readlink -f "/sys/bus/pci/devices/${bdf}")")")
            log "${bdf}: card unlocked (TLS=2), firing retrain via ${upstream}"
            retrain "${upstream}"
            for _ in $(seq 1 $(( RETRAIN_WAIT_SEC * 10 ))); do
                link_gt_gen1 "${bdf}" && break
                sleep 0.1
            done
            if link_gt_gen1 "${bdf}"; then
                log "${bdf}: PASS, link at $(cat "/sys/bus/pci/devices/${bdf}/current_link_speed")"
            else
                log "${bdf}: retrain fired, link still at Gen1 (will retry)"
            fi
        fi
    done
    if [[ ${all_fast} -eq 1 ]]; then
        log "all CMP 50HX cards at Gen2 or better"
        exit 0
    fi
    if [[ $(date +%s) -ge ${deadline} ]]; then
        log "TIMEOUT after ${TIMEOUT_MIN} min; some card(s) still at Gen1"
        exit 1
    fi
    sleep "${POLL_SEC}"
done
