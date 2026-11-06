#!/usr/bin/env bash
# Fetches the OpenFront.io repository (maps + TypeScript engine) at the
# commit this engine follows, into third_party/openfront (git-ignored).
#
# Maps are CC BY-SA 4.0 and the TypeScript code is AGPL-3.0; they are not
# vendored into this repository.
#
#   tools/fetch_openfront.sh            # pinned commit
#   OPENFRONT_REF=main tools/fetch_openfront.sh
set -euo pipefail
REF="${OPENFRONT_REF:-1f6f7e36f93512b8cb5c4eec93aa2831da111f46}"
DEST="$(cd "$(dirname "$0")/.." && pwd)/third_party/openfront"
if [ ! -d "$DEST/.git" ]; then
  git init -q "$DEST"
  git -C "$DEST" remote add origin https://github.com/openfrontio/OpenFrontIO.git
fi
git -C "$DEST" fetch -q --depth 1 origin "$REF"
git -C "$DEST" checkout -q FETCH_HEAD
echo "OpenFront $(git -C "$DEST" rev-parse --short HEAD) in $DEST"
echo "export OFE_MAPS_DIR=$DEST/resources/maps"
