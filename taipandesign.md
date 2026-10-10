# Taipan Language Design Spec

**Status:** Work in progress
**Last updated:** 2026-10-10

## Vision

A scripting language for game engines, designed as a modern replacement for Lua.
Blends Python-style readability with C/C++-style structure and discipline.
Rejects Lua's `then/do/end` syntax in favor of brace-based blocks.

### Naming history
- ~~Cobra~~ — rejected, name collision with an existing (dormant) Python/C# hybrid language.
- ~~Viper~~ — rejected, oversaturated by gaming branding (VALORANT, Dota 2, Razer).
- ~~Krait~~ — rejected, name collision with an existing systems/bootloader language. Also clarified design direction: Taipan is explicitly NOT a low-level systems language (see "Anti-Krait principles" below).
- **Taipan** — confirmed clear, locked in.

### Anti-Krait principles (what Taipan deliberately is NOT)
Krait is a real existing language: transpiles to NASM assembly, zero runtime/GC, raw pointers,
inline `asm {}` blocks, built for kernels/bootloaders. Taipan is the philosophical opposite:
- No raw pointers exposed to the scripter, ever.
- No inline assembly — out of scope entirely.
- Has a managed runtime (GC), not manual memory management.
- Target user is a gameplay programmer, not a systems/kernel programmer.

### Target differentiators vs Lua (not yet fully decided, tracked here)
- Coroutines as a first-class, built-in feature (not bolted on later).
- Hot-reload treated as a core design goal, not an afterthought.
- Game-domain types (`vec2`, `vec3`, `color`, `entity`) likely built-in or in a tight standard library.
- Open question: optional/gradual static typing vs full static typing — to be decided in Phase 2.

---

## Phase 1 — Foundational Syntax ✅ LOCKED (pending 2 final confirmations)

### Comments
```
// single-line comment
/* multi-line
   comment */
```
Decision: `//` and `/* */`, matching C-family convention. `#` deliberately avoided and
reserved in case Taipan later wants preprocessor-style directives (`#import`, etc.).

### Statement termination
No semicolons required. Python-style rule set:
- Statement ends at newline.
- Newlines inside unclosed `()`, `[]`, `{}` are ignored (statement continues).
- Optional trailing `\` for explicit line continuation outside brackets.

```
int result = calculateDamage(
    base,
    multiplier
)

int x = 5 + \
        3
```

### Block delimiters
Braces `{ }` define all blocks. This is the core "anti-Lua" differentiator
(rejecting `then`/`do`/`end`).

### Indentation
Whitespace/indentation has **zero semantic meaning**. Braces alone define block structure.
All of the following are equivalent to the parser:
```
if condition {
    doSomething()
}

if condition
{
    doSomething()
}

if condition { doSomething() }
```
Convention (style guide, NOT compiler-enforced): K&R style recommended
(opening brace on same line), matching common modern scripting/game language convention
(Lua, GDScript, JS, Rust).

### Function declaration
**STATUS: NEEDS FINAL CONFIRMATION — currently defaulting to arrow-style per earlier session.**

Arrow-style (current lock-in):
```
func calculateDamage(int base, float multiplier) -> int {
    return base * multiplier
}

func healPlayer(int amount) {
    hp += amount
}
```
Rationale: `func` stays a 100% reliable first-token anchor for every function definition
(simplifies parsing, simplifies visual scanning). No return arrow = no return value,
avoiding the need for a `void` keyword on the common case (most game callbacks/handlers
don't return anything).

Alternative considered and NOT currently chosen: type-first (`func int name(args) {}`),
which would require a `void` keyword for non-returning functions.

### Variable declaration
```
int x = 5       // explicit value
int x           // defaults to zero-value of the type (0, 0.0, false, "", etc.)
```
Zero-value defaults modeled on Go's approach, to eliminate uninitialized-variable bugs.

**Open dependency:** zero-value behavior for custom/user-defined types (e.g. `entity`,
future `class`/`struct` types) is undecided — revisit once the type system (Phase 2)
and data structures (Phase 5) are designed.

### Constants
**STATUS: NEEDS FINAL CONFIRMATION — currently proposing `const` keyword + convention combined.**

Proposed:
```
const int MAX_HEALTH = 100
const float GRAVITY = 9.8

MAX_HEALTH = 50   // should be a compile error
```
Rationale: UPPER_CASE-only convention (Python-style) is not actually enforced by the
compiler and is easy to silently violate. Taipan blends Python readability with C++
discipline — constants are a place where real enforcement matters (prevents silent
game-balance bugs from accidental reassignment). UPPER_CASE is kept as a style
convention on top of real `const` enforcement, not instead of it.

---

## Phase 2 — Type System ✅ LOCKED

### Static/dynamic hybrid via `any`
Not full static (Lane A) or full dynamic (Lane B) — a hybrid via an explicit `any` type.
A variable declared with a concrete type is statically locked; a variable declared `any`
can hold and be reassigned to any type at runtime.

```
int myVar = 1
myVar = "oops"     // COMPILE ERROR — myVar is locked to int

any myVar2 = 1
myVar2 = "fine"    // OK — myVar2's type is "any", fully dynamic
```

No separate "var"-style inferred-and-locked declaration — `any` is the single, explicit
way to opt out of static typing. Keeps the type system's rule simple: every declaration
states its type, no exceptions.

### Primitive types

**Core (always available):** `any`, `str`, `int`, `float`, `bool`
(`null` is NOT a standalone declarable type — see Null Handling below; it's a value
that nullable types can hold.)

**ExtType (opt-in via explicit enable, not loaded by default):**
```
enable ExtType

int8, int16, int32, int64
uint8, uint16, uint32, uint64   // "byte" may alias uint8
f32, f64
```
Opt-in keeps the default language surface small/approachable for scripters and designers,
while still letting gameplay/engine programmers reach for precise bit-widths when needed
(networking payloads, inventory packing, perf-sensitive hot paths).

Deliberately excluded: `int24`/`uint24`. Considered for audio use cases, but rejected —
audio processing pipelines overwhelmingly work in `f32` internally regardless of source
file bit-depth; 24-bit only matters for raw file I/O, which doesn't justify a dedicated
language-level type.

### Function return type — no-return case (resolves earlier ambiguity)
Confirmed: omitting the `->` arrow means "returns nothing" (void-equivalent). There is
no `func null name()` form — `null` is not used to mean "no return value." This keeps
the Phase 1 arrow-style rule intact (see Phase 1 Function Declaration) and avoids
conflating "returns nothing" with "returns a null value."

```
func healPlayer(int amount) {              // returns nothing — no arrow
    hp += amount
}

func findEnemy(str name) -> entity? {      // returns a nullable entity
    return nearestEnemyNamed(name)
}
```

### Type inference
None. Every declaration must state its type explicitly. `any` fills the role that
inference serves in other languages (an explicit opt-out of static typing), so there's
no need for the compiler to guess a type — avoids an extra, error-prone subsystem.

### Null handling
Non-nullable by default. A type must be explicitly marked nullable with a `?` suffix
to be allowed to hold `null`. Compiler-enforced.

```
int x = 5
x = null              // COMPILE ERROR — int is not nullable

int? x = 5
x = null              // OK — explicitly marked nullable

entity target = findEnemy()    // COMPILE ERROR if findEnemy() can return null
                                 // and target isn't marked nullable
entity? target = findEnemy()   // correct — acknowledges it might not exist
```
Rationale: null-reference crashes are one of the most common runtime crash categories
in shipped games (referencing a dead enemy, an empty inventory slot, etc.). Lua has no
null-safety at all (`nil` can silently appear anywhere) — explicit, compiler-enforced
nullability is a concrete, real differentiator against Lua, not just a syntax preference.

### Dependency note: lists/arrays are NOT a primitive type
A `list`/array concept is required for Phase 3 (Control Flow, specifically `for` loops)
to make sense, but full array/list design (fixed vs dynamic sizing, indexing syntax,
slicing, etc.) belongs to Phase 5 (Data Structures), not here. Primitives are atomic
single-value types; a list is a container built on top of them — different design
question. Phase 3 will use placeholder list syntax where needed (e.g. `range(10)`,
`list<int>`) without fully committing to Phase 5's array internals yet. Revisit Phase 3's
loop syntax if Phase 5's eventual array design conflicts with the placeholder assumptions.

### Game-domain types
NOT built into the language core. `vec2`, `vec3`, `color`, `entity` etc. are defined at
the standard-library or hosting-application level, not the language spec itself.
Keeps the core language engine-agnostic (different engines have different coordinate
systems, perf requirements, etc.) and keeps the core spec lean.

```
import engine.types

vec3 position = vec3(0, 0, 0)
entity player = engine.spawn("player.prefab")
```

---

## Phase 3 — Control Flow ✅ LOCKED

### If / else
No parens required around the condition — the `{ }` block already delimits scope
clearly, so parens would be redundant visual noise (matches Swift/Rust's reasoning for
dropping required parens once braces exist).

```
if condition {

} else if condition {

} else {

}
```
`else if`, not `elif` — Taipan is brace-delimited, C-family-signaling syntax; `elif` is
a Python keyword and mixing it in sends the same "which language is this" confusion
flagged earlier with the `#` vs `//` comment decision. Matches the original Phase-1
code sample.

### For loops — two distinct forms, never mixed
Resolves the original broken hybrid (`for (int i) in range(10)`, which mixed C-style
declaration with Python-style iteration incoherently).

**Range/collection iteration (the common case):**
```
for int i in range(10) {
    z += i
}

for entity e in enemies {
    e.takeDamage(5)
}
```
No parens for a single loop variable — nothing to group, parens would be pure noise.

**Destructuring multiple values (e.g. index + item, key + value):**
```
for (int index, entity e) in enemies {
    engine.print(index)
}
```
Parens required here specifically because they group multiple values being pulled from
one iterated pair/tuple — this is the one case where they earn their keep.

**Classic C-style three-part loop (escape hatch for custom step/condition logic):**
```
for (int i = 0; i < 10; i++) {
    z += i
}
```
Kept available for cases `range()` can't express cleanly (counting backwards, custom
step sizes, multiple independent counters with different update rules).

Rule of thumb: parens appear only when there's a C-style three-part clause, or when
destructuring multiple values out of one iterated item. A single loop variable never
gets wrapped in parens.

(Note: full `range()` and list/array semantics depend on Phase 5 — see Phase 2's
"Dependency note" on lists/arrays.)

### While loops
```
while a < 50 {
    a += 1
}
```
No parens, consistent with `if`/`for`.

### Match (not switch)
`match` chosen over a classic C-style `switch` specifically to avoid the fallthrough
footgun (forgetting `break` silently falling into the next case) — a well-documented,
real-world bug source in C-family languages. Pairs well with the planned `enum` type
(Phase 5) for exhaustive state-machine handling (player states, AI states, etc.).

```
match playerState {
    "idle" -> { engine.print("standing still") }
    "walking" -> { engine.print("moving") }
    "attacking" -> { engine.print("swinging!") }
    else -> { engine.print("unknown state") }
}
```

### Break / continue
Standard behavior, standard keywords — `break` exits the loop entirely, `continue`
skips to the next iteration.

### Error handling: try/catch
Chosen over Result-types or silent-crash specifically for familiarity — lowers the
barrier to entry for programmers coming from Java/C++/Python/JS, which covers the
large majority of likely Taipan adopters.

```
try {
    int result = riskyOperation()
} catch error {
    engine.print("something broke: " + error)
}
```

## Phase 4 — Functions, deeper ✅ LOCKED

### Default parameters
```
func heal(entity target, int amount = 10) {
    target.hp += amount
}

heal(player)           // amount defaults to 10
heal(player, 25)       // explicit override
```
Default-valued parameters must come after all required parameters, same rule as most
C-family/Python-family languages — avoids ambiguity about which argument maps to which
parameter.

### Multiple return values
```
func getPlayerStats() -> (int, int) {
    return hp, mana
}

int health, int mana = getPlayerStats()
```
Extends the existing `->` arrow return syntax (locked in Phase 1) to a parenthesized
type list rather than introducing a new keyword. Each return slot keeps its own real,
static type (not forced into `any`) — rejected an earlier `multi<any,any,any>` idea
because it conflated "any" as a placeholder-for-some-type with "any" as the dynamic
type from Phase 2, and rejected a `multiout` keyword-flag idea because it would have
given callers no compile-time knowledge of how many values or what types come back,
undermining Phase 2's static typing guarantees.

### Variadic / optional args
```
func logMessage(str prefix, any... args) {
    for any a in args {
        engine.print(prefix + a)
    }
}

logMessage("DEBUG:", "hp=", 100, "mana=", 50)
```
`...` marks a variadic parameter, must be the last parameter in the list. Typed `any`
since variadic args are typically mixed-type in practice (log calls, event payloads).

### First-class functions
```
func onDeath(func callback) {
    callback()
}

func explode() {
    engine.print("boom")
}

onDeath(explode)
```
Functions are values — can be stored in variables and passed as arguments. Required for
callback-style game code (death handlers, trigger zones, UI events).

### Lambdas / anonymous functions
```
entity.onHit = (int damage) -> {
    engine.print("took " + damage + " damage")
}

enemies.forEach((entity e) -> {
    e.update()
})
```
Reuses the same `->` arrow already established for function return types, rather than
inventing separate lambda-specific syntax — keeps the language's visual vocabulary small.

### Coroutines
Confirmed as a required, first-class feature (differentiator vs Lua established back in
early project planning). Cooperative, single-threaded, NOT multithreading/multiprocessing,
and NOT true parallelism.

```
coroutine enemyPatrol(entity e) {
    while true {
        e.moveTo(pointA)
        wait(2.0)
        e.moveTo(pointB)
        wait(2.0)
    }
}

start enemyPatrol(goblin)
```
- `coroutine` declares one, parallel in spirit to `func` but pausable.
- `wait(seconds)` yields control back to the engine/VM without blocking, resumes
  automatically from the same point once the duration elapses.
- `start` launches a coroutine as a running instance.

**Threading model clarification (important, resolves a real point of confusion):**
Coroutines are cooperative task-switching on a SINGLE thread — conceptually close to
Python's asyncio (not parallelism), distinct from true OS-level multithreading or
multiprocessing (no race conditions, no locks needed, since nothing ever runs
simultaneously — coroutines just take turns, switching only when a coroutine itself
chooses to pause via `wait()`). This mirrors real Lua's `coroutine.yield()` /
`coroutine.resume()` mechanism, which is exactly why Lua became dominant for patrol
routes, cutscene sequencing, and delayed triggers in shipped games.

**VM-level threading (separate question, affects Phase 7 later):**
A single Taipan VM instance is single-threaded by design — this keeps the language
itself simple and safe for scripters (no thread-safety footguns, consistent with the
no-raw-pointers/managed-memory anti-Krait philosophy). True multi-core usage, if
needed, is an ENGINE-level decision: the host engine may run multiple independent
Taipan VM instances across different threads (e.g. one per entity/system), each
internally single-threaded and simple. This mirrors how Lua is actually used in
production game engines today. Flagged as a dependency for Phase 7 (Engine Integration):
how the host engine spins up and manages multiple VM instances is a real design
question to resolve there, not here.

## Phase 5 — Data Structures ✅ LOCKED

### Classes — no classical inheritance
Classes support fields and methods, but deliberately have NO classical inheritance
(no `extends`). Composition and interfaces are used instead of inheritance hierarchies.
Rationale: inheritance chains are a well-documented source of fragile game code (the
"fragile base class problem" — changing a parent breaks unrelated children); modern
game architectures (ECS and composition-based designs) generally favor composition
anyway, and this fits a game-scripting-focused language better than classic OOP.

### Access modifiers: `public` / `private` only — NO `protected`
`protected` is deliberately excluded. It exists specifically to control access across
an inheritance hierarchy ("visible to subclasses, not to outside code") — but Taipan
has no classical inheritance, so there is no subclass relationship for `protected` to
apply to. (Matches Go's reasoning for also omitting `protected`.)

Per-member explicit keyword style (not C++-style `public:`/`private:` section labels —
section labels were considered and rejected: in a long class, a member's visibility
becomes dependent on scrolling up to find which section label it falls under, which
works against Taipan's "explicit over implicit" philosophy already established via
static types, `?` nullable markers, and enforced `const`).

Default visibility when the keyword is omitted: **`public`** — matches the established
pattern of not forcing ceremony on the common case (same reasoning as omitting `->` for
no-return functions in Phase 1). `private` is opt-in, for when something specifically
needs to be locked down.

```
class Enemy {
    int hp = 100                   // no keyword = public by default
    private str internalName = "goblin_01"

    func takeDamage(int amount) {  // no keyword = public by default
        hp -= amount
        logDamage(amount)
    }

    private func logDamage(int amount) {
        engine.print("took " + amount)
    }
}

Enemy goblin = Enemy()
goblin.takeDamage(10)
```

### Interfaces
Composition-friendly alternative to inheritance — define a behavior contract without
forcing a shared base class.

```
interface Damageable {
    func takeDamage(int amount)
}

class Enemy implements Damageable {
    int hp = 100
    func takeDamage(int amount) {
        hp -= amount
    }
}
```
A `Player` and an `Enemy` can both `implement Damageable` without needing a common
parent class.

### Arrays / Lists
```
list<int> numbers = [1, 2, 3, 4]
list<entity> enemies = []

numbers.add(5)
int first = numbers[0]
int count = numbers.length
```
Dynamic/growable by default, matching scripting-language expectations (Lua tables,
Python lists). `list<type>` reuses the angle-bracket generic-style syntax. `[ ]` for
indexing, standard.

**Resolves the dependency notes from Phase 2 and Phase 3:** `range(10)` and `for ... in`
loops now have a concrete backing concept.

### Dictionaries / Maps
```
dict<str, int> scores = {"player1": 100, "player2": 85}

scores["player1"] = 150
int score = scores["player2"]

for (str key, int value) in scores {
    engine.print(key + ": " + value)
}
```
`dict<keyType, valueType>`, consistent with `list<type>`. Note: the `for` loop here
uses parens around `(str key, int value)` per Phase 3's rule — parens are required
when destructuring multiple values out of one iterated item (a key-value pair here).

### Enums
```
enum PlayerState {
    IDLE,
    WALKING,
    ATTACKING,
    DEAD
}

PlayerState state = PlayerState.IDLE

match state {
    PlayerState.IDLE -> { engine.print("standing still") }
    PlayerState.WALKING -> { engine.print("moving") }
    else -> { engine.print("other") }
}
```
Pairs directly with Phase 3's `match` statement — the intended use case for both
(player states, AI states, game-phase state machines).

## Phase 6 — Memory Model ✅ LOCKED

### No raw pointers (carried over from early project principles)
Already established as a core anti-Krait principle (see top of this document) —
scripters never see or manipulate raw pointers. This phase defines HOW the VM manages
memory underneath that guarantee.

### Primitives — VM fast-path storage, no GC overhead
```
int x = 5
float y = 3.14
bool flag = true
```
`int`, `float`, `bool` (and ExtType's fixed-width numeric types) live in VM-managed
fast storage (register/stack-slot style within the VM's execution frame) — NOT on the
GC-tracked heap. Mirrors how real Lua's VM handles numbers/booleans internally: no
heap allocation, no GC pressure, effectively free to create and destroy. Matters for
per-frame game math (damage calc, position updates, timers) that needs to be cheap.

### Complex/reference types — GC-managed heap
```
entity player = engine.spawn("player.prefab")
list<int> numbers = [1, 2, 3]
Enemy goblin = Enemy()
```
Classes, `list<>`, and `dict<>` are reference types: heap-allocated, GC-managed, passed
by reference (not copied) when passed to functions or assigned to another variable.

```
func modifyList(list<int> nums) {
    nums.add(99)    // mutates the SAME list the caller holds, not a copy
}

list<int> myList = [1, 2, 3]
modifyList(myList)
// myList is now [1, 2, 3, 99]
```

### Strings — heap-allocated, reference type, but IMMUTABLE
```
str hello = "hello"
str world = "world"
hello += world   // produces a NEW string; does not mutate the original in place
```
Immutability avoids a real bug class: two variables accidentally aliasing the same
mutable string, where modifying one unexpectedly affects the other. Matches how
Python, Java, and C# all treat strings.

### Garbage collection: incremental/generational (Option B), chosen deliberately over a
### simpler v1 path
Decision: build directly for incremental/generational GC rather than starting with a
simpler mark-and-sweep collector and upgrading later. This is a deliberate departure
from the "ship the simple version first" pattern used elsewhere in this spec (e.g. the
JIT deferral in Phase 8) — made consciously, with the real trade-off acknowledged below,
because implementation is being treated as a separate concern (Claude Code / future
build phase) from design (this document aiming for the best end-state).

**What this means and why it's harder (recorded for whoever implements it):**
- *Incremental*: GC work is spread in small increments across multiple frames instead
  of one large stop-the-world pause — avoids visible game stutter, but requires
  correctly handling objects created WHILE a collection cycle is still in progress
  (the "write barrier" problem — a well-known source of subtle bugs if implemented
  incorrectly).
- *Generational*: objects are split into "young" (recently created) and "old"
  (long-lived) generations and collected at different frequencies, since most objects
  in typical programs die young (temp strings, short-lived calculation results,
  transient list entries). Requires correctly promoting surviving young objects to the
  old generation.
- This mirrors real Lua's actual production GC design (Lua's GC is incremental,
  specifically to avoid frame stutters) — Taipan is targeting the proven end-state
  directly rather than Lua's own historical, more gradual path there.

**Explicitly rejected: reference counting (Option C).** Game object graphs commonly
contain natural reference cycles (an Enemy referencing its current Target while the
Target's AI references nearby Enemies back; parent/child Entity relationships) — pure
reference counting cannot reclaim cycles without extra cycle-detection machinery bolted
on, which erases most of its simplicity advantage for this specific domain.

## Phase 7 — Engine Integration ✅ LOCKED

### Naming convention: lowercase singleton/module vs UPPERCASE instantiable type
Established as an explicit rule (previously used instinctively but undocumented):
- **lowercase** (`engine`) — a global singleton/module access point, always directly
  accessible, never instantiated. The engine's native API surface lives here.
- **UPPERCASE** (`Player`, `Enemy`, `World`) — an actual type, native (C++-exposed) or
  authored in pure Taipan (Phase 5 classes) alike, used to declare variables/instances.

```
entity player = engine.spawn("player.prefab")
engine.print("hello")

Player player = engine.getWorld().getPlayer()
player.doSomething()

World world = engine.getWorld()
```
This unifies native-exposed types and Taipan-authored classes under one convention — a
script author can't tell (and doesn't need to know) whether `Player` is implemented in
C++ or pure Taipan. The native-binding layer stays invisible to the script author,
which is the actual goal of good engine integration.

### Embedding API (C++ side)
Mirrors Lua's proven minimal-surface-area approach (`lua_State*` + a small set of
push/pop-style functions) rather than inventing something more elaborate. One opaque
handle per VM instance, directly supporting Phase 4's multi-VM-per-thread model for
multi-core usage.

```cpp
TaipanState* state = taipan_create();

taipan_load_file(state, "enemy_ai.tp");
taipan_run(state);

taipan_call_function(state, "onPlayerDeath");

taipan_destroy(state);
```
Goal: an engine programmer should be able to go from "never heard of Taipan" to
"calling a script function" in under an hour, from a single header file.

### Calling C++ from Taipan (native function registration)
Every native function follows one fixed signature —
`TaipanValue someFunc(TaipanArgs args)` — so the VM can treat all native functions
uniformly regardless of what they actually do.

```cpp
TaipanValue native_spawn(TaipanArgs args) {
    std::string prefabName = args.getString(0);
    int x = args.getInt(1);
    int y = args.getInt(2);

    Entity* e = World::SpawnAt(prefabName, x, y);
    return TaipanValue::fromEntity(e);
}

taipan_register_function(state, "engine.spawn", native_spawn);
```
```
entity player = engine.spawn("player.prefab", 0, 0)
```

### Exposing native classes to Taipan
Builder-style chained registration (matches proven real-world binding libraries like
sol2 for Lua and pybind11 for Python — avoids hand-writing marshalling code per
field/method).

```cpp
taipan_register_class(state, "Entity")
    .property("x", &Entity::x)
    .property("y", &Entity::y)
    .property("hp", &Entity::hp)
    .method("takeDamage", &Entity::takeDamage);
```
```
entity goblin = engine.spawn("goblin.prefab")
goblin.takeDamage(10)
engine.print(goblin.hp)
```
Chained calls work naturally with no special mechanism: since every native
method/function returns a `TaipanValue` that may itself wrap a handle to another
native object, calling another method on that return value is just an ordinary method
call — chaining falls out of the design rather than needing dedicated support.
```
Player player = engine.getWorld().getPlayer()
player.doSomething()
```

### Reading/writing native-side variables from C++
Generic `TaipanValue` representation plus typed getters/setters, mirroring Lua's
`lua_getglobal`/`lua_tointeger`-style C API. Type is checked before trusting a value
(handles the case where a script set a global to an unexpected type).

```cpp
TaipanValue v = taipan_get_global(state, "playerHealth");
if (v.isInt()) {
    int health = v.asInt();
}

taipan_set_global_int(state, "playerHealth", 150);
```

### Native object ownership: engine owns it, Taipan holds reference handles
Decision: the host C++ engine (and its existing entity/component/pooling systems)
retains real ownership of native objects. Taipan-side values like `entity` or `Player`
are lightweight reference handles pointing back into the engine's own storage, not
objects owned/managed by Taipan's GC.

```cpp
// C++ continues to fully own and manage real Entity objects.
// Taipan only holds safe handles, e.g. conceptually { handle_id: 42, type: "entity" }
```

Rationale: nearly every serious game engine already has its own memory/object
management (ECS, entity pools, UObject-style systems) — Taipan should reference into
that, not compete with or duplicate it. Matches how production Lua bindings actually
work (Lua "userdata" wraps a handle to a native object rather than owning it).

A handle whose underlying native object has been destroyed (entity died, got pooled)
must be detectable as stale — calling a method on a stale handle is a catchable error
via Phase 3's `try`/`catch`, not a crash.

### Hot-reload mechanics
```cpp
taipan_reload_file(state, "enemy_ai.tp");
```
On reload: re-parse/re-compile the changed script to fresh bytecode, swap in updated
function definitions, and **preserve existing object state** where possible (e.g. a
goblin mid-patrol via a running coroutine keeps its current position/state rather than
resetting) — otherwise hot-reload feels broken during actual iteration/testing. This is
made architecturally easier by Phase 6's memory model: since complex types are
GC/VM-managed rather than raw memory blobs, swapping function code without destroying
existing object instances is feasible by design, not a special case bolted on.

### Standalone runner (Taipan can run embedded OR standalone, like Lua)
Taipan ships its own reference "host" executable (`taipan myscript.tp` from a terminal),
alongside its primary identity as an embeddable game-scripting language — mirroring
exactly how real Lua works (`lua myscript.lua` standalone, or embedded in an
application). This is NOT Taipan becoming a general-purpose language competing with
Python — standalone mode exists for testing, tooling, and utility scripts, while
embedding-in-an-engine remains the primary, core use case.

Mechanically, this requires no new architecture: Phase 7's native-function-registration
model already means "whatever host embeds the VM decides what native functions are
available." The standalone runner is simply a specific host that registers a more
permissive set of built-ins (blocking I/O, unrestricted filesystem access) than a game
engine would — a game engine's host registers its own safer/sandboxed equivalents
instead, per the core/standalone split documented in Phase 9's standard library.

### Module/import system

**Three categories of "things you can reference," only one of which needs `import`:**

| Category | How it gets there | Needs `import`? |
|---|---|---|
| Standard library (`math`, `str`, `json`, `time`, core `file`) | Built into the VM itself, always present | No |
| Native engine bindings (`engine.*`, `physics.*`, `audio.*`, `Player`, `Entity`, etc.) | C++ host registers these at VM startup via `taipan_register_function`/`taipan_register_class` (see above) | No |
| User-authored `.tp` modules (`enemies.ai`, `player.controller`) | Taipan source files, parsed/compiled on demand | **Yes** |

`import` is reserved exclusively for "find and parse another `.tp` file on disk."
Standard library and native engine bindings are both already present in the VM before
any script starts running — regardless of how many separate namespaces a host engine
organizes its native bindings into (`engine.*`, `physics.*`, `audio.*` can all coexist,
none of them need `import`) — so neither category goes through the import system at all.

```
// Taipan script
import enemies.ai          // a real .tp file - needs import

enemies.ai.updateAll()
physics.raycast(origin, direction)   // native engine binding - no import needed
math.sqrt(16)                          // stdlib - no import needed
```

**Whole-module import only** — no Python-style `from X import Y` selective import.
Always `module.function()`, never a bare imported name. This is a deliberate
consistency choice: it matches how `engine.*`, `math.*`, `str.*` etc. have been written
throughout this entire spec, and it keeps the SOURCE of every call immediately visible
when scanning code — a bare `updateAll()` could be local, could be imported from
anywhere; `enemies.ai.updateAll()` can only mean one thing.

**Nested/dotted paths for disambiguation:**
```
import enemies.ai
import player.ai

enemies.ai.updateAll()
player.ai.handleInput()
```
Resolves naming collisions across folders (`enemies/ai.tp` vs `player/ai.tp`) — the
full dotted path is the module's identity, so no ambiguity is possible. A bare
`import ai` only resolves if exactly one `ai.tp` is reachable from the script root; a
genuine collision with no dotted path given is a compile-time error, not a silent guess.

**Aliasing:**
```
import enemies.ai as enemyAI
import player.ai as playerAI

enemyAI.updateAll()
playerAI.handleInput()
```
Supported for collision-avoidance or shortening long dotted paths.

**Circular imports are a compile-time error**, caught during the bytecode-generation
pass (Phase 8) — not a silent infinite loop or undefined behavior. Uses the Phase 9
Rust-style error format:
```
error: circular import detected
  --> enemy_ai.tp:2:1
   |
 2 | import player_controller
   |        ^^^^^^^^^^^^^^^^^ 'player_controller' imports 'enemy_ai', creating a cycle
```

Resolution root: relative to an engine-configured script root (a `scripts/` folder, or
a packed game archive — the host engine decides, same as the standalone runner
resolving relative to its own working directory when there's no engine attached).

## Phase 8 — Execution Strategy ✅ PARTIALLY LOCKED

### Execution model: Bytecode VM
Confirmed: Taipan compiles to bytecode, executed by a virtual machine — NOT a pure
tree-walking interpreter, and NOT a transpile-to-C++ / transpile-to-native approach.
This matches real Lua's actual architecture (Lua is not a pure interpreter either —
it compiles to bytecode internally), which is the proven, battle-tested model for
embedded game-scripting languages.

```
Taipan source (.tp file)
      |
Lexer -> Parser -> AST
      |
Type-checking pass (static types checked here, before execution)
      |
Bytecode generation
      |
VM executes bytecode
```

Rejected for v1:
- **Pure tree-walking interpreter** — simplest to build, but meaningfully slower than
  bytecode with no real benefit for a language targeting Lua-level performance.
- **Transpile to C++ / native binary** — was the original "Route A" idea from early
  planning, rejected because a real scripting language needs to load and run scripts
  without an ahead-of-time native compile step (hot-reload, fast iteration, no build
  step for designers/scripters).
- **JIT (e.g. LuaJIT-style)** — explicitly deferred, NOT part of v1. JIT compiler
  engineering is a multi-year, deeply specialized undertaking (register allocation,
  deoptimization, correctness edge cases) even for experienced teams. Revisit only
  after the bytecode VM is stable, shipped, and profiling shows a real need for it —
  mirrors how real Lua existed and thrived for years before LuaJIT appeared.

### Clarifying earlier terminology
Because Taipan is NOT ahead-of-time compiled to a native binary, "compile error" in
earlier parts of this doc (e.g. Phase 2 type errors) should be understood as: an error
caught during the type-checking / bytecode-generation pass, before the VM executes the
script — not a traditional C++-style separate build step. Scripts still load and run
fast, still support hot-reload; type safety is enforced at load-time, not deferred to
mid-execution like fully dynamic languages (Lua/Python) do.

### Bytecode caching ✅ LOCKED
Enabled by default. Generated bytecode is cached to a temp folder (mirrors Python's
`.pyc` approach) so subsequent loads of an unchanged script skip re-parsing. Can be
disabled. Cache invalidation ties naturally into Phase 7's hot-reload mechanism — a
changed source file on disk should invalidate/regenerate its cached bytecode rather
than serving a stale cache.

## Phase 9 — Tooling (IN PROGRESS)

### Standard library: core vs standalone-only split
Resolves a real design fork: Taipan is primarily a game-embedding language (like Lua
embedded in an application), but also runs standalone via its own reference runner (like
`lua myscript.lua`). Functions are split into two buckets based on whether they involve
BLOCKING execution — a blocking call is fine in a standalone script (nothing else is
depending on that thread staying responsive) but unacceptable inside a real-time game
loop (it would freeze the whole game, not just the calling script). See Phase 7's
"Standalone runner" section for the mechanism (different hosts register different
native functions).

**Core — always available, both embedded and standalone:**
```
print(any value)
f"interpolated {expression} string"     // f-strings, confirmed from the
                                           // original Phase-1 code sample,
                                           // formalized here

math.abs(x)
math.min(a, b)
math.max(a, b)
math.floor(x)
math.ceil(x)
math.round(x)
math.sqrt(x)
math.pow(base, exp)
math.random()            // 0.0-1.0
math.randomInt(min, max)

str.length(s)
str.toUpper(s)
str.toLower(s)
str.split(s, delimiter) -> list<str>
str.trim(s)
str.contains(s, substring) -> bool
str.replace(s, old, new)

toInt(value) -> int
toFloat(value) -> float
toStr(value) -> str
toBool(value) -> bool

json.parse(str) -> dict
json.stringify(dict) -> str

time.now() -> float      // timestamp; safe, non-blocking, instant
```
`print` and f-strings stay bare/global (no namespace prefix) since they're used too
constantly to tax with namespace friction — same reasoning as omitting `->` on
no-return functions in Phase 1. Everything else is namespaced (`math.*`, `str.*`) for
organization/discoverability.

**Standalone-only — registered by the standalone runner, NOT available when embedded
in a game engine:**
```
input(str prompt) -> str     // blocks waiting on terminal input - would freeze a
                               // real-time game loop, fine in a standalone script
time.sleep(float seconds)    // blocks the whole thread - the embedded-context
                               // equivalent is wait() inside a coroutine (Phase 4),
                               // which yields instead of blocking
file.read(path) -> str       // unrestricted filesystem access when standalone;
file.write(path, data)        // when embedded, an engine registers its OWN
                               // sandboxed file.* restricted to an engine-configured
                               // directory instead (see Phase 7 ownership/sandboxing)
```

**Explicitly excluded from Taipan's design, in both contexts:**
- Blocking network calls — would need an async/coroutine-friendly form if ever added,
  never a blocking one. Not designed yet; flagged as future work, not core v1.

**Deliberately NOT part of language-level design:** `engine.*` functions (`engine.spawn`,
`engine.print`, `engine.getWorld()`, etc.) are entirely host/game-engine-defined per
Phase 7, not part of Taipan's own standard library — different engines will expose
completely different `engine.*` surfaces.

### Error message format ✅ LOCKED
Rust-style formatting, not bare Python-traceback-style — chosen deliberately because
error message quality is a genuine, often-underrated adoption factor (Rust's compiler
is widely regarded as best-in-class specifically because of this). More work to
implement than a bare "error at line N", but treated the same way as Phase 6's GC
decision: design aims for the best end-state, implementation complexity is a build-phase
concern (Claude Code), not a reason to undershoot the design.

Required elements: file + exact line:column, the actual offending source line shown
inline (not just a line number the reader has to go look up), a visual pointer
(`^^^^^^^`) at the exact problematic token, and a plain-English explanation — not just
"type error" but specifically what was expected vs what was found. Where relevant,
back-reference earlier context (e.g. where a `const` was originally declared).

```
error: type mismatch
  --> game.tp:3:11
   |
 3 |     int x = "hello"
   |             ^^^^^^^ expected 'int', found 'str'

error: cannot assign to constant
  --> game.tp:8:5
   |
 8 |     MAX_HEALTH = 50
   |     ^^^^^^^^^^ 'MAX_HEALTH' was declared as const on line 2

error: possible null reference
  --> game.tp:12:5
   |
12 |     target.takeDamage(10)
   |     ^^^^^^ 'target' is type 'entity?' (nullable) - check for null before use

error: use of invalid entity handle
  --> game.tp:20:5
   |
20 |     goblin.takeDamage(5)
   |     ^^^^^^ 'goblin' was destroyed - this handle is no longer valid
```

Two-tier category system: `error:` for hard failures that block execution (type
mismatches, syntax errors, const violations), `warning:` for issues that don't block
execution but merit attention (unused variables, unreachable code after `return`).

### Debugger support, syntax highlighting, LSP — DEFERRED (post-runtime)
All three explicitly deferred until after the core runtime (Phase 8's bytecode VM) is
built and stable — same sequencing logic already used for JIT (Phase 8): there is
nothing to attach a debugger or language server to until a working runtime exists, so
designing these in detail now would be premature. Not a scope cut, just correct
ordering.

Noted for whoever picks this up later: a debugger (breakpoints, variable inspection,
step-through execution, call stack view) is likely to matter more for Taipan than for
some languages, given two of its own design choices — coroutines (Phase 4) that pause
and resume mid-execution are hard to reason about via print-statement debugging alone,
and timing-dependent game bugs in general benefit more from real breakpoint/step tooling
than from manual print-driven debugging.
