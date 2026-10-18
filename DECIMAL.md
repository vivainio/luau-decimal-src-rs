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
LUAU_REVISION. Preserve Custom. The decimal implementation requires `__int128`.
