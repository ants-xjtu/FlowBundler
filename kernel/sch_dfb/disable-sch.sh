#!/bin/bash
set -eu

SCRIPTPATH="$(cd "$(dirname ${BASH_SOURCE[0]})" && pwd)"

. ../lib/utils.sh

SCHNAME='dfb'
MODNAME="sch_$SCHNAME"

main () {
    sudo ls > /dev/null
    remove_sch_mod $MODNAME $SCHNAME
}

main "$@"
