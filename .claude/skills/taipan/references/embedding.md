# Embedding Taipan in a C++ host

Everything a host needs is in `include/taipan.h`. Link `taipan_lib` (CMake) or
compile `src/*.cpp` into the project. See `examples/embed_demo.cpp` and
`examples/enemy_ai.tp` for a complete working host.

## Lifecycle

```cpp
#include "taipan.h"

TaipanState* state = taipan_create();       // core stdlib only (no blocking I/O)
// register natives here, BEFORE loading: the type checker needs them
taipan_load_file(state, "enemy_ai.tp");     // parse + type-check (+ bytecode cache)
taipan_run(state);                          // run top-level code
taipan_call_function(state, "onPlayerDeath", {TaipanValue::fromInt(3)});
while (running) taipan_update(state, dt);   // resume due coroutines every frame
taipan_reload_file(state, "enemy_ai.tp");   // hot reload
taipan_destroy(state);
```

- Every call returns `false` (or a null value plus `taipan_call_ok() == false`)
  on error. The formatted diagnostics go to `taipan_set_error_handler`
  (stderr by default) and are collected in `taipan_last_error`.
- A state is single-threaded. For multiple cores, run one state per thread.
- `taipan_open_standalone` adds `input`, `time.sleep` and `file.*`. A game
  should not call it; register sandboxed equivalents instead.

## Native functions

Every native has one signature: `TaipanValue fn(TaipanArgs args)`.

```cpp
TaipanValue native_spawn(TaipanArgs args) {
    std::string prefab = args.getString(0);
    int x = args.count() > 1 ? (int)args.getInt(1) : 0;   // optional args: check count()
    return TaipanValue::fromObject("Entity", world.spawn(prefab, x));
}
taipan_register_function(state, "engine.spawn", native_spawn, "(str, int=) -> Entity");
```

- A dotted name creates namespaces (`engine`, `engine.audio`, ...). Scripts use
  them without `import`.
- The signature string is optional, but provide it: with it, calls are
  type-checked at load time, and hover/completion show real types.
  - Format: `(T1, T2=, T3...) -> R`. `=` marks an optional parameter, `...` a
    variadic one.
  - Omit `-> R` (or write `-> void`) for no result.
  - Use `-> (int, int)` for multiple results.
  - Any type name works, including registered class names.
- `args.getInt/getFloat/getString/getBool/getObject<T>(i)` read the raw
  arguments directly. They are the fast path, and raise a script error on a
  type mismatch.
- `args.typeAt(i)` / `args.isNull(i)` check an argument's type.
- `args[i]` converts all arguments to `TaipanValue` on first use, which is
  slower.
- `args.error("msg")` raises a catchable script error.
- `args.call(fn, {...})` calls a script function (a callback).

## Native classes (engine-owned objects)

```cpp
taipan_register_class(state, "Entity")
    .alias("entity")                         // extra type name scripts may use
    .property("hp", &Entity::hp)             // types are deduced from member pointers
    .property("name", &Entity::name)         // const members become read-only
    .method("takeDamage", &Entity::takeDamage)
    .method("custom", [](void* self, TaipanArgs a) { return TaipanValue(); }, "(int) -> bool");
```

- The engine keeps ownership. Scripts hold handles created by
  `TaipanValue::fromObject("Entity", ptr)`.
- When the object dies, call `taipan_invalidate_object(state, ptr)`. Any later
  script use is a catchable `use of invalid Entity handle` error, never a crash.
- Supported member and parameter types:
  - `int` / integral, `float` / `double`, `bool`, `std::string`
  - `T*` of a registered class
  - `TaipanValue` (= `any`)

## Values and lifetimes

- `TaipanValue` copies primitives and strings.
- Lists, dicts and functions are references into the VM. They stay valid while
  the script references them, or for the duration of the current native call.
  Keep one across frames with `taipan_pin` / `taipan_unpin`.
- Create containers with `taipan_new_list`, `taipan_new_dict`,
  `taipan_list_add`, `taipan_dict_set`.
- Globals: `taipan_get_global(state, "playerHealth")`,
  `taipan_set_global_int/_float/_bool/_string`. Setting a global the script
  declared keeps its static type contract, so set the right kind of value.

## Binding files (.tpi)

Tools that never run the engine (the language server, `taipan --check`, CI)
cannot see natives registered at runtime. A `.tpi` file declares them:

```
namespace engine {
    func spawn(str prefab, int x = 0) -> Entity
    float gravity
}
class Entity as entity {
    int hp
    readonly str name
    func takeDamage(int amount)
}
```

- Generate it from the real registrations with
  `taipan_dump_bindings(state, "engine.tpi")`, so it can never drift from the
  engine.
- Load it with `taipan_load_bindings(state, path)`, or
  `taipan --check --bindings engine.tpi script.tp`.
- The language server loads every `*.tpi` in the workspace automatically.
- Stubs never override real registrations. Calling a stub the host didn't
  register is a catchable `missing native` error.

## Script instances (one environment per entity)

```cpp
TaipanScript* s = taipan_script_new(state, "enemy_ai.tp");  // shared bytecode, own globals
taipan_script_run(state, s);                                 // its top-level code (starts its coroutines)
taipan_script_call(state, s, "onUpdate", {TaipanValue::fromFloat(dt)});
taipan_script_get(state, s, "hp");  taipan_script_set(state, s, "hp", TaipanValue::fromInt(5));
taipan_script_free(state, s);                                // also stops its coroutines
```

- Each instance has its own globals, like a Lua environment per entity.
  Modules it imports are shared.
- `taipan_reload_file` updates every live instance of that file and keeps each
  one's state.
- `taipan_load_file` / `taipan_call_function` still work for a single "main"
  script.

## Coroutines from the host

- `taipan_update(state, dt)` advances script time and resumes coroutines whose
  `wait` has elapsed.
- `taipan_coroutine_count` and `taipan_next_wake` let the host decide when to
  update or sleep. The standalone runner loops on them.
- An uncaught error inside a coroutine is reported and ends that coroutine
  only.

## Hot reload semantics (`taipan_reload_file`)

- Functions are replaced. Running coroutines call the new versions from their
  next call on.
- Classes are updated in place, so live instances get the new methods. A field
  added by the reload gets its zero value on existing instances (not its
  initializer).
- Global variables keep their current values; new globals are initialized.
- Other top-level statements (`start ...`, prints) are not re-run.
- `func onReload()` is called afterwards if the script defines it.

## Other settings

- `taipan_set_script_root(state, dir)`: where imports resolve from.
- `taipan_set_bytecode_cache(state, enabled, dir)`: defaults to `<temp>/taipan_cache`.
  An entry is reused only if the source and every import are unchanged.
- `taipan_set_print_handler`: where `print` goes.
- `taipan_set_warnings`.
- GC tools: `taipan_gc_collect`, `taipan_gc_stats`, `taipan_set_gc_stress` (debugging).
