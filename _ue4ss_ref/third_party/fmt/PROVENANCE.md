# {fmt} 11.2.0 — vendored copy (provenance)

## Why this is committed instead of downloaded at build time

`DynamicOutput/Output.hpp` (from the pinned UE4SS headers) includes `<fmt/core.h>`, and
`Output::send` is a template that expands **in our translation units** into calls to
non-template fmt symbols (`fmt::vformat`, `dragonbox::to_decimal<float>`, …). UE4SS.dll
links fmt 11.2.0 statically and does **not** export it, so those symbols would be
unresolved at link time:

```
LNK2019: fmt::v11::report_error / dragonbox::to_decimal<float> ... (8 symbols)
```

So `src/format.cc` and `src/os.cc` are compiled into this DLL. The version therefore has
to match UE4SS's **exactly**: `std::string` / `std::wstring` arguments cross the DLL
boundary, and fmt objects constructed on our side are consumed by UE4SS's machinery.

Committing it (rather than fetching it in `tools/bootstrap.cmd`) keeps the build
reproducible offline and pinned to the same 11.2.0 that commit `0196ef29` of RE-UE4SS
pins in `deps/third/CMakeLists.txt` (`FetchContent GIT_TAG`).

## Where it came from

| Item | Value |
|---|---|
| Project | [fmtlib/fmt](https://github.com/fmtlib/fmt) |
| Version | **11.2.0** |
| Obtained | `_ue4ss_ref/third_party/fmt` as laid down by the RE-UE4SS `0196ef29` checkout |
| Fetched / verified | 2026-10-18, every file compared byte-for-byte against `raw.githubusercontent.com/fmtlib/fmt/11.2.0/...` |

Verification result (sha256, 17 files):

| File | vs upstream 11.2.0 |
|---|---|
| `include/fmt/{args,base,chrono,color,compile,core,format,format-inl,os,ostream,printf,std,xchar}.h` | identical |
| `src/{fmt,format,os}.cc` | identical |
| `include/fmt/ranges.h` | **patched** — see below |
| `LICENSE` | upstream `LICENSE` at tag 11.2.0, verbatim |

## The one local modification: `include/fmt/ranges.h`

RE-UE4SS applies a two-hunk patch via `cmake/modules/MacroPatch.cmake`. Unreal defines a
`check(...)` macro that collides with fmt's own `check`, so the patch temporarily undefines
it and restores it at the end of the header. Our copy carries the same two hunks:

```diff
@@ 8a9,13
+// [UE4SS patch] UE defines a 'check' macro that collides with fmt.
+#ifdef check
+#  define FMT_UE_CHECK_GUARD 1
+#  undef check
+#endif
@@ 849a855,859
+// [UE4SS patch] restore UE's 'check' macro
+#ifdef FMT_UE_CHECK_GUARD
+#  undef FMT_UE_CHECK_GUARD
+#  define check(expr) ((void)0)
+#endif
```

Everything else in that file is upstream. (Note: the copy as originally laid down had CRLF
line endings in this one file, unlike the other 16; it has been normalised to LF so the
whole tree has uniform line endings.)

## Licensing

{fmt} is MIT-licensed; the upstream `LICENSE` text is included in this directory and is
what applies to these files — **not** this repository's own MIT `LICENSE`, which covers our
code only. See `README.md` §9 for the third-party summary.
