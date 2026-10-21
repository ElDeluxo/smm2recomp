#!/bin/sh
# zig cc as a target's C compiler for cargo's cc crate (the course server's C / assembly parts, `make embed-server`
# for Windows and Linux): ZIG is zig, ZIGCC_TARGET zig's target. The crate's own --target=<rust triple> is dropped:
# zig takes its -target instead.
for a do
    shift
    case "$a" in --target=*) ;; *) set -- "$@" "$a" ;; esac
done
exec "${ZIG:-zig}" cc -target "$ZIGCC_TARGET" "$@"
