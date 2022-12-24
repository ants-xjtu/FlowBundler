#!/bin/bash
set -eu

SCRIPTPATH="$(cd "$(dirname ${BASH_SOURCE[0]})" && pwd)"

. ../lib/utils.sh

SCHNAME='eiffel'
MODNAME="sch_eiffel"

main () {
    sudo ls > /dev/null
    remove_sch_mod $MODNAME $SCHNAME
}

main "$@"
