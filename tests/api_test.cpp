// api_test.cpp - automated checks of the embedding API (hot reload, globals,
// native bindings, errors). Exit code 0 = all passed.
//
// Build: g++ -std=c++17 -I../include ../src/*.cpp api_test.cpp -o api_test
#include "taipan.h"

#include <cstdio>
#include <fstream>
#include <string>

static int failures = 0;
#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
            failures++;                                                      \
        }                                                                    \
    } while (0)

static void writeFile(const char* path, const std::string& text) {
    std::ofstream(path, std::ios::binary) << text;
}

struct Counter {
    int value = 0;
    void add(int n) { value += n; }
};

static std::string captured;

static void testHotReload() {
    writeFile("reload_test.tp", R"(
int counter = 0
class Goblin {
    int x = 0
    func step() { x += 1 }
}
Goblin g = Goblin()
coroutine patrol() {
    while true {
        g.step()
        counter += 1
        wait(1.0)
    }
}
start patrol()
func speed() -> int { return 1 }
)");
    TaipanState* s = taipan_create();
    taipan_set_bytecode_cache(s, false);
    CHECK(taipan_load_file(s, "reload_test.tp"));
    CHECK(taipan_run(s));
    taipan_update(s, 1.0);
    taipan_update(s, 1.0);
    CHECK(taipan_get_global(s, "counter").asInt() == 3);
    CHECK(taipan_call_function(s, "speed").asInt() == 1);

    writeFile("reload_test.tp", R"(
int counter = 0
class Goblin {
    int x = 0
    int y = 7
    func step() { x += 10 }
    func getY() -> int { return y }
}
Goblin g = Goblin()
coroutine patrol() {
    while true {
        g.step()
        counter += 1
        wait(1.0)
    }
}
start patrol()
func speed() -> int { return 2 }
func goblinX() -> int { return g.x }
func goblinY() -> int { return g.getY() }
int reloads = 0
func onReload() { reloads += 1 }
)");
    CHECK(taipan_reload_file(s, "reload_test.tp"));
    CHECK(taipan_get_global(s, "counter").asInt() == 3);      // state preserved
    CHECK(taipan_call_function(s, "speed").asInt() == 2);     // new code
    CHECK(taipan_coroutine_count(s) == 1);                    // patrol not restarted
    CHECK(taipan_get_global(s, "reloads").asInt() == 1);      // onReload ran
    CHECK(taipan_call_function(s, "goblinX").asInt() == 3);   // same instance
    taipan_update(s, 1.0);
    CHECK(taipan_call_function(s, "goblinX").asInt() == 13);  // running coroutine uses the new method
    CHECK(taipan_call_function(s, "goblinY").asInt() == 0);   // new field: zero value on old instances
    CHECK(taipan_get_global(s, "counter").asInt() == 4);
    taipan_destroy(s);
    std::remove("reload_test.tp");
}

static void testBindings() {
    TaipanState* s = taipan_create();
    taipan_set_bytecode_cache(s, false);
    taipan_set_print_handler(s, [](const std::string& line) { captured += line + "\n"; });
    std::string errors;
    taipan_set_error_handler(s, [&errors](const std::string& e) { errors += e; });
    Counter counter;
    taipan_register_class(s, "Counter").property("value", &Counter::value).method("add", &Counter::add);
    taipan_register_function(s, "game.counter", [&counter](TaipanArgs) {
        return TaipanValue::fromObject("Counter", &counter);
    }, "() -> Counter");
    taipan_register_function(s, "game.twice", [](TaipanArgs args) {
        return args.call(args[0], {TaipanValue::fromInt(21)});
    }, "(func) -> any");
    taipan_register_function(s, "game.fail", [](TaipanArgs args) -> TaipanValue {
        args.error("engine said no");
    });

    // a typed native signature is checked at load time
    CHECK(!taipan_load_string(s, "Counter c = game.counter()\nc.add(\"x\")\n", "typed.tp"));
    CHECK(errors.find("expected 'int', found 'str'") != std::string::npos);
    errors.clear();

    CHECK(taipan_load_string(s, R"(
Counter c = game.counter()
c.add(5)
c.value += 2
int r = toInt(game.twice((int x) -> x * 2))
print("r", r)
try {
    game.fail()
} catch e {
    print("caught", e)
}
int playerHealth = 100
func addHealth(int n) -> int {
    playerHealth += n
    return playerHealth
}
)", "bindings.tp"));
    CHECK(taipan_run(s));
    CHECK(counter.value == 7);
    CHECK(captured.find("r 42") != std::string::npos);
    CHECK(captured.find("caught engine said no") != std::string::npos);
    taipan_set_global_int(s, "playerHealth", 150);
    CHECK(taipan_call_function(s, "addHealth", {TaipanValue::fromInt(5)}).asInt() == 155);
    CHECK(taipan_call_ok(s));
    taipan_call_function(s, "doesNotExist");
    CHECK(!taipan_call_ok(s));

    // stale handles are catchable errors, not crashes
    taipan_invalidate_object(s, &counter);
    CHECK(taipan_load_string(s, R"(
Counter c = game.counter()
print("fresh", c.value)
)", "fresh.tp"));
    CHECK(taipan_run(s));
    CHECK(captured.find("fresh 7") != std::string::npos);
    taipan_destroy(s);
}

static void testRuntimeError() {
    TaipanState* s = taipan_create();
    taipan_set_bytecode_cache(s, false);
    std::string errors;
    taipan_set_error_handler(s, [&errors](const std::string& e) { errors += e; });
    CHECK(taipan_load_string(s, "list<int> xs = [1]\nfunc f() -> int {\n    return xs[5]\n}\nf()\n", "rt.tp"));
    CHECK(!taipan_run(s));
    CHECK(errors.find("error: index out of range") != std::string::npos);
    CHECK(errors.find("--> rt.tp:3:12") != std::string::npos);
    CHECK(errors.find("traceback: f (rt.tp:3) <- <main> (rt.tp:5)") != std::string::npos);
    taipan_destroy(s);
}

static void testBindings2() {
    // dump the registered API, then type-check a script against it in a
    // fresh state that has only the stub file
    TaipanState* host = taipan_create();
    Counter counter;
    taipan_register_class(host, "Counter").alias("counter").property("value", &Counter::value).method("add", &Counter::add);
    taipan_register_function(host, "game.counter", [&counter](TaipanArgs) {
        return TaipanValue::fromObject("Counter", &counter);
    }, "(int=) -> Counter");
    CHECK(taipan_dump_bindings(host, "api_test.tpi"));
    taipan_destroy(host);

    TaipanState* tool = taipan_create();
    std::string errors;
    taipan_set_error_handler(tool, [&errors](const std::string& e) { errors += e; });
    CHECK(taipan_load_bindings(tool, "api_test.tpi"));
    CHECK(taipan_load_string(tool, "counter c = game.counter()\nc.add(c.value)\n", "stub_ok.tp"));
    CHECK(!taipan_load_string(tool, "counter c = game.counter()\nc.add(\"x\")\n", "stub_bad.tp"));
    CHECK(errors.find("expected 'int', found 'str'") != std::string::npos);
    errors.clear();
    CHECK(taipan_load_string(tool, "counter c = game.counter()\n", "stub_run.tp"));
    CHECK(!taipan_run(tool)); // stubs are declarations only
    CHECK(errors.find("declared in a binding file but the host did not register it") != std::string::npos);
    taipan_destroy(tool);
    std::remove("api_test.tpi");
}

static void testInstances() {
    // one environment per script instance (like a Lua environment per entity)
    writeFile("instance_test.tp", R"(
int hp = 100
str label = "unnamed"
coroutine regen() {
    while true {
        wait(1.0)
        hp += 1
    }
}
start regen()
func damage(int n) -> int {
    hp -= n
    return hp
}
func setLabel(str s) { label = s }
)");
    TaipanState* s = taipan_create();
    taipan_set_bytecode_cache(s, false);
    TaipanScript* a = taipan_script_new(s, "instance_test.tp");
    TaipanScript* b = taipan_script_new(s, "instance_test.tp");
    CHECK(a && b);
    CHECK(taipan_script_run(s, a) && taipan_script_run(s, b));
    CHECK(taipan_script_call(s, a, "damage", {TaipanValue::fromInt(30)}).asInt() == 70);
    CHECK(taipan_script_call(s, b, "damage", {TaipanValue::fromInt(5)}).asInt() == 95);
    taipan_script_call(s, a, "setLabel", {TaipanValue::fromString("goblin")});
    CHECK(taipan_script_get(s, a, "label").asString() == "goblin");
    CHECK(taipan_script_get(s, b, "label").asString() == "unnamed");   // separate globals
    CHECK(taipan_coroutine_count(s) == 2);                              // each instance started its own
    taipan_update(s, 1.0);
    CHECK(taipan_script_get(s, a, "hp").asInt() == 71);
    CHECK(taipan_script_get(s, b, "hp").asInt() == 96);
    taipan_script_set(s, b, "hp", TaipanValue::fromInt(500));
    CHECK(taipan_script_get(s, b, "hp").asInt() == 500);

    // hot reload updates every instance and keeps each one's state
    writeFile("instance_test.tp", R"(
int hp = 100
str label = "unnamed"
coroutine regen() {
    while true {
        wait(1.0)
        hp += 1
    }
}
start regen()
func damage(int n) -> int {
    hp -= n * 2
    return hp
}
func setLabel(str s) { label = s }
)");
    CHECK(taipan_reload_file(s, "instance_test.tp"));
    CHECK(taipan_script_get(s, a, "label").asString() == "goblin");
    CHECK(taipan_script_call(s, a, "damage", {TaipanValue::fromInt(1)}).asInt() == 69);
    CHECK(taipan_script_call(s, b, "damage", {TaipanValue::fromInt(1)}).asInt() == 498);
    CHECK(taipan_coroutine_count(s) == 2);                              // nothing restarted

    taipan_script_free(s, a);
    CHECK(taipan_coroutine_count(s) == 1);                              // a's coroutine stopped with it
    taipan_gc_collect(s);
    CHECK(taipan_script_call(s, b, "damage", {TaipanValue::fromInt(1)}).asInt() == 496);
    CHECK(!taipan_script_has_function(s, b, "missing"));
    taipan_script_free(s, b);
    taipan_destroy(s);
    std::remove("instance_test.tp");
}

int main() {
    testHotReload();
    testInstances();
    testBindings2();
    testBindings();
    testRuntimeError();
    if (failures == 0) std::printf("api_test: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
