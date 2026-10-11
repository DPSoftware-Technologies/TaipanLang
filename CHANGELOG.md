# Changelog

## Unreleased

### Embedding API
- `taipan_set_file_reader(state, reader)`: the host can supply script sources -
  for example out of an asset package - before Taipan reads the disk. Imports
  resolve through the reader as well (dotted imports; a bare-name import still
  searches subfolders on disk only).

## 0.2.0

Performance, engine integration and math-friendly types. Scripts written for
0.1.0 run unchanged.

### Language
- **Operator overloading** for classes: `operator +(Vec2 o) -> Vec2 { ... }`.
  Overloadable: `+ - * / %`, `== < <= > >=` and unary `-`. `!=` is derived
  from `==`; missing `>`, `<=`, `>=` are derived from `<`. Compound
  assignment (`pos += vel * dt`) uses them. Operators are type-checked at
  compile time.
- **Static members**: `static int count = 0`, `static const float EPS = 0.001`,
  `static func create() -> Enemy`. Use them as `Enemy.count` / `Enemy.create()`
  from outside, or by bare name inside the class. Static initializers run once.
  On hot reload, statics keep their values.
- **Engine value types** in scripts: `Vector3(1, 2, 3)`, `Vector3.zero`,
  `Vector3.Distance(a, b)`, `2.0 * v`, `t.position += dir * speed * dt`.
  Values are immutable and compare by content, so `v.x = 5` is a compile
  error.
- `static` and `operator` are contextual words: they are still usable as
  names outside class bodies.

### Embedding API
- `taipan_register_value<T>(state, "Vector3")` registers a C++ value type
  that scripts own as immutable, garbage-collected copies. Builder methods:
  - `.constructor<T(A...)>()`
  - `.op(symbol, callable)`: one parameter makes unary minus; a first
    parameter that is not `T` makes the reversed form (`float * Vector3`)
  - `.toString(callable)`
- `.staticMethod(name, callable)` and `.constant(name, value)` add static
  members to any registered class.
- `taipan_bind_function(state, "Debug.Log", callable)` takes the signature
  from the C++ callable.
- `TaipanValue::fromValue(v)`, `TaipanValue::asValue<T>()`,
  `TaipanValue::isValue()`, `TaipanArgs::getValue(i, "Vector3")`.
- **Script instances (one environment per entity):**
  - `taipan_script_new`, `taipan_script_run`, `taipan_script_call`,
    `taipan_script_get`, `taipan_script_set`, `taipan_script_has_function`,
    `taipan_script_free`
  - each instance has its own globals and shares the bytecode
  - hot reload updates every instance and keeps its state
  - freeing an instance stops its coroutines
- `.tpi` binding files support `value class`, `func init` (constructor),
  `operator`, `static func` and `static T name`, and
  `taipan_dump_bindings` writes all of them.
- **ABI change:** the internal layout of `TaipanArgs` changed. Recompile
  host code. Binding code needs no source changes.

### Performance
- Scripts run 2–5× faster than in 0.1.0. With GCC, Taipan is close to Lua 5.4:
  about equal on loops and method calls, about 1.5× slower on deep recursion.
- The interpreter loop was rewritten:
  - raw value stack
  - computed goto on GCC/Clang
  - fused instructions for common patterns (constant operands,
    compare-and-jump, counted `for` loops, field access, native calls)
  - inline caches for fields, methods and natives
- Native calls are about 2× faster: arguments are passed as a raw buffer.
  Method calls and property reads on engine objects are faster than in Lua.
- MSVC builds use a switch-dispatched loop and are slower than GCC/Clang
  builds. For the best speed in an MSVC project, compile Taipan with clang-cl.
- Operators on numbers and strings keep their fast paths. Overloads are only
  looked up when an operand is an object.

### Tools
- Language server:
  - completes `ClassName.` with static members
  - shows statics and operators on hover
  - knows `static` / `operator`
- VS Code grammar highlights `static`, `operator`, `value`.
- The bytecode cache format changed (version 5). Old `.tpc` files are
  recompiled automatically.

### Fixes
- Incremental GC: objects created during marking could keep a cycle from
  finishing.
- Interpreter: errors thrown across computed-goto dispatch no longer skip
  destructors.

## 0.1.0

First release:
- bytecode VM in C++17, with no dependencies
- static types with `any`, null safety
- classes, interfaces, enums
- coroutines, modules, hot reload, bytecode cache
- incremental generational GC
- embedding API and `.tpi` bindings
- language server and VS Code extension
