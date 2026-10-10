# Taipan for VS Code

Language support for Taipan, the game scripting language.

- Syntax highlighting for `.tp` scripts and `.tpi` binding files
- Errors and warnings as you type (the real Taipan type checker)
- Hover for types and signatures
- Go to definition, including into imported modules
- Completion for locals, globals, built-ins, and members after `.`
- Outline of classes, functions, enums and globals
- **Taipan: Run Current File** command

## Setup

The extension starts the `taipan-lsp` language server. It looks for it in this
order:

1. the `taipan.server.path` setting
2. `bin/taipan-lsp` inside the extension (bundled builds)
3. `taipan-lsp` on your `PATH`

Build it from the repository root with `./build.sh` or `build.bat`
(it is written to `build/taipan-lsp`).

## Engine bindings

Scripts for a game engine use natives such as `engine.spawn()` that the
engine registers at runtime. Describe them in a `.tpi` binding file anywhere
in the workspace, or generate one from the engine itself with
`taipan_dump_bindings(state, "engine.tpi")`, and the editor will type-check
against it.

## Settings

| Setting | Default | Meaning |
|---|---|---|
| `taipan.server.path` | `""` | Path to `taipan-lsp` |
| `taipan.runtime.path` | `taipan` | Runner used by **Run Current File** |
| `taipan.scriptRoot` | `""` | Folder imports resolve from (default: each file's folder) |
| `taipan.bindings` | `[]` | Extra `.tpi` files outside the workspace |
| `taipan.standalone` | `true` | Allow standalone-only built-ins (`input`, `file.*`) |
