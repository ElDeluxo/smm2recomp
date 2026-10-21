#!/bin/sh
# zig's dlltool for rustc's windows-gnu raw-dylib imports (`make embed-server`; Windows targets)
exec "${ZIG:-zig}" dlltool "$@"
