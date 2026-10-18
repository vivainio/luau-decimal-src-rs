# Decimal Luau sources

The `luau/` sources are a snapshot of https://github.com/vivainio/luau.
`LUAU_REVISION` records the exact source commit. The `Custom/` directory is
retained from mlua-rs/luau-src-rs because mlua uses its helper functions.

The compiler, VM, bytecode support, and optional code generator are taken from
the same revision. This fork changes the public type tags and adds
`lua_pushdecimal`, `lua_todecimal`, and `luaopen_decimal`; use matching bindings
from vivainio/mlua-decimal-native's `main` branch.

To refresh the snapshot, copy the Ast, Bytecode, CodeGen, Common, Compiler,
Config, Inliner, Require, and VM directories from the Luau checkout and update
LUAU_REVISION. Preserve Custom. GCC/Clang use native `__int128`; MSVC uses the portable
two-word mantissa. CI covers Linux, macOS, Windows/MSVC, and the forced portable
path on Linux (`LUAU_CXXFLAGS=-DLUAU_DECIMAL_PORTABLE`).
