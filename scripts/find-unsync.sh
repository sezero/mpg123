#!/bin/sh

dump=src/mpg123-id3dump
if ! test -e "$dump"; then
  cat <<EOT >&2
Run from a mpg123 build dir with a debug build of
mpg123-id3dump in src/.
EOT
  exit 1
fi
for f in "$@"
do
  if test -n "$("$dump" -n "$f" 2>&1 | grep -e "going to de-unsync the frame data" -e  "ID3v2: flags 0x" | grep -v 0x00)"; then
    printf "possible unsynchronization: %s\n" "$f"
  fi
done
