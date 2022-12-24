#!/bin/bash
#######################################
# Remove a qdisc from all devices
# Args:
#   qdisc name
# Returns:
#   0 if success, non-zero on error
#######################################
remove_qdisc () {
    local USAGE="${FUNCNAME[0]} QDISC"
    if (($# < 1)); then
        echo $USAGE >&2
        return 1
    fi
    local qdisc="$1"
    for dev in $(ip link show | grep '^[0-9]\+' | cut -d':' -f2); do
        if [[ -n "$(tc qdisc show dev $dev | cut -d' ' -f2 | grep "^${qdisc}$")" ]]; then
            sudo tc qdisc del dev $dev root
        fi
    done
}

#######################################
# Remove a packet scheduling kernel module
# Args:
#   kernel module name
#   qdisc name of this kernel module
# Returns:
#   0 if success, non-zero on error
#######################################
remove_sch_mod () {
    local USAGE="${FUNCNAME[0]} MODULE_NAME QDISC_NAME"
    if (($# < 2)); then
        echo $USAGE >&2
        return 1
    fi
    local modname="$1"
    local qdisc="$2"
    local n_used=$(lsmod | grep "^${modname}\\s" | awk '{print $3}')
    if [[ -n "$n_used" ]]; then
        if (($n_used > 0)); then
            remove_qdisc $qdisc
        fi
        sudo rmmod $modname
    fi
}
