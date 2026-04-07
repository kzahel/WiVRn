# macOS `mmsghdr` Regression Note

Date: 2026-04-07

## Summary

The macOS build failure in `common/wivrn_sockets.cpp` was caused by a guard mismatch introduced during the Windows socket portability refactor.

- Older common networking code already used `mmsghdr`, `recvmmsg`, and `sendmmsg`.
- Commit `062945f6d886cd3cfc975274190a23c040f0e180` (`Add macOS headless host checkpoint`) added an Apple-local `struct mmsghdr` and Apple shim implementations.
- Commit `acc0829e385846676219341743721765b52ad715` (`Start Windows headless control socket port`) moved socket compatibility structs into `common/wivrn_sockets.h`, widened the shim block to `__APPLE__ || _WIN32`, and accidentally left `struct mmsghdr` defined only for `_WIN32`.

That left macOS compiling code that uses `mmsghdr` without any visible definition.

## Failure Mode

The current shim block lives in `common/wivrn_sockets.cpp` behind:

```cpp
#if defined(__APPLE__) || defined(_WIN32)
```

But before this fix, `common/wivrn_sockets.h` defined `struct mmsghdr` only behind:

```cpp
#if defined(_WIN32)
```

On macOS, the function signatures in `common/wivrn_sockets.cpp` therefore created only a forward declaration:

```cpp
recvmmsg(native_socket_t fd, struct mmsghdr * msgvec, ...)
```

That is enough for a pointer type, but not enough for:

- `msgvec[received].msg_hdr`
- `msgvec[received].msg_len`
- `std::array<mmsghdr, num_messages>`
- `std::vector<mmsghdr>`

Clang then reports `subscript of pointer to incomplete type 'struct mmsghdr'` and related incomplete-type container errors.

## Why This Is Not Just a Missing Header

The macOS SDK does not provide `mmsghdr`, `recvmmsg`, or `sendmmsg`, so adding a system include would not solve this. The project already needs a local compatibility type whenever the local Apple shim is compiled.

## Fix Applied

`struct mmsghdr` is now defined for both Apple and Windows:

```cpp
#if defined(__APPLE__) || defined(_WIN32)
```

This matches the existing shim guard in `common/wivrn_sockets.cpp` and restores the pre-refactor macOS behavior while keeping the Windows portability layer intact.

## Smallest Safe Rule Going Forward

If a platform-local shim is compiled, any compatibility structs it depends on must be visible under the same guard or a broader one. Keep the type and shim guards aligned when iterating further on the Windows socket work.
