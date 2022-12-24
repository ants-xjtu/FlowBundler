#!/bin/bash
set -eu

SCRIPTPATH="$(cd "$(dirname ${BASH_SOURCE[0]})" && pwd)"

. $SCRIPTPATH/../lib/utils.sh

SCHNAME='carousel'
MODNAME="sch_carousel"

main () {
    sudo ls > /dev/null
    remove_sch_mod $MODNAME $SCHNAME
}

main "$@"
