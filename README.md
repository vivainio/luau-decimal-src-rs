# luau-decimal-src-rs

Experimental Luau source crate with native decimal support for
[mlua-decimal-native](https://github.com/vivainio/mlua-decimal-native).

This fork bundles [vivainio/luau](https://github.com/vivainio/luau)'s native
decimal implementation. See [decimal integration](DECIMAL.md) for source provenance
and binding requirements. The Cargo package name remains `luau0-src`.

The decimal VM supports GCC/Clang and MSVC: compilers without native `__int128`
use portable two-word storage. CI tests Linux, macOS, and Windows.

The upstream source crate documentation follows below.

# luau-src
[![Build Status]][github-actions]

[Build Status]: https://github.com/mlua-rs/luau-src-rs/workflows/CI/badge.svg
[github-actions]: https://github.com/mlua-rs/luau-src-rs/actions

This crate contains the sources of [Luau] and logic to build them.
Intended to be consumed by the [mlua-sys] crate.

[Luau]: https://github.com/luau-lang/luau
[mlua-sys]: https://crates.io/crates/mlua-sys

# License

This project is licensed under [MIT license](http://opensource.org/licenses/MIT)
