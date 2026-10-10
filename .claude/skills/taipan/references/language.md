# Writing Taipan scripts

Contents:
1. Mistakes to avoid
2. Syntax reference
3. Classes, interfaces, enums
4. Null safety
5. Coroutines
6. Modules
7. Standard library
8. Reading error messages

## 1. Mistakes to avoid

These come from habits formed in other languages. Each one is a compile error
or a silent bug in Taipan.

| Habit | Taipan |
|---|---|
| `x = 5`, `var x`, `let x` to declare | Every declaration states a type: `int x = 5`. Use `any` only when a value really changes type. No inference. |
| `elif` / `elsif` | `else if` |
| `and`, `or`, `not` | `&&`, `\|\|`, `!` |
| `cond ? a : b` | No ternary. Use `if`/`else`, or `??` for null defaults. |
| `for i in range(10)` | The loop variable needs a type: `for int i in range(10)` |
| `for i, x in enumerate(xs)` | `for (int i, T x) in xs` (parentheses only when destructuring) |
| `def` / `function` / `fn` | `func name(int a, str b) -> int { }`. No `->` means it returns nothing. |
| `# comment` | `// comment` or `/* */`. `#` is a syntax error. |
| `then` / `do` / `end` | Braces only. Indentation has no meaning. |
| Splitting an expression across lines | A newline ends the statement, even after `+`. Wrap it in `( )` or end the line with `\`. |
| Calling a function at the top level above its `func` | Top-level code runs in order and a `func` is defined when its line runs. Inside function bodies order doesn't matter, but top-level calls must come after the definition. `--check` can't catch this; it fails at runtime. |
| `class B extends A` | No inheritance. Use `implements` with an interface, or composition. |
| Reading `obj.field` right after `if obj.field != null` | Narrowing only works on plain variables. Copy into a local first. |
| `Enemy e` with no value | A class-typed variable needs a value or a nullable type: `Enemy? e` |
| `7 / 2` expecting `3.5` | `int / int` is integer division (3). Write `7.0 / 2`. |
| `wait(1)` in a normal function | `wait` only works inside a coroutine launched with `start`. |
| `d["missing"]` expecting null | A missing dict key throws. Use `d.get(k, default)` or `d.has(k)`. |
| `from mod import f` | Import the whole module and call `mod.f()`. |

## 2. Syntax reference

```taipan
// variables and constants
int hp = 100
float speed            // zero value 0.0 (int 0, bool false, str "", lists/dicts empty)
str name = "goblin"
bool alive = true
const int MAX_HEALTH = 100        // reassigning is a compile error
any loose = 1
loose = "text"                    // any is the only dynamic type
list<int> nums = [1, 2, 3]
dict<str, int> scores = {"a": 1, "b": 2}
list<str> empty = []              // an empty literal needs a declared element type

// statements end at newlines; ';' separates statements on one line
int a = 1; int b = 2
int total = a + \
    b                             // a trailing backslash continues the line
int sum = math.max(
    a,
    b
)                                 // newlines inside ( ) [ ] { } literals are ignored
str label = f"hp {hp} / {MAX_HEALTH}, braces {{ }}"   // f-strings

// functions
func damage(int base, float mult = 1.5) -> int {
    return base * mult            // float -> int truncates (warning)
}
func log(str prefix, any... args) {      // variadic: args is list<any>
    for any v in args { print(prefix, v) }
}
func stats() -> (int, int) {             // multiple return values
    return hp, 50
}
int h, int m = stats()

// functions as values and lambdas
func apply(func f, int x) -> any {       // `func` = any callable, untyped
    return f(x)
}
int doubled = toInt(apply((int n) -> n * 2, 21))  // calling a func value returns any: convert it
list<any> squares = nums.map((int n) -> n * n)
nums.forEach((int n) -> {
    print(n)
})

// control flow
if hp <= 0 {
    print("dead")
} else if hp < 20 {
    print("low")
} else {
    print("ok")
}
for int i in range(10) { }               // range(end), range(start, end), range(start, end, step)
for (int i, int n) in nums { }           // index + item
for (str k, int v) in scores { }         // key + value
for (int i = 10; i > 0; i -= 2) { }      // C-style
while hp > 0 { hp -= 10 }
match name {                             // no fallthrough
    "goblin", "orc" -> print("monster")
    "hero" -> { print("player") }
    else -> { print("unknown") }
}
try {
    throw "bad thing"                    // any value can be thrown
} catch err {
    print("caught: " + err)              // runtime errors arrive as their message (str)
}
```

Operators:
- arithmetic: `+ - * / %`
- comparison: `== != < <= > >=`
- logical: `&& || !`
- bitwise: `& | ^ ~ << >>`
- null coalescing: `??`
- assignment: `+= -= *= /= %=`, and `x++` / `x--` as statements

`str + anything` converts the other side to text.

Strings are immutable and indexed by byte. `s.length` is a property;
`list.length` and `dict.length` are too.

## 3. Classes, interfaces, enums

```taipan
interface Damageable {
    func takeDamage(int amount)
}

class Enemy implements Damageable {
    int hp = 100                         // members are public unless marked private
    private str id = "e1"
    Enemy? target                        // class-typed fields: nullable or initialized
    func onHit                           // a field holding a function (starts null)

    func init(int startHp = 100) {       // optional constructor: Enemy(), Enemy(50)
        hp = startHp
    }

    func takeDamage(int amount) {
        hp -= amount                     // members need no `this.` (it is allowed)
        log(amount)
    }

    private func log(int amount) {
        print(id + " took " + amount)
    }
}

enum State { IDLE, RUNNING, DEAD }
State s = State.IDLE                     // s.name == "IDLE", s.index == 0
match s {                                // missing enum members without `else` -> warning
    State.IDLE -> print("idle")
    State.RUNNING, State.DEAD -> print("busy")
}

Enemy e = Enemy(80)
list<Damageable> things = [e]            // a class value fits its interface type
```

Classes, lists and dicts are passed by reference.

## 4. Null safety

(The examples in sections 4 and 5 reuse the `Enemy` class from section 3.)

Only `T?` types may hold `null`. Using a nullable value as non-null is a
compile error until you check it:

```taipan
func find(str name) -> Enemy? {
    if name == "" { return null }
    return Enemy(10)
}

func maybeBonus() -> int? { return null }

Enemy? found = find("orc")
if found != null {
    found.takeDamage(1)                  // narrowed to Enemy inside the check
}
if found == null { return }              // an early exit narrows the rest of the block
found.takeDamage(1)
int bonus = maybeBonus() ?? 0            // `??` supplies a default
```

Narrowing applies to plain variables only. For `obj.field`, copy it into a
local first: `Enemy? t = e.target`, then `if t != null { ... }`.

## 5. Coroutines

Coroutines are cooperative and single-threaded. They are resumed by the host
every frame (`taipan_update`), or by real time in the standalone runner.

```taipan
coroutine patrol(Enemy e) {
    while e.hp > 0 {
        e.takeDamage(1)
        wait(0.5)                        // yield for 0.5 s; wait() = until the next frame
    }
}
any task = start patrol(Enemy(3))        // runs until its first wait, then returns a handle
// task.isRunning(), task.stop()
```

A normal function called from a coroutine may also call `wait`. `wait` inside
a callback that native code invokes (such as the lambda of `forEach`) is a
runtime error.

## 6. Modules

```taipan
import enemies.ai              // <script root>/enemies/ai.tp
import player.ai as playerAI   // alias
enemies.ai.updateAll()         // always module.member
enemies.ai.Goblin g = enemies.ai.Goblin()   // types from modules use the dotted name
```

- The script root is the main script's folder, unless the host sets it.
- A bare `import helpers` must match exactly one `helpers.tp` under the root.
- Circular imports are compile errors.
- The standard library and engine natives never need `import`.
- A module's top-level code runs once, on first import.

## 7. Standard library

Always available:

- `print(a, b, ...)`
- `range`, `toInt`, `toFloat`, `toStr`, `toBool`, `typeOf`
- `math.`: `abs min max floor ceil round sqrt pow sin cos tan atan2 clamp lerp random randomInt seed pi inf`
- `str.`: `length toUpper toLower split trim contains startsWith endsWith indexOf replace sub repeat`. These also work as methods: `s.toUpper()`, `s.split(",")`.
- `json.parse(str) -> any`, `json.stringify(any) -> str`
- `time.now()`, `time.clock()`, `time.scriptTime()`

List methods:
- `add insert removeAt remove pop contains indexOf clear`
- `forEach map filter sort reverse slice join copy isEmpty`
- `length`

Dict methods:
- `keys values has contains remove get clear copy isEmpty`
- `length`

Standalone runner only: `input`, `time.sleep`, `file.read`, `file.write`,
`file.append`, `file.exists`. Inside an engine these are absent by design,
because they block.

`enable ExtType` at the top of a file unlocks `int8` to `int64`, `uint8` to
`uint64` (`byte` = `uint8`), `f32` and `f64`. Out-of-range literals are compile
errors, and stored values wrap.

## 8. Reading error messages

```
error: possible null reference
  --> game.tp:12:5
   |
12 |     target.takeDamage(10)
   |     ^^^^^^ 'target' is type 'Enemy?' (nullable) - check for null before use
```

The title names the problem. The caret marks the exact token. The label says
what was expected and what was found. Fix the first error first: later ones
are often consequences of it.

Warnings don't block running, but they usually point at real problems:
- unused variables (prefix with `_` if intended)
- unreachable code
- non-exhaustive enum matches
- truncating float to int
