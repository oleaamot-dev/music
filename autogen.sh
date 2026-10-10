#!/bin/sh
# Run this to generate all the initial makefiles, etc.
set -e
srcdir=$(dirname "$0")
test -z "$srcdir" && srcdir=.
cd "$srcdir"
mkdir -p m4
autoreconf --force --install --verbose
cd - >/dev/null
if test -z "$NOCONFIGURE"; then
  exec "$srcdir/configure" "$@"
fi
