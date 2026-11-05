#!/bin/sh
# Report what Gamescope for Hermes can no longer link against: a library a
# system update removed (soname bump) or a symbol it dropped. pacman runs it
# after every library change (gamescope-hermes.hook); exits 1 when broken.
#
#   check-libs.sh [prefix]
prefix=${1:-/usr/lib/hermes-gamescope}
elf=$(printf '\177ELF')
broken=$(for file in "$prefix"/libexec/* "$prefix"/bin/* "$prefix"/lib/*.so; do
  [ -f "$file" ] && [ "$(head -c4 "$file")" = "$elf" ] || continue
  out=$(ldd -r "$file" 2>&1)
  # A missing library also leaves all its symbols undefined: name only the library then.
  missing=$(printf '%s\n' "$out" | sed -n 's/^[[:space:]]*\([^[:space:]]*\) => not found.*/\1/p')
  [ -n "$missing" ] && echo "$missing" || printf '%s\n' "$out" | sed -n 's/^undefined symbol: \([^[:space:]]*\).*/\1/p'
done | sort -u)
# Loaded with dlopen, so no ELF file names it.
ldconfig -p | grep -q '^[[:space:]]*libvulkan\.so\.1 ' || broken="$broken libvulkan.so.1"
[ -z "$broken" ] && exit 0
echo "==> WARNING: gamescope-hermes must be rebuilt or updated, it misses:" $broken
echo "    Rebuild it from its checkout (cd packaging/hermes && makepkg -fsi) or install a newer package."
exit 1
