#!/bin/bash
set -eu

SCRIPTPATH="$(cd "$(dirname ${BASH_SOURCE[0]})" && pwd)"

. ../lib/utils.sh

SCHNAME='eiffel'
MODNAME="sch_eiffel"

main () {
    USAGE="$0 DEVNAME"
    if (($# < 1)); then
        echo $USAGE
        exit 1
    fi
    devname="$1"
    sudo ls > /dev/null
    (cd $SCRIPTPATH; make)
    remove_sch_mod $MODNAME $SCHNAME
    sudo insmod $SCRIPTPATH/${MODNAME}.ko gso_split=1
    sudo tc qdisc replace dev $devname root $SCHNAME
}

main "$@"
