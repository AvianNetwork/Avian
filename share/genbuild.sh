#!/bin/sh
# Copyright (c) 2012-2016 The Bitcoin Core developers
# Copyright (c) 2017-2019 The Raven Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

# Debug flag (set AVIAN_GENBUILD_DEBUG=1 to print diagnostic info to stderr)
DEBUG="${AVIAN_GENBUILD_DEBUG:-0}"

log() {
    if [ "${DEBUG}" = "1" ]; then
        # shellcheck disable=SC2068
        echo "genbuild: $@" 1>&2
    fi
}

if [ $# -gt 1 ]; then
    cd "$2"
fi
if [ $# -gt 0 ]; then
    FILE="$1"
    shift
    if [ -f "$FILE" ]; then
        INFO="$(head -n 1 "$FILE")"
    fi
else
    echo "Usage: $0 <filename> <srcroot>"
    exit 1
fi

git_check_in_repo() {
    ! { git status --porcelain -uall --ignored "$@" 2>/dev/null || echo '??'; } | grep -q '?'
}

DESC=""
SUFFIX=""
if [ "${AVIAN_GENBUILD_NO_GIT}" != "1" -a -e "$(which git 2>/dev/null)" -a "$(git rev-parse --is-inside-work-tree 2>/dev/null)" = "true" ] && git_check_in_repo share/genbuild.sh; then
    # clean 'dirty' status of touched files that haven't been modified
    git update-index -q --refresh

    # if latest commit is tagged and not dirty, then override using the tag name
    RAWDESC=$(git describe --abbrev=0 2>/dev/null)
    if [ "$(git rev-parse HEAD)" = "$(git rev-list -1 $RAWDESC 2>/dev/null)" ]; then
        git diff-index --quiet HEAD -- && DESC=$RAWDESC
    fi
    log "HEAD=$(git rev-parse --short HEAD 2>/dev/null), RAWDESC=${RAWDESC:-<none>}, DESC=${DESC:-<unset>}"

    # otherwise generate suffix from git, i.e. string like "59887e8-dirty"
    SUFFIX=$(git rev-parse --short HEAD)
    if git diff-index --quiet HEAD -- . ":(exclude)depends/"; then
        log "Working tree clean (excluding depends/). SUFFIX=${SUFFIX}"
    else
        log "Dirty paths detected (excluding depends/):"
        git diff-index --name-status HEAD -- . ":(exclude)depends/" 1>&2 || true
        SUFFIX="$SUFFIX-dirty"
    fi
fi

if [ -n "$DESC" ]; then
    NEWINFO="#define BUILD_DESC \"$DESC\""
elif [ -n "$SUFFIX" ]; then
    NEWINFO="#define BUILD_SUFFIX $SUFFIX"
else
    NEWINFO="// No build information available"
fi

log "NEWINFO computed: $(echo "$NEWINFO" | sed 's/\"/"/g')"

# only update build.h if necessary
if [ "$INFO" != "$NEWINFO" ]; then
    echo "$NEWINFO" >"$FILE"
fi
