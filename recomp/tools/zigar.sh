#!/bin/sh
# zig ar for cargo's cc crate (tools/zigcc.sh)
exec "${ZIG:-zig}" ar "$@"
