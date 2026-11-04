# Local host integration patch

The upstream LuaJIT copyright and MIT license remain unchanged. This directory is
not relicensed as application code. The following small local patch is required
by the host's file-provenance query; it adds no public Lua or C API.

- `src/lj_obj.h`: one internal per-thread `native_call_tail` byte.
- `src/lj_state.c`: initialize that byte for a newly created coroutine.
- `src/vm_x64.dasc`: reset it on ordinary call dispatch and set it on a Lua tail
  call before the original caller frame is overwritten.

LuaJIT normally erases a tail caller, including when dynamically compiled text
returns a C function call. Merely reading the next surviving Lua frame can falsely
attribute that dynamic code to an outer trusted file. Looking up a current global
or upvalue is also insufficient: the erased function could change that value
before tail-calling. The host therefore reads the dispatch-time marker and returns
`nil` for an erased caller, as its API contract permits. Actual file provenance is
separately stored by the host using GC-safe weak prototype keys, never chunk-name
strings. No debug hook is installed, and native `require`, JIT, coroutines, and
error objects retain their original mechanisms.

The VM marker is currently implemented and tested for the supported Windows x64
build. Additional VM architectures need equivalent dispatch instrumentation before
claiming the same provenance behavior. The build input dependency glob includes
`.dasc`, so interpreter changes invalidate the generated VM.

Validation is recorded in the host's local `tests/v2-file-lua` evidence: Debug and
Release, direct and nested closures, coroutine sources, JIT loops, forged `@path`
chunk names, mutable-callsite tail-call spoof, and collected prototype lifetimes.
This is provenance attribution, not a sandbox against FFI, debug registry access,
trusted native extensions, or application-provided dynamic source.
