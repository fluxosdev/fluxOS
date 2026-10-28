#!/bin/sh
# Compile fluxOS libc sources for the host, against the fluxOS headers (so
# they are the code fluxOS runs, with its constants and types), and rename
# what they define to flux_<name>, so a test can call them next to glibc's.
# errno becomes flux_errno, which the test defines.
#   tests/libc-host-build.sh OUT.o SOURCE.c...
set -e
out=$1
shift
cc=${HOSTCC:-cc}
dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT
objs=
for src in "$@"; do
    o="$dir/$(basename "$src" .c).o"
    $cc -std=gnu11 -O1 -g -Wall -Wextra -Wno-unused-parameter -fno-builtin -ffreestanding -fno-stack-protector \
        -nostdinc -isystem "$($cc -print-file-name=include)" -Iuser/include -Iinclude -Iuser/libc \
        -c "$src" -o "$o"
    objs="$objs $o"
done
ld -r -o "$dir/all.o" $objs
nm "$dir/all.o" | awk '$2 ~ /^[TDBRC]$/ { n = $3; if (n !~ /^__/ || n == "__vformat" || n == "__vscan") print n " flux_" n }' > "$dir/map"
echo "errno flux_errno" >> "$dir/map"
objcopy --redefine-syms="$dir/map" "$dir/all.o" "$out"
