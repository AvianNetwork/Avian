#!/bin/sh

TOPDIR=${TOPDIR:-$(git rev-parse --show-toplevel)}
SRCDIR=${SRCDIR:-$TOPDIR/src}
MANDIR=${MANDIR:-$TOPDIR/doc/man}

AVIAND=${AVIAND:-$SRCDIR/aviand}
AVIANCLI=${AVIANCLI:-$SRCDIR/avian-cli}
AVIANQT=${AVIANQT:-$SRCDIR/qt/avian-qt}

[ ! -x $AVIAND ] && echo "$AVIAND not found or not executable." && exit 1

# The autodetected version git tag can screw up manpage output a little bit
AVNVER=($($AVIANCLI --version | head -n1 | awk -F'[ -]' '{ print $6, $7 }'))

# Create a footer file with copyright content.
# This gets autodetected fine for aviand if --version-string is not set,
# but has different outcomes for avian-qt and avian-cli.
echo "[COPYRIGHT]" > footer.h2m
$AVIAND --version | sed -n '1!p' >> footer.h2m

for cmd in $AVIAND $AVIANCLI $AVIANQT; do
  cmdname="${cmd##*/}"
  help2man -N --version-string=${AVNVER[0]} --include=footer.h2m -o ${MANDIR}/${cmdname}.1 ${cmd}
  sed -i "s/\\\-${AVNVER[1]}//g" ${MANDIR}/${cmdname}.1
done

rm -f footer.h2m
