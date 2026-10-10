---
name: taipan
description: Work with the Taipan game scripting language in this repository - writing or fixing .tp scripts, writing .tpi binding files, embedding Taipan in a C++ engine through include/taipan.h, and changing the interpreter itself (lexer, parser, type checker, bytecode VM, garbage collector, standard library, language server, VS Code extension). Use this skill whenever a task touches .tp or .tpi files, mentions Taipan, taipan.h, taipan-lsp, coroutines/hot reload/bindings in this project, or edits anything under src/, tools/, editors/ or tests/ here - even for small fixes, because Taipan's syntax differs from Python/Lua/Swift in ways that are easy to get wrong from memory.
---

# Taipan

Taipan is a statically typed game scripting language (a Lua replacement) implemented
in dependency-free C++17. This repository contains:

- the bytecode VM
- `taipan`, the standalone runner
- `taipan-lsp`, the language server
- a VS Code extension
- the design spec, `taipandesign.md`

You probably have no prior knowledge of Taipan. Its syntax looks familiar, but it
borrows from several languages at once. Don't guess syntax: read the relevant
reference file below first, then let the compiler confirm your work.

## Pick the reference for the task

| Task | Read |
|---|---|
| Writing, reviewing or fixing `.tp` scripts | `references/language.md` |
| C++ host code: `taipan.h`, native functions/classes, `.tpi` bindings, hot reload | `references/embedding.md` |
| Changing the interpreter, stdlib, LSP or editor support | `references/internals.md`, then the source |

`README.md` is the user-facing guide. `taipandesign.md` is the authoritative
design: when the implementation and the spec disagree, follow the spec unless
the README lists the difference as a deliberate decision.

## The compiler is your fastest feedback loop

Build once with `./build.sh` (or `build.bat` on Windows without Git Bash). That
produces `build/taipan` and `build/taipan-lsp`.

```
build/taipan --check file.tp                           # type-check only; Rust-style errors with line:col
build/taipan --check --bindings engine.tpi file.tp     # check engine scripts against a binding file
build/taipan --no-cache file.tp                        # run (coroutines keep running until they finish)
build/taipan --disasm file.tp                          # show bytecode
```

After writing or editing any `.tp` file, run `--check` on it and fix every error
before calling the work done. Also fix warnings unless they are intended (an
unused variable can be renamed with a leading `_`). The checker catches most
mistakes an unfamiliar author makes:
- missing types
- nullable values used without a null check
- `elif`
- wrong argument types

Its messages say what to change. Scripts that use engine natives (`engine.*`,
`entity`, ...) need a `.tpi` binding file to check outside the engine; see
`references/embedding.md`.

## Changing the C++ code

1. Read `references/internals.md` to find the right layer and the recipe for the change.
2. Follow the repository's C++ style:
   - attached braces, 4-space indent
   - no space before function parentheses (`int main() {`)
   - LF line endings
   - comments as sparse as the surrounding code
   - plain C++17 with no new dependencies; it must build with g++, clang and MSVC
3. Add or extend a test that shows the change working:
   - a script plus its expected output under `tests/`
   - a check in `tests/api_test.cpp`
   - a check in `tests/lsp_test.py`
4. Run the whole suite: `.claude/skills/taipan/scripts/verify.sh`. It builds, runs
   the golden tests under three garbage-collector modes, then the API and LSP
   tests. Report the result honestly. If something fails, show the decisive
   output line instead of claiming success.

Golden expected outputs live in `tests/expected/`. Regenerate them with
`tests/run_tests.sh --update` only after you have read the diff and confirmed
each change is intended. Otherwise the tests silently stop protecting anything.
