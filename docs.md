# The Taipan Programming Guide

Taipan is a scripting language for games: brace blocks, Python-like
readability, static types, null safety, built-in coroutines, hot reload. This
guide teaches the whole language from the first line to complete programs.
Every example is a working program you can paste into a `.tp` file and run.

> Building the interpreter and embedding it in an engine are covered in
> [README.md](README.md). The language design and its reasons are in
> [taipandesign.md](taipandesign.md).

## Contents

1. [Getting started](#1-getting-started)
2. [Lexical structure](#2-lexical-structure)
3. [Variables and types](#3-variables-and-types)
4. [Operators](#4-operators)
5. [Strings](#5-strings)
6. [Lists](#6-lists)
7. [Dicts](#7-dicts)
8. [Control flow](#8-control-flow)
9. [Functions](#9-functions)
10. [Null safety](#10-null-safety)
11. [Classes](#11-classes)
12. [Interfaces](#12-interfaces)
13. [Enums](#13-enums)
14. [Error handling](#14-error-handling)
15. [Coroutines](#15-coroutines)
16. [Modules and imports](#16-modules-and-imports)
17. [Extended number types](#17-extended-number-types)
18. [Standard library reference](#18-standard-library-reference)
19. [Reading error messages](#19-reading-error-messages)
20. [Scripting a game engine](#20-scripting-a-game-engine)
21. [Style guide](#21-style-guide)
22. [A complete program](#22-a-complete-program)
23. [Quick reference](#23-quick-reference)

---

## 1. Getting started

Create `hello.tp`:

```taipan
str name = "Taipan"
print("Hello from " + name + "!")
print(f"2 + 3 = {2 + 3}")
```

Run it:

```
taipan hello.tp
```

```
Hello from Taipan!
2 + 3 = 5
```

Useful commands:

| Command | What it does |
|---|---|
| `taipan file.tp` | Check and run a script. Running coroutines keep going until they finish. |
| `taipan --check file.tp` | Type-check only, without running. |
| `taipan --no-warnings file.tp` | Hide warnings. |
| `taipan --disasm file.tp` | Show the bytecode the compiler produced. |
| `taipan --help` | All options. |

A script runs from top to bottom. There is no `main` function: the top level
of the file is the program.

Before running anything, Taipan checks the whole file: types, null safety,
constants, missing returns, unknown names. A script with an error does not
start at all. You get a message pointing at the exact spot instead.

---

## 2. Lexical structure

### Comments

```taipan
// a line comment

/* a block comment
   can span lines */

int x = 1 // comments can follow code
print(x)
```

`#` is not a comment: it is reserved for future directives and is a syntax
error.

### Statements and newlines

A statement ends at the end of its line. Semicolons are not needed, but you
may use `;` to put several statements on one line.

```taipan
int a = 1
int b = 2; int c = 3
print(a, b, c)
```

A newline inside `( )`, `[ ]` or a dict literal `{ }` does not end the
statement, so long calls and literals can span lines freely:

```taipan
int biggest = math.max(
    10,
    20
)
list<int> primes = [
    2, 3, 5,
    7, 11,
]
print(biggest, primes)
```

Anywhere else, end the line with `\` to continue it:

```taipan
int total = 1 + 2 + \
            3 + 4
print(total)
```

A newline ends a statement even right after an operator (`int t = 1 +` and
then `2` on the next line is an error). Use parentheses or `\`.

### Blocks and indentation

Braces `{ }` define every block. Indentation has no meaning to the compiler,
so all of these are the same:

```taipan
bool ready = true
if ready {
    print("one")
}

if ready
{
    print("two")
}

if ready { print("three") }
```

### Identifiers

Names start with a letter or `_`, followed by letters, digits or `_`. They are
case-sensitive (`hp` and `HP` are different names).

### Keywords

```
func  coroutine  start  return  if  else  for  in  while  match  break  continue
try  catch  throw  class  interface  implements  enum  import  as  enable
const  private  public  true  false  null  this
any  int  float  bool  str  list  dict
```

`print`, `range`, `wait` and the other built-ins are ordinary names, not
keywords. `static` and `operator` have their special meaning only at the
start of a class member, so they stay usable as ordinary names elsewhere.

### Literals

```taipan
int decimal = 1_000_000          // underscores are ignored, for readability
int hex = 0xFF
int binary = 0b1010
float f = 3.14
float big = 1.5e3                // exponent form is always a float
bool yes = true
str s1 = "double quotes"
str s2 = 'single quotes'
str escaped = "tab:\t quote:\" newline:\n backslash:\\ hex:\x41"
str interpolated = f"{decimal} is a million"
print(decimal, hex, binary, f, big, yes, s1, s2)
print(escaped)
print(interpolated)
```

A float needs a digit on both sides of the point: write `0.5` and `5.0`, not
`.5` or `5.`.

String escapes: `\n \t \r \0 \\ \" \' \{ \} \xHH`. Strings must fit on one
line.

---

## 3. Variables and types

### Declarations

Every variable is declared with its type. There is no `var` or `let`, and no
type inference: the type is always written, so it is always visible.

```taipan
int hp = 100
float speed = 4.5
bool alive = true
str name = "goblin"
print(hp, speed, alive, name)
```

Once declared, a variable keeps its type. Assigning a value of another type is
a compile error.

### Zero values

A variable declared without a value starts at the zero value of its type, so
there are no uninitialized variables:

```taipan
int i            // 0
float f          // 0.0
bool b           // false
str s            // ""
list<int> xs     // []
dict<str, int> d // {}
int? maybe       // null (nullable types start as null)
print(i, f, b, s, xs, d, maybe)
```

Class-typed variables have no zero value. Give them a value, or make them
nullable (`Enemy? e`). Enums start at their first member.

### Constants

`const` makes a variable read-only. Assigning to it again is a compile error.
UPPER_CASE names are the convention.

```taipan
const int MAX_HEALTH = 100
const float GRAVITY = 9.8
print(MAX_HEALTH, GRAVITY)
```

### The built-in types

| Type | Values | Notes |
|---|---|---|
| `int` | 64-bit signed integers | Overflow wraps around |
| `float` | 64-bit floating point | |
| `bool` | `true`, `false` | |
| `str` | Text (UTF-8) | Immutable |
| `list<T>` | Ordered, growable list of `T` | Reference type |
| `dict<K, V>` | Key/value map, keeps insertion order | Reference type |
| `func` | Any function value | Untyped; see Functions |
| `any` | Anything | The only dynamically typed type |
| `T?` | `T` or `null` | See Null safety |

`int`, `float` and `bool` are stored directly (no allocation). Strings, lists,
dicts and objects live on the heap and are managed by the garbage collector.

### `any`: opting out of static types

A variable declared `any` can hold any value and change type freely:

```taipan
any thing = 1
thing = "now a string"
thing = [1, 2, 3]
print(thing, typeOf(thing))
```

Storing an `any` into a typed variable inserts a runtime check. If the value
has the wrong type, you get a catchable `type mismatch` error at that point:

```taipan
any value = 42
int n = value            // checked when it runs: OK, value is an int
print(n + 1)
```

### Conversions

```taipan
int a = 7
float b = a              // int -> float: automatic
int c = 7.9              // float -> int: truncates (the compiler warns)
print(b, c)

print(toInt("42"), toInt("0x1F"), toInt(3.7))   // 42 31 3
print(toFloat("2.5"), toFloat(3))               // 2.5 3.0
print(toStr(12) + "!", toBool("true"), toBool(0))
print(typeOf(3.5), typeOf("x"), typeOf([1]))    // float str list
```

### Scope

A variable exists from its declaration to the end of the block it was
declared in. An inner block may declare a name that hides an outer one.
Declaring the same name twice in one block is an error.

```taipan
int level = 1
if level > 0 {
    int level = 99         // a new variable, only inside this block
    print(level)           // 99
}
print(level)               // 1
```

Variables declared at the top level of a file are globals, visible to every
function in the file.

---

## 4. Operators

### Arithmetic

```taipan
print(7 + 2, 7 - 2, 7 * 2)
print(7 / 2)        // 3   : int / int is integer division
print(7.0 / 2)      // 3.5 : any float makes it float division
print(7 % 3, -7 % 3) // 1 -1 : the sign follows the left operand (like C)
print(2.5 * 2)      // 5.0
print(-(3 + 4))     // -7
```

- Integer division or modulo by zero is a runtime error.
- Float division by zero gives `inf`.
- Integer overflow wraps around.

### Comparison and equality

```taipan
print(3 < 5, 3 <= 3, 5 > 7, 5 >= 5)
print(1 == 1.0)              // true: numbers compare by value
print("apple" < "banana")    // true: strings compare alphabetically (by byte)
print("a" != "b")
```

`==` compares numbers by value and strings by content. Lists, dicts and
objects are equal only if they are the same object - unless their class
defines `operator ==` (see [Operator overloading](#operator-overloading)).
Engine value types such as `Vector3` compare by content.

### Logical

```taipan
bool a = true
bool b = false
print(a && b, a || b, !a)
```

`&&` and `||` short-circuit: the right side is evaluated only if needed.
Conditions must be `bool`. Taipan has no "truthy" numbers or strings, so
`if 1 { }` is an error.

### Bitwise

```taipan
int flags = 0b1100
print(flags & 0b0100, flags | 0b0001, flags ^ 0b1111, ~0)
print(1 << 4, 256 >> 2)
```

### String joining

`+` with a string on either side converts the other side to text:

```taipan
int damage = 12
print("took " + damage + " damage")
print("ready: " + true)
```

### Null coalescing `??`

`a ?? b` gives `a` unless it is `null`, otherwise `b`:

```taipan
str? nickname = null
print(nickname ?? "anonymous")
```

### Assignment

```taipan
int score = 10
score += 5     // also -=  *=  /=  %=
score++        // add one (a statement, not an expression)
score--
print(score)
```

`++` and `--` are statements on their own line. `x = y++` is not valid.

### Precedence

From lowest to highest:

| Level | Operators |
|---|---|
| 1 | `??` |
| 2 | `\|\|` |
| 3 | `&&` |
| 4 | `==  !=` |
| 5 | `<  <=  >  >=` |
| 6 | `\|` |
| 7 | `^` |
| 8 | `&` |
| 9 | `<<  >>` |
| 10 | `+  -` |
| 11 | `*  /  %` |
| 12 | unary `-  !  ~` |
| 13 | call `f()`, index `a[i]`, member `a.b` |

Unlike C, bitwise operators bind tighter than comparisons, so
`flags & MASK == 0` means `(flags & MASK) == 0`. There is no ternary
`a ? b : c`: use `if`/`else` or `??`.

---

## 5. Strings

Strings are immutable: operations return new strings. Indexing and `length`
count bytes, which equal characters for ASCII text.

```taipan
str s = "Hello, World"
print(s.length)              // 12
print(s[0], s[-1])           // H d  (negative indexes count from the end)
print(s.toUpper(), s.toLower())
print(s.contains("World"), s.startsWith("Hell"), s.endsWith("!"))
print(s.indexOf("o"))        // 4
print(s.replace("World", "Taipan"))
print(s.sub(0, 5))           // Hello  (start, end)
print("  padded  ".trim() + "|")
print("a,b,c".split(","))    // ["a", "b", "c"]
print("ab".repeat(3))        // ababab
for str ch in "hey" {
    print(ch)
}
```

Every string function also exists in the `str` namespace, which takes the
string as its first argument: `str.toUpper(s)` is the same as `s.toUpper()`.

### f-strings

An `f` before the quote turns `{expression}` into its value. `{{` and `}}`
write literal braces.

```taipan
str player = "Ayla"
int hp = 72
print(f"{player} has {hp} hp ({hp * 100 / 120}% of max)")
print(f"braces: {{ and }}")
print(f'single quotes work too: {player.toUpper()}')
```

---

## 6. Lists

A `list<T>` is an ordered, growable list of values of type `T`.

```taipan
list<int> numbers = [10, 20, 30]
list<str> names = []                 // empty: the declared type says what goes in

numbers.add(40)                      // append
numbers.insert(0, 5)                 // insert at index
print(numbers, numbers.length)       // [5, 10, 20, 30, 40] 5
print(numbers[0], numbers[-1])       // 5 40
numbers[1] = 11                      // replace
int removed = numbers.removeAt(0)    // remove by index, returns the item
bool found = numbers.remove(30)      // remove the first matching value
print(removed, found, numbers)
print(numbers.contains(20), numbers.indexOf(20), numbers.isEmpty())
print(numbers.pop(), numbers)        // remove and return the last item
names.add("orc")
print(names)
```

An index outside the list is a runtime error. Negative indexes count from the
end (`-1` is the last item).

### Iterating

```taipan
list<str> enemies = ["goblin", "orc", "troll"]
for str e in enemies {
    print(e)
}
for (int i, str e) in enemies {      // index and item
    print(i, e)
}
```

### Sorting, slicing, joining

```taipan
list<int> xs = [5, 3, 9, 1]
xs.sort()                            // ascending, in place
print(xs)
xs.sort((int a, int b) -> a > b)     // with a "comes before" function
print(xs)
xs.reverse()
print(xs, xs.slice(1, 3), xs.slice(-2))
print(xs.join(", "))
list<int> copy = xs.copy()
copy.clear()
print(xs.length, copy.length)
```

### Functional helpers

```taipan
list<int> nums = [1, 2, 3, 4, 5, 6]
list<int> evens = nums.filter((int n) -> n % 2 == 0)
list<any> squares = nums.map((int n) -> n * n)
print(evens, squares)
nums.forEach((int n) -> {
    if n > 4 {
        print("big:", n)
    }
})
```

`map` returns `list<any>`, because the function's return type is not tracked.

### Lists are references

Assigning a list or passing it to a function does not copy it: both names
refer to the same list.

```taipan
func addBonus(list<int> scores) {
    scores.add(100)
}
list<int> mine = [1, 2]
list<int> same = mine
addBonus(same)
print(mine)          // [1, 2, 100]
print(mine.copy())   // use copy() when you need an independent list
```

---

## 7. Dicts

A `dict<K, V>` maps keys to values and remembers insertion order. Keys may be
`str`, `int`, `float`, `bool`, enums or objects.

```taipan
dict<str, int> scores = {"ayla": 120, "bram": 85}
dict<int, str> byId = {}

scores["cora"] = 99              // add or replace
print(scores["ayla"])            // 120
print(scores.length, scores.has("bram"), scores.get("dax", 0))
scores.remove("bram")
print(scores.keys(), scores.values())
byId[7] = "seven"
print(byId)
for (str name, int score) in scores {
    print(name + ": " + score)
}
for str name in scores {         // one variable: just the keys
    print(name)
}
```

Reading a missing key with `d[key]` is a runtime error (`key not found`). Use
`d.get(key, default)` or check `d.has(key)` first.

---

## 8. Control flow

### if / else if / else

Conditions need no parentheses, and must be `bool`.

```taipan
int hp = 35
if hp <= 0 {
    print("dead")
} else if hp < 50 {
    print("wounded")
} else {
    print("healthy")
}
```

Write `else if`, not `elif`.

### while

```taipan
int n = 1
while n < 100 {
    n *= 2
}
print(n)        // 128
```

### for ... in range

`range(end)`, `range(start, end)` and `range(start, end, step)` count from
`start` (default 0) up to, but not including, `end`. The loop variable always
has a type.

```taipan
for int i in range(3) {
    print("a", i)            // 0 1 2
}
for int i in range(2, 5) {
    print("b", i)            // 2 3 4
}
for int i in range(10, 0, -3) {
    print("c", i)            // 10 7 4 1
}
```

### for ... in a collection

```taipan
list<str> items = ["sword", "shield"]
dict<str, int> stock = {"potion": 3, "arrow": 20}
for str item in items { print(item) }
for (int i, str item) in items { print(i, item) }
for (str key, int count) in stock { print(key, count) }
for str ch in "abc" { print(ch) }
```

Parentheses are used only when the loop takes two values (index and item, or
key and value).

### C-style for

For custom steps or conditions:

```taipan
for (int i = 0; i < 10; i += 3) {
    print(i)
}
int k = 10
for (; k > 0; k -= 4) {          // each of the three parts may be left empty
    print("k", k)
}
```

The header holds one initializer, one condition and one step. Declare extra
variables before the loop.

### break and continue

```taipan
for int i in range(10) {
    if i == 2 { continue }   // skip to the next round
    if i == 5 { break }      // leave the loop
    print(i)                 // 0 1 3 4
}
```

### match

`match` compares a value against patterns. Exactly one arm runs: there is no
fallthrough and no `break` needed.

```taipan
str state = "walking"
match state {
    "idle" -> { print("standing still") }
    "walking", "running" -> print("moving")      // several patterns, one arm
    else -> { print("unknown state") }
}

int roll = 6
match roll {
    1 -> print("critical miss")
    6 -> print("critical hit")
    else -> print("normal")
}
```

- An arm's body is either a `{ block }` or a single statement.
- `else ->` catches everything else and must come last.
- With enums, the compiler warns when you forget a member and there is no
  `else` (see Enums).

---

## 9. Functions

### Declaring functions

```taipan
func greet(str name) {                     // no "->": returns nothing
    print("hi " + name)
}

func add(int a, int b) -> int {            // returns an int
    return a + b
}

greet("Ayla")
print(add(2, 3))
```

A function with `-> T` must return a `T` on every path; the compiler checks
this. A function without an arrow may use a bare `return` to leave early.

### Default parameters

Parameters with defaults must come after the required ones.

```taipan
func heal(str who, int amount = 10) {
    print(who + " heals " + amount)
}
heal("Ayla")          // amount = 10
heal("Bram", 25)
```

### Multiple return values

```taipan
func minMax(list<int> xs) -> (int, int) {
    int lo = xs[0]
    int hi = xs[0]
    for int x in xs {
        if x < lo { lo = x }
        if x > hi { hi = x }
    }
    return lo, hi
}

int low, int high = minMax([4, 9, 1, 7])
print(low, high)      // 1 9
```

### Variadic parameters

A last parameter written `T... name` collects any number of extra arguments
into a `list<T>`:

```taipan
func logAll(str prefix, any... values) {
    for any v in values {
        print(prefix, v)
    }
}
logAll("DEBUG:", "hp", 100, true)
```

### Recursion

```taipan
func fib(int n) -> int {
    if n < 2 { return n }
    return fib(n - 1) + fib(n - 2)
}
print(fib(20))        // 6765
```

### Definition order

Functions may call each other in any order inside their bodies. Top-level code
runs from top to bottom, though, and a function exists from the line where it
is defined. So call a function at the top level only below its definition:

```taipan
func early() -> int {
    return later() * 2      // fine: runs when early() is called
}
func later() -> int {
    return 21
}
print(early())               // fine: both are defined by now
```

Calling `print(early())` above the two definitions passes `--check` but fails
when it runs (`'early' is used before its declaration ran`).

### Functions as values

`func` is the type of any function value. Functions can be stored, passed and
returned. Calls through a `func` value are checked when they run, and return
`any`.

```taipan
func shout(str s) -> str {
    return s.toUpper() + "!"
}
func applyTwice(func f, str s) -> any {
    return f(f(s))
}
func handler = shout
print(handler("hey"))
print(applyTwice(shout, "go"))
```

### Lambdas

A lambda is a function written inline: `(parameters) -> body`.

```taipan
func double = (int x) -> x * 2                 // expression body
func describe = (str name, int hp) -> {        // block body
    print(name + ": " + hp)
}
func clampHp = (int hp) -> int {               // block body with a return type
    if hp < 0 { return 0 }
    return hp
}
func hello = () -> { print("no parameters") }

print(double(21))
describe("orc", 30)
print(clampHp(-5))
hello()
```

A lambda without `-> Type` before its block returns `any`.

### Closures

Functions remember the variables around them, even after the outer function
has returned:

```taipan
func makeCounter() -> func {
    int count = 0
    return () -> {
        count += 1
        return count
    }
}
func next = makeCounter()
next()
next()
print(next())         // 3
```

Each round of a `for ... in` loop has its own loop variable, so closures
created in the loop remember different values. A C-style `for` has a single
variable shared by all rounds.

### Local functions

Functions can be declared inside other functions and can call themselves:

```taipan
func factorialOf(int n) -> int {
    func fact(int k) -> int {
        if k <= 1 { return 1 }
        return k * fact(k - 1)
    }
    return fact(n)
}
print(factorialOf(5))     // 120
```

---

## 10. Null safety

`null` means "no value". Only nullable types, written with `?`, may hold it.
The compiler makes sure a nullable value is checked before it is used, so you
can't hit "attempt to index a nil value" at runtime.

```taipan
int x = 5            // never null
int? y = null        // may be null
y = 10
print(x, y)
```

### Checking before use

Inside `if v != null { }` the variable is treated as non-null:

```taipan
class Enemy {
    int hp = 30
}

func findEnemy(str name) -> Enemy? {
    if name == "orc" {
        return Enemy()
    }
    return null
}

Enemy? target = findEnemy("orc")
if target != null {
    print("found, hp", target.hp)      // OK: target is known to be non-null here
}
```

Using `target.hp` without the check is a compile error:

```text
error: possible null reference
  --> game.tp:15:7
   |
15 | print(target.hp)
   |       ^^^^^^ 'target' is type 'Enemy?' (nullable) - check for null before use
```

### Other ways to handle null

```taipan
func lookup(str key) -> int? {
    if key == "gold" { return 50 }
    return null
}

int gold = lookup("gold") ?? 0         // a default value
print(gold)

func spend(str key) {
    int? amount = lookup(key)
    if amount == null {
        print("nothing to spend")
        return                         // an early exit...
    }
    print("spending", amount + 1)      // ...makes the rest of the function non-null
}
spend("gold")
spend("gems")

int? a = lookup("gold")
if a != null && a > 10 {               // checks combine with && and ||
    print("rich")
}
```

### Rules worth knowing

- Narrowing works on plain variables. For `obj.field`, copy it into a local
  first: `Enemy? t = hero.target`, then check `t`.
- Assigning a possibly-null value to a variable cancels its narrowing.
- Comparing anything with `null` is always allowed.
- `any` may hold `null` without a `?`.

---

## 11. Classes

A class groups data (fields) and behaviour (methods).

```taipan
class Enemy {
    str name = "goblin"          // fields, with optional default values
    int hp = 100
    bool alerted                 // zero value: false

    func takeDamage(int amount) {
        hp -= amount             // fields are visible in methods without "this."
        if hp <= 0 {
            print(name + " is defeated")
        }
    }

    func isAlive() -> bool {
        return this.hp > 0       // "this." is allowed when you want to be explicit
    }
}

Enemy goblin = Enemy()
goblin.takeDamage(30)
print(goblin.hp, goblin.isAlive())
goblin.name = "big goblin"
print(goblin)                    // Enemy(name="big goblin", hp=70, alerted=false)
```

### Constructors

A method named `init` is the constructor. It runs when the class is called,
after the field defaults are set.

```taipan
class Player {
    str name
    int hp
    int level = 1

    func init(str playerName, int startHp = 100) {
        name = playerName
        hp = startHp
    }
}

Player a = Player("Ayla")
Player b = Player("Bram", 80)
print(a.name, a.hp, a.level)
print(b.name, b.hp)
```

Without `init`, a class is created with no arguments: `Enemy()`.

Field defaults run in order, so a default may use earlier fields:
`int maxHp = 100` then `int hp = maxHp`.

### private and public

Members are public by default. `private` restricts a field or method to the
class's own methods. The compiler enforces this.

```taipan
class Wallet {
    private int coins = 0

    func deposit(int amount) {
        if amount > 0 {
            coins += amount
            log("deposit " + amount)
        }
    }

    func balance() -> int {
        return coins
    }

    private func log(str message) {
        print("[wallet] " + message)
    }
}

Wallet w = Wallet()
w.deposit(50)
print(w.balance())
// w.coins = 999      // compile error: 'coins' is private to class 'Wallet'
```

There is no `protected`, because Taipan has no inheritance.

### Constant fields

```taipan
class Weapon {
    const int MAX_DURABILITY = 100
    int durability = 100
}
Weapon sword = Weapon()
print(sword.MAX_DURABILITY)
// sword.MAX_DURABILITY = 5   // compile error: cannot assign to constant
```

### Objects are references

```taipan
class Box {
    int value = 1
}
Box first = Box()
Box second = first        // the same object, not a copy
second.value = 42
print(first.value)        // 42
```

### Composition instead of inheritance

Taipan classes cannot inherit from other classes (`extends` is an error). To
reuse behaviour, give a class a field holding another object, and use
interfaces (next section) for shared contracts.

```taipan
class Health {
    int current = 100
    int max = 100

    func damage(int n) {
        current = math.max(0, current - n)
    }

    func percent() -> int {
        return current * 100 / max
    }
}

class Knight {
    str name = "Sir Bram"
    Health health = Health()       // a Knight HAS a Health
}

class Dragon {
    Health health = Health()       // so does a Dragon: same code, no inheritance
}

Knight k = Knight()
k.health.damage(30)
print(k.name, k.health.percent() + "%")
```

### Fields that hold functions

A `func` field holds a callback. It starts as `null`.

```taipan
class Button {
    str label = "OK"
    func onClick
}

Button b = Button()
b.onClick = () -> { print("clicked!") }
b.onClick()
```

### Methods as values

`obj.method` without parentheses gives a function bound to that object:

```taipan
class Counter {
    int count = 0
    func bump() { count += 1 }
}
Counter c = Counter()
func step = c.bump
step()
step()
print(c.count)      // 2
```

### Static members

A `static` field belongs to the class, not to each object: there is one copy,
shared by everyone. A `static func` is called on the class and has no `this`.

```taipan
class Enemy {
    static int alive = 0
    static const int MAX = 50
    int hp = 10

    func init() {
        alive += 1                 // inside the class, statics need no prefix
    }

    static func canSpawn() -> bool {
        return alive < MAX
    }
}

Enemy a = Enemy()
Enemy b = Enemy()
print(Enemy.alive)                 // 2
print(Enemy.canSpawn())            // true
Enemy.alive = 0                    // assignable from outside, unless const or private
```

- Read and write statics as `ClassName.name`; inside the class (instance or
  static methods) the bare name works too.
- A static method cannot use instance fields or `this` - there is no object.
- Static initializers run once, when the class is defined, in declaration
  order. They may use the class itself: `static Vec2 zero = Vec2(0, 0)`.
- On hot reload, statics keep their current values (like globals) and new
  static fields get their initializer.
- `private` works the same as on instance members.
- `Enemy.canSpawn` without parentheses is a function value, like any other.

### Operator overloading

A class can give meaning to `+ - * / %`, the comparisons `== < <= > >=`, and
unary `-` by defining `operator` methods:

```taipan
class Vec2 {
    float x
    float y

    func init(float ax, float ay) {
        x = ax
        y = ay
    }

    operator +(Vec2 o) -> Vec2 { return Vec2(x + o.x, y + o.y) }
    operator -(Vec2 o) -> Vec2 { return Vec2(x - o.x, y - o.y) }
    operator *(float k) -> Vec2 { return Vec2(x * k, y * k) }
    operator -() -> Vec2 { return Vec2(-x, -y) }               // unary minus
    operator ==(Vec2 o) -> bool { return x == o.x && y == o.y }
    operator <(Vec2 o) -> bool { return x * x + y * y < o.x * o.x + o.y * o.y }
}

Vec2 pos = Vec2(0, 0)
Vec2 vel = Vec2(2, 4)
pos += vel * 0.5                   // compound assignment uses the operator too
print(pos.x, pos.y)                // 1.0 2.0
print(-pos == Vec2(-1, -2))        // true
print(Vec2(1, 1) < vel, vel >= Vec2(1, 1))
```

Rules:
- The left operand decides: `a + b` calls `a`'s `operator +` with `b` as the
  argument. `vel * 0.5` works; for `0.5 * vel` the class has no hook (engine
  value types can define both orders, see below).
- Binary operators take exactly one parameter; unary `-` takes none.
- Comparison operators must return `bool`; the others must return a value.
- `!=` is always `!(a == b)`. If `>`, `<=` or `>=` is missing, it is derived
  from `<` (`a > b` is `b < a`, `a >= b` is `!(a < b)`).
- `&&`, `||`, `??`, indexing and assignment cannot be overloaded.
- Operator methods are type-checked like any method: `vel * "x"` is a
  compile-time error when `operator *` takes a `float`.
- Without `operator ==`, objects compare by identity (same object).

---

## 12. Interfaces

An interface lists methods a class promises to have. Different classes can
implement the same interface, so code can work with any of them. This is how
Taipan does polymorphism.

```taipan
interface Damageable {
    func takeDamage(int amount)
    func isAlive() -> bool
}

class Enemy implements Damageable {
    int hp = 30
    func takeDamage(int amount) { hp -= amount }
    func isAlive() -> bool { return hp > 0 }
}

class Crate implements Damageable {
    int durability = 10
    func takeDamage(int amount) {
        durability -= amount
        if durability <= 0 { print("crate breaks") }
    }
    func isAlive() -> bool { return durability > 0 }
}

list<Damageable> targets = [Enemy(), Crate(), Enemy()]
for Damageable t in targets {
    t.takeDamage(15)
}
for Damageable t in targets {
    print(t.isAlive())
}
```

- A class can implement several interfaces: `class Hero implements Damageable, Named { }`.
- The compiler checks that each method exists with exactly the declared
  signature, and is public.
- A variable of an interface type accepts any class that implements it.

---

## 13. Enums

An enum is a fixed set of named values, ideal for states.

```taipan
enum PlayerState {
    IDLE,
    WALKING,
    ATTACKING,
    DEAD
}

PlayerState state = PlayerState.IDLE
state = PlayerState.ATTACKING
print(state, state.name, state.index)      // PlayerState.ATTACKING ATTACKING 2

match state {
    PlayerState.IDLE -> print("standing still")
    PlayerState.WALKING -> print("moving")
    PlayerState.ATTACKING -> print("swinging!")
    PlayerState.DEAD -> print("game over")
}
```

- Members are separated by commas or newlines.
- `.name` gives the member's name as a `str`; `.index` gives its position.
- A variable or field of an enum type starts at the first member.
- A `match` that misses members and has no `else` gets a warning listing the
  missing ones. A `match` covering every member counts as complete, so a
  function can return from each arm without a final `return`.

---

## 14. Error handling

### try / catch

```taipan
list<int> scores = [1, 2, 3]
try {
    print(scores[10])
} catch error {
    print("something broke: " + error)
}
print("still running")
```

The variable after `catch` receives the error. For built-in errors it is the
message text. The name is optional (`catch { ... }`).

### throw

Any value can be thrown, and `catch` receives that same value:

```taipan
func withdraw(int balance, int amount) -> int {
    if amount > balance {
        throw "insufficient funds"
    }
    return balance - amount
}

try {
    withdraw(10, 50)
} catch e {
    print("error:", e)
}

try {
    throw {"code": 404, "reason": "not found"}
} catch e {
    print(e)
}
```

### What raises errors at runtime

| Error | Cause |
|---|---|
| `index out of range` | `list[i]` / `str[i]` outside the bounds |
| `key not found` | `dict[key]` with a missing key |
| `division by zero` | integer `/` or `%` by 0 |
| `null reference` | using a `null` held in `any` or a `func` |
| `type mismatch` | an `any` value stored into the wrong typed variable |
| `stack overflow` | runaway recursion |
| `uninitialized global` | using a top-level variable or function before its line ran |
| `wait outside coroutine` | `wait()` outside a coroutine |
| `invalid conversion` | `toInt("abc")` and similar |
| `invalid json` | malformed text in `json.parse` |
| `use of invalid ... handle` | an engine object that has been destroyed |

An error nobody catches stops the script and prints a message with the
location and a traceback of the calls that led there.

---

## 15. Coroutines

A coroutine is a function that can pause with `wait(seconds)` and continue
later from the same spot. It's the natural way to write patrols, cutscenes,
timers and delayed events, without callbacks or state machines.

```taipan
coroutine countdown(int from) {
    int n = from
    while n > 0 {
        print("countdown", n)
        n -= 1
        wait(0.2)                 // pause for 0.2 seconds
    }
    print("liftoff!")
}

start countdown(3)                // launch it; runs until the first wait()
print("countdown started")        // the main script continues immediately
```

Output:

```
countdown 3
countdown started
countdown 2
countdown 1
liftoff!
```

### How it works

- `coroutine name(params) { }` declares one, like `func` but able to pause.
- `start name(args)` launches it. It runs until its first `wait`, then the
  caller continues.
- `wait(seconds)` pauses for that long; `wait()` pauses until the next frame.
- Everything runs on one thread, and coroutines take turns only at `wait`. So
  there are no race conditions and no locks.
- Inside a game engine, the engine resumes coroutines every frame. The
  standalone `taipan` runner resumes them in real time and exits when all
  have finished.

### Controlling a running coroutine

`start` returns a handle:

```taipan
coroutine blink() {
    while true {
        print("blink")
        wait(0.1)
    }
}

coroutine stopLater(any task) {
    wait(0.35)
    task.stop()
    print("stopped:", !task.isRunning())
}

any blinker = start blink()
start stopLater(blinker)
```

### Coroutines in classes

```taipan
class Guard {
    str name = "guard"
    int steps = 0

    coroutine patrol(int laps) {
        for int i in range(laps) {
            steps += 1
            print(name + " walks lap " + (i + 1))
            wait(0.1)
        }
        print(name + " is done after " + steps + " laps")
    }
}

Guard g = Guard()
start g.patrol(3)
```

### Rules

- `wait()` is only allowed while running inside a coroutine. Normal functions
  called from a coroutine may wait too, and pause the whole coroutine.
- `wait()` inside a function called from native code (such as the lambda
  given to `list.forEach`) is an error. Use a normal `for` loop instead.
- A coroutine with an uncaught error prints it and ends; other coroutines keep
  running.
- `time.scriptTime()` returns the script clock that coroutines run on.

---

## 16. Modules and imports

Larger projects split code across files. `import` loads another `.tp` file,
and its members are used through the module path.

Say the project looks like this:

```
game/
  main.tp
  helpers.tp
  enemies/
    ai.tp
```

`enemies/ai.tp`:

```text
int spawned = 0

class Goblin {
    int hp = 30
}

func spawn() -> Goblin {
    spawned += 1
    return Goblin()
}
```

`main.tp`:

```text
import enemies.ai              // loads enemies/ai.tp
import enemies.ai as ai        // the same module under a shorter name
import helpers                 // a bare name: helpers.tp

enemies.ai.Goblin g = enemies.ai.spawn()
ai.spawn()
print(g.hp, ai.spawned)        // 30 2
```

- **Dotted paths** map to folders: `import a.b.c` loads `a/b/c.tp` from the
  script root. The script root is the main script's folder, unless the engine
  sets another.
- **A bare name** (`import helpers`) must match exactly one `helpers.tp`
  anywhere under the root. If two match, the compiler asks for the dotted
  path.
- **Always use `module.member`.** There is no `from x import y`, so the source
  of every name stays visible.
- **Types from modules** are written with the path: `enemies.ai.Goblin`.
- **A module's top-level code runs once**, the first time it is imported. Its
  globals keep their values between uses.
- **Circular imports** (`a` imports `b`, `b` imports `a`) are compile errors
  that show the chain.
- **The standard library** (`math`, `str`, `json`, `time`) and engine
  functions (`engine.*`) never need `import`.

---

## 17. Extended number types

For networking, packed data or tight memory, `enable ExtType` at the top of a
file unlocks fixed-width numbers:

```taipan
enable ExtType

uint8 small = 250
small += 10                      // wraps around: 4
int16 medium = 32767
medium += 1                      // wraps to -32768
byte flags = 0b1010              // byte is the same as uint8
f32 third = 1.0 / 3.0            // single precision
int packed = (flags << 4) | 0x0F
print(small, medium, third, packed)
```

| Types | Range |
|---|---|
| `int8 int16 int32 int64` | signed integers of that many bits |
| `uint8 uint16 uint32 uint64` (`byte` = `uint8`) | unsigned integers |
| `f32 f64` | single / double precision floats |

- Values wrap or round to the width whenever they are stored.
- A literal that doesn't fit is a compile error (`uint8 bad = 300`).
- Arithmetic is done at full width; storing the result narrows it.

---

## 18. Standard library reference

These are always available, both standalone and inside an engine. `= ...`
marks an optional argument.

### Global functions

| Function | Description |
|---|---|
| `print(any...)` | Print values separated by spaces, then a newline |
| `range(int, int = ..., int = ...) -> list<int>` | `range(end)`, `range(start, end)`, `range(start, end, step)` |
| `toInt(any) -> int` | From float (truncates), str (also `"0x1F"`), bool |
| `toFloat(any) -> float` | From int, str, bool |
| `toStr(any) -> str` | Text form of any value |
| `toBool(any) -> bool` | From numbers (non-zero), `"true"`/`"false"`/`"1"`/`"0"`, null (false) |
| `typeOf(any) -> str` | Runtime type name: `"int"`, `"list"`, a class name, ... |
| `wait(float = 0.0)` | Pause the current coroutine |

### math

| Function | Description |
|---|---|
| `math.abs(x)`, `math.min(a, b)`, `math.max(a, b)` | Integer in, integer out; float otherwise |
| `math.floor(float) -> int`, `math.ceil(float) -> int`, `math.round(float) -> int` | Round to an integer (`round` goes half away from zero) |
| `math.sqrt(float) -> float`, `math.pow(float, float) -> float` | Square root, power |
| `math.sin`, `math.cos`, `math.tan` `(float) -> float` | Trigonometry, in radians |
| `math.atan2(float y, float x) -> float` | Angle of a vector |
| `math.clamp(float v, float lo, float hi) -> float` | Limit to a range |
| `math.lerp(float a, float b, float t) -> float` | Linear interpolation |
| `math.random() -> float` | Random number in 0.0–1.0 |
| `math.randomInt(int lo, int hi) -> int` | Random integer, both ends included |
| `math.seed(int)` | Make random numbers repeatable |
| `math.pi`, `math.inf` | Constants |

### str

Each can also be called as a method on a string: `s.trim()`.

| Function | Description |
|---|---|
| `str.length(s) -> int` | Length in bytes (also the `s.length` property) |
| `str.toUpper(s)`, `str.toLower(s)` | Change case |
| `str.trim(s)` | Remove surrounding whitespace |
| `str.split(s, str sep = " ") -> list<str>` | Split into parts |
| `str.contains(s, part) -> bool` | Substring test |
| `str.startsWith(s, p)`, `str.endsWith(s, p)` | Prefix / suffix test |
| `str.indexOf(s, part) -> int` | First position, or `-1` |
| `str.replace(s, old, new)` | Replace every occurrence |
| `str.sub(s, int start, int end = ...)` | Part of the string (negative positions count from the end) |
| `str.repeat(s, int n)` | Repeat `n` times |

### json

| Function | Description |
|---|---|
| `json.parse(str) -> any` | JSON text to dicts, lists, strings, numbers, bools, null |
| `json.stringify(any) -> str` | Value to JSON (objects become JSON objects of their fields) |

### time

| Function | Description |
|---|---|
| `time.now() -> float` | Wall-clock time in seconds since 1970 |
| `time.clock() -> float` | Seconds since the program started (good for measuring) |
| `time.scriptTime() -> float` | The coroutine clock |

### List methods

| Method | Description |
|---|---|
| `add(T)`, `insert(int, T)` | Append, insert at a position |
| `removeAt(int) -> T`, `remove(T) -> bool`, `pop() -> T`, `clear()` | Remove items |
| `contains(T) -> bool`, `indexOf(T) -> int`, `isEmpty() -> bool` | Search |
| `sort(func = ...)`, `reverse()` | Reorder in place |
| `slice(int, int = ...) -> list<T>`, `copy() -> list<T>` | New lists |
| `join(str = ...) -> str` | Join the items as text |
| `forEach(func)`, `map(func) -> list<any>`, `filter(func) -> list<T>` | Functional helpers |
| `length` | Number of items (a property) |

### Dict methods

| Method | Description |
|---|---|
| `keys() -> list<K>`, `values() -> list<V>` | In insertion order |
| `has(K) -> bool` (or `contains`) | Key test |
| `get(K, V default) -> V` | Value, or the default if missing |
| `remove(K) -> bool`, `clear()` | Remove entries |
| `copy()`, `isEmpty()`, `length` | |

### Standalone runner only

Available when running scripts with `taipan`. They are not available inside a
game engine, because they block or touch the file system freely.

| Function | Description |
|---|---|
| `input(str prompt = ...) -> str` | Read a line from the keyboard |
| `time.sleep(float seconds)` | Block the program for a while |
| `file.read(str path) -> str` | Read a whole file |
| `file.write(str path, any data)`, `file.append(str path, any data)` | Write / append text |
| `file.exists(str path) -> bool` | File test |

---

## 19. Reading error messages

Every error points at the exact spot and says what was expected:

```text
error: type mismatch
  --> game.tp:3:13
   |
 3 |     int x = "hello"
   |             ^^^^^^^ expected 'int', found 'str'
```

- **The first line** names the problem.
- **`-->`** gives file, line and column.
- **The source line and the `^^^` carets** mark the exact code.
- **The label after the carets** explains what was expected and what was
  found.
- **`= note:`** lines add hints.

Fix the first error first: later errors are often consequences of it.
`warning:` messages don't stop the script but usually point at real problems.

| Message | Usual fix |
|---|---|
| `type mismatch` ... `expected 'int', found 'str'` | Convert (`toInt`, `toStr`) or change the variable's type |
| `possible null reference` | Check `!= null` first, or use `??` |
| `cannot use 'null' as non-nullable type` | Declare the type with `?` |
| `unknown identifier` | Typo, a missing `import`, or the name isn't declared in this scope |
| `cannot assign to constant` | It was declared `const`; use a normal variable |
| `missing return` | Every path of a `-> T` function must `return` a value |
| `missing initializer` | Give the class-typed variable a value, or make it `T?` |
| `syntax error` ... `Taipan uses 'else if', not 'elif'` | Write `else if` |
| `unused variable` (warning) | Remove it, or prefix its name with `_` |
| `implicit conversion` (warning) | A float was truncated to an int; use `math.round` / `toInt` to make it explicit |
| `non-exhaustive match` (warning) | Add the missing enum members or an `else ->` arm |

---

## 20. Scripting a game engine

Inside an engine, scripts call functions and use types that the engine
provides, usually under names like `engine.*` and types like `Entity`:

```text
entity goblin = engine.spawn("goblin.prefab", 3, 4)
goblin.takeDamage(10)
engine.print(goblin.hp)

coroutine patrol(entity e) {
    while e.hp > 0 {
        e.x += 1
        wait(0.5)
    }
}
start patrol(goblin)
```

- Engine objects are handles: the engine owns them. If one is destroyed,
  using its handle is a catchable `use of invalid ... handle` error, not a
  crash.
- To check such scripts outside the engine (editor, CI), the engine writes a
  binding file (`engine.tpi`) describing its API, and
  `taipan --check --bindings engine.tpi script.tp` uses it. The editor
  extension loads `.tpi` files automatically.
- When the engine hot-reloads a script:
  - functions and class methods are replaced
  - global variables and running coroutines keep their state
  - top-level statements like `start patrol(goblin)` don't run again
  - a function named `onReload()`, if the script defines one, is called
    afterwards

```taipan
int reloads = 0
func onReload() {
    reloads += 1
    print("script reloaded", reloads, "times")
}
print("loaded")
```

### Engine value types: Vector3 and friends

Engines usually provide small math types - `Vector3`, `Quaternion`,
`Color`. They are **value types**: each value is an immutable copy, so
`Vector3 a = b` never links the two, and they compare by content. The engine
gives them constructors, operators and static members, which makes
Unity-style code work as written:

```text
float speed = 4.0

func update(Transform self, float dt) {
    Vector3 dir = Input.GetAxis2("Move")
    self.position += dir * speed * dt            // operators + property write-back
    if Vector3.Distance(self.position, Vector3.zero) > 100.0 {
        self.position = Vector3(0, 1, 0)         // constructor
    }
    Debug.Log(f"at {self.position}")             // prints (x, y, z)
}
```

- `Vector3(1, 2, 3)` calls the engine's constructor; `Vector3.zero` and
  `Vector3.Distance(a, b)` are static members.
- Engine types may define operators in both orders, so `2.0 * v` and
  `v * 2.0` can both work.
- Fields of a value are read-only: `v.x = 5` is a compile-time error.
  Build a new value instead: `v = Vector3(5, v.y, v.z)`.
- `self.position += delta` reads the property, adds, and writes the new value
  back through the engine's setter.
- Value types work as dict keys and in lists like numbers do.
- Each operation creates a new small object for the garbage collector. In a
  hot loop over thousands of entities, prefer engine functions that work in
  bulk.

How the engine side works (registering functions, classes and value types,
hot reload, bindings) is in [README.md](README.md#4-embedding-in-an-engine).

---

## 21. Style guide

These are conventions, not rules the compiler enforces:

- **Braces:** the opening brace goes on the same line: `if ready {`.
- **Indentation:** four spaces.
- **Names:**
  - `camelCase` for variables, functions and fields
  - `UpperCase` for classes, interfaces and enums
  - `UPPER_CASE` for constants and enum members
- **Lowercase names** (`engine`, `math`) are modules and namespaces you use
  directly. **Capitalized names** (`Player`, `World`) are types you declare
  variables of.
- **Use `any` sparingly.** Concrete types let the compiler catch mistakes.
- **Prefer nullable types** (`Enemy?`) and early returns over sentinel values.
- **Prefer interfaces and composition** to deep object hierarchies.

---

## 22. A complete program

A small turn-based battle that uses most of the language:

```taipan
interface Fighter {
    func attack() -> int
    func takeHit(int damage)
    func alive() -> bool
}

enum Mood { CALM, ANGRY }

const int MAX_HP = 60

class Hero implements Fighter {
    str name
    int hp = MAX_HP
    list<str> bag = ["potion", "potion"]

    func init(str heroName) {
        name = heroName
    }

    func attack() -> int {
        return math.randomInt(8, 14)
    }

    func takeHit(int damage) {
        hp = math.max(0, hp - damage)
        print(f"  {name} takes {damage} ({hp} hp left)")
        if hp < 20 && bag.contains("potion") {
            bag.remove("potion")
            hp += 25
            print(f"  {name} drinks a potion ({hp} hp)")
        }
    }

    func alive() -> bool {
        return hp > 0
    }
}

class Monster implements Fighter {
    str kind
    int hp
    Mood mood

    func init(str k, int health) {
        kind = k
        hp = health
    }

    func attack() -> int {
        match mood {
            Mood.CALM -> { return math.randomInt(3, 7) }
            Mood.ANGRY -> { return math.randomInt(8, 12) }
        }
    }

    func takeHit(int damage) {
        hp -= damage
        mood = Mood.ANGRY
        print(f"  the {kind} takes {damage}")
    }

    func alive() -> bool {
        return hp > 0
    }
}

func pickTarget(list<Monster> monsters) -> Monster? {
    for Monster m in monsters {
        if m.alive() {
            return m
        }
    }
    return null
}

math.seed(3)
Hero hero = Hero("Ayla")
list<Monster> monsters = [Monster("goblin", 25), Monster("orc", 40)]
dict<str, int> kills = {}
int round = 0

while hero.alive() {
    Monster? target = pickTarget(monsters)
    if target == null {
        break
    }
    round += 1
    print(f"-- round {round} --")
    target.takeHit(hero.attack())
    if !target.alive() {
        print(f"  the {target.kind} is defeated")
        kills[target.kind] = kills.get(target.kind, 0) + 1
    }
    for Monster m in monsters {
        if m.alive() {
            hero.takeHit(m.attack())
        }
    }
}

str result = "fell in battle"
if hero.alive() {
    result = "won"
}
print(f"{hero.name} {result} after {round} rounds. Kills: {kills}")

coroutine victoryLap(int laps) {
    for int lap in range(laps) {
        print(f"  victory lap {lap + 1}")
        wait(0.1)
    }
}
if hero.alive() {
    start victoryLap(2)
}
```

---

## 23. Quick reference

```text
// declarations
int x = 1            float f          str s = "hi"         bool b = true
any a = 1            int? maybe = null                     const int MAX = 10
list<int> xs = [1]   dict<str, int> d = {"k": 1}

// functions
func name(int a, int b = 2) -> int { return a + b }
func noResult(str s) { print(s) }
func pair() -> (int, int) { return 1, 2 }        int p, int q = pair()
func rest(str head, any... tail) { }
func f = (int n) -> n * 2                        // lambda
coroutine task(int n) { wait(0.5) }              start task(3)

// control flow
if c { } else if d { } else { }
while c { }
for int i in range(10) { }        for (int i, T x) in xs { }
for (int i = 0; i < n; i++) { }   break   continue
match v { 1, 2 -> f()   3 -> { g() }   else -> h() }
try { throw "x" } catch e { }

// types
class C implements I { int f = 0; private int p; func init() { }  func m() { } }
class V { static int n = 0; static func make() -> V { }  operator +(V o) -> V { }  operator -() -> V { } }
C.staticField        C.staticMethod()        Vector3(1, 2, 3) + Vector3.zero
interface I { func m() }
enum E { A, B }               E.A   e.name   e.index
x ?? fallback                 if v != null { use(v) }

// modules
import a.b                    import a.b as ab           a.b.member()
enable ExtType                uint8  int16  f32  byte
```

**Operators:**
- arithmetic: `+ - * / %`
- comparison: `== != < <= > >=`
- logical: `&& || !`
- bitwise: `& | ^ ~ << >>`
- null coalescing: `??`
- assignment: `= += -= *= /= %=`, `x++` / `x--` as statements
- overloadable in classes: `+ - * / % == < <= > >=` and unary `-`

**Not in Taipan, on purpose:**
- semicolon requirements
- `then` / `do` / `end`
- indentation rules
- `elif`
- the ternary `?:`
- class inheritance and `protected`
- implicit type inference (`var`)
- `from x import y`
- `#` comments
