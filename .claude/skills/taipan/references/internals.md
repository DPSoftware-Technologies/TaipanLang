# Taipan internals: changing the interpreter

Contents:
1. Pipeline and file map
2. Recipes for common changes
3. Garbage-collector rules (read before touching objects)
4. Language-server analysis
5. Conventions
6. Testing

## 1. Pipeline and file map

```
source -> lexer -> parser -> AST -> compiler (type check + codegen, one pass) -> Proto bytecode -> VM
```

| File | Role |
|---|---|
| `include/taipan.h` | Public embedding API. Templates for the class builder live here. |
| `src/internal.h` | `Value`, heap objects (`Obj*`), opcode table `TAIPAN_OPCODES`, `Proto`, the `VM` struct |
| `src/lexer.*` | Tokens, newline / `\` continuation rules, f-string tokens |
| `src/parser.*`, `src/ast.h` | Recursive descent. A newline-significance mode stack. `.tpi` binding parser. |
| `src/types.h` | Static types (`TK`), `ClassInfo`, `ModuleSymbols` |
| `src/compiler.*` | Hoists top-level declarations, then checks and emits in one walk. Null narrowing, diagnostics, `AnalysisSink` hooks. |
| `src/vm.cpp` | Interpreter loop `VM::execute`, calls, `INVOKE`, coroutines, try/catch unwinding, fields/indexing, runtime type checks |
| `src/gc.cpp` | Generational + incremental collector |
| `src/object.cpp` | Interned strings, dict, `toStr`, equality / hashing |
| `src/stdlib.cpp` | Standard library, list/dict methods |
| `src/module.cpp`, `src/loader.h` | Import resolution, compile, bytecode cache (`.tpc`), hot reload, disassembler |
| `src/bindings.cpp` | Loading and dumping `.tpi` files |
| `src/analysis.*` | Editor analysis for the LSP (fresh VM per request; results copied out as plain data) |
| `src/api.cpp` | Implementation of `taipan.h`, host-value conversion, native handles |
| `src/diag.cpp` | Rust-style diagnostic formatting |
| `tools/taipan_main.cpp` | Standalone runner |
| `tools/taipan_lsp.cpp` | LSP server: JSON, stdio transport, UTF-16 positions |
| `editors/vscode/` | Extension: TextMate grammar, language config, `extension.js` LSP client |

Key runtime facts:
- Each coroutine is an `ObjThread` with its own value stack and frames, so
  `wait` just returns `Yielded` from the interpreter loop.
- Upvalues are index-based, so stacks may reallocate safely.
- Runtime errors are C++ `ScriptError` exceptions, caught in `VM::run` and
  unwound to the nearest `try` handler. `OP_THROW` has a fast path that skips
  the C++ exception when the handler is in the same run.
- Every bytecode word carries a source `Span` for diagnostics.

### The interpreter loop (`VM::execute`), read before editing it

- **Register state.** `ip`, `sp` (stack top) and `slots` (frame base) live in
  local variables. `SYNC()` writes `ip`, `lastOp` and the stack top back to
  the frame and thread. `LOAD()` re-reads everything.
  - Call `SYNC()` before anything that can throw, call out (natives, `getField`
    on engine objects, `callSetup`), or collect garbage.
  - Call `LOAD()` after anything that may push frames or grow the stack
    (calls, imports, natives).
  - An op that throws without `SYNC()` reports the wrong source location.
  - Using `sp` after a call without `LOAD()` is a use-after-free when the stack
    grew.
- **Stack capacity.** `pushFrame` reserves `Proto::maxStack` slots
  (`computeMaxStack`, a walk over every bytecode path), so pushes inside a
  frame have no bounds check. A new opcode must get its stack effect in
  `computeMaxStack`, or frames can overflow their reservation.
- **Dispatch.** Computed goto on GCC/Clang, a `switch` on MSVC. Ops end with
  `NEXT`. Never put `NEXT` inside an inner loop, because in the `switch`
  build it is `continue`. Objects with destructors must be scoped so `NEXT`
  doesn't jump past their destructor.
- **`Value` copies field by field** on purpose: a single 16-byte copy right
  after two smaller stores stalls store forwarding and made the loop 2–3×
  slower. Keep the explicit copy constructor.
- **Fused instructions** come from the compiler's peephole layer:
  - `emitBinary`: `x op constant` becomes `*_K`
  - `emitJump`: compare + `JUMP_IF_FALSE` becomes `J*` / `J*_K`
  - `assign`: `local += simple` becomes `ADD_LOCAL[_K]`
  - `forIn`: `for int i in range(...)` becomes `RANGE_INIT` / `RANGE_LOOP`
  - field access on a statically known class becomes `GET_FIELD_I` /
    `GET_LFIELD` / `SET_FIELD_I` (index plus a name guard, so hot reload
    stays correct)
  - method and namespace calls become `INVOKE_C`, with an inline cache keyed
    by class `uid` / namespace epoch
  - builtins become `GET_BUILTIN_C`

  Fusion is only allowed when no jump targets the boundary (`lastIs()`,
  `label()`, `patchJump()` track this).
- **Caches** live in `Proto::caches`. Bump `ObjClass::uid` or `VM::nsEpoch`
  whenever what a cache could have stored changes.
- Before and after any loop change, measure with
  `build/taipan tests/perf/core.tp` (Lua equivalent: `tests/perf/core.lua`)
  and `tests/perf/host_bench.cpp`. Several "obvious" speedups (inlining the
  frame push, `__forceinline` on MSVC) measured slower.

## 2. Recipes for common changes

**New built-in function**
1. Add `defineNative("name", fn, "(sig) -> ret")` in `VM::openCore`
   (`stdlib.cpp`). Use `openStandalone` instead if the function blocks.
2. Read arguments with the `argStr/argNum/argInt` helpers so mistakes give good
   errors. Natives read their arguments in place on the VM stack: missing
   args up to index 3 read as null, and `argv` is only valid until the native
   calls back into the VM (copy what you need first, as `map` / `sort` do).
3. A namespace constant: set the member value and add `nativeSigs["ns.name"] = "float"`.

**New list/dict method**
1. Add the name to the table in the `VM::VM` constructor (`vm.cpp`).
2. Add its minimum argument count in `invokeBuiltin` (`listMin` / `dictMin`).
3. Implement it in `builtinListMethod` / `builtinDictMethod`.
4. Add its static signature in `Compiler::builtinMethod`.
5. Add its completion detail in `Builder::builtinMembers` (`analysis.cpp`).

**New operator / opcode**
1. Append it to `TAIPAN_OPCODES` in `internal.h` (append, so existing numbers
   stay stable). Its operand count is used by the disassembler and the cache.
2. Give it a stack effect in `computeMaxStack` (`vm.cpp`).
3. Emit it in `compiler.cpp`, including the static result type in `binaryResult`.
4. Execute it in `VM::execute` with `CASE(NAME) { ...; NEXT; }`, following
   the SYNC/LOAD rules above.
5. Bump `kCacheVersion` in `module.cpp`, so stale `.tpc` files are rejected
   instead of misread.

**New syntax**
1. Token in `lexer.*`.
2. Parse into an AST node (`ast.h`, `parser.cpp`).
3. Check and emit in the compiler.
4. Update `editors/vscode/syntaxes/taipan.tmLanguage.json`.
5. Update `.claude/skills/taipan/references/language.md`.

**New diagnostic**
- Use `error(span, "short title", "label saying expected vs found", {notes})` in
  the compiler.
- Point the span at the exact offending token. Back-reference declarations
  where useful ("declared as const on line N").

**Embedding API addition**
1. Declare it in `taipan.h` with a one-line comment.
2. Implement it in `api.cpp`.
3. Add a check in `tests/api_test.cpp`.
4. Document it in README §4 and `references/embedding.md`.

## 3. Garbage-collector rules

The collector is generational (young list, objects promoted after surviving
two minor collections, remembered set) and incremental (tri-colour major
cycle). Breaking these rules causes use-after-free bugs that appear only
under GC pressure.

- **Write barrier on every store into a heap object.** Call
  `vm.barrier(container, value)` after storing into a list, dict, instance
  field, module global, closed upvalue, class or namespace. For bulk
  mutations (sort, merge) call `vm.touch(container)`. Thread stacks are the
  exception: they are re-scanned instead.
- **Collection only happens at safepoints:** loop back-edges (`LOOP`,
  `RANGE_LOOP`) and calls (`CALL`, `INVOKE`, `INVOKE_C`), always after
  `SYNC()`. Allocation alone never collects, and values above the synced
  stack top are invisible to the collector. A native that calls back
  into script (`callValue`, `args.call`) does pass safepoints, so any object it
  holds only in C++ locals must be pushed to `vm.tempRoots` (and popped
  afterwards). See `map` / `filter` in `stdlib.cpp`.
- **Compilation runs with `gcPause++`.** Long C++-only sequences that allocate
  may also pause the collector.
- **Weak tables** (the string intern table, native handles) are cleaned in
  `freeObject`. A lookup that finds a dead-but-unswept object must resurrect
  it, as `VM::str` does.
- **Verifying GC changes:**
  - `tests/run_tests.sh --gc-tiny` (tiny incremental steps)
  - `tests/run_tests.sh --gc-stress` (collect at every safepoint)
  - a `-DTAIPAN_GC_DEBUG` build, which poisons freed objects so a dangling
    reference aborts immediately
  - `tests/gc_torture.tp` is the allocation-heavy test

## 4. Language-server analysis

- `compileModule` takes `CompileOptions::sink` (an `AnalysisSink`). With a sink
  the compiler records:
  - uses (span, hover text, declaration span)
  - local scopes
  - class ranges
  - the completion receiver
- Member completion works by replacing the identifier after `.` with the
  placeholder `__tp_complete__` and reading which receiver the compiler
  reported.
- Syntax errors: with a sink, the loader still compiles the statements that
  parsed, but reports only the syntax errors.
- When adding language features, also record uses for the new names
  (`recordUse` / `recordRes` / `recordMember`), so hover and go-to-definition
  keep working.

## 5. Conventions

- C++17, no dependencies. It must compile with g++ (MinGW), clang and MSVC:
  no GNU extensions, no `<bits/...>`.
- Style:
  - attached braces, 4 spaces
  - no space before a function's parameter list (`int main() {`, `[this](VM& vm)`)
  - LF line endings
- Comment density: short header comment per file, comments for non-obvious
  reasons only.
- The language design comes from `taipandesign.md`. When adding behaviour the
  spec doesn't cover, record the decision in README §6.
- Diagnostics follow the spec's Rust-style format: title, `--> file:line:col`,
  source line, carets, label. `warning:` must not block execution.

## 6. Testing

Run `.claude/skills/taipan/scripts/verify.sh` for everything. Or run the pieces:

| Command | Covers |
|---|---|
| `./build.sh debug` | Build with `-Wall -Wextra`. Keep it warning-free. |
| `tests/run_tests.sh [--gc-tiny\|--gc-stress]` | Golden stdout+stderr for `tests/*.tp`, `tests/errors/*.tp`, imports |
| `tests/run_tests.sh --update` | Regenerate expected outputs. Only after reviewing the diff. |
| `build/api_test` (run from `tests/`) | Embedding API, hot reload, bindings, runtime errors |
| `python tests/lsp_test.py` | The LSP server end to end over JSON-RPC |
| `build/taipan examples/benchmark.tp` | Performance and correctness of hot paths (prints `results verified: true`) |

To test a new language feature:
1. Add a script to `tests/` (or `tests/errors/` for diagnostics).
2. Run it, read the output carefully, then `--update` and commit the
   expected file.
