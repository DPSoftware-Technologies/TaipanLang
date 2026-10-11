// api_test.cpp - automated checks of the embedding API (hot reload, globals,
// native bindings, errors). Exit code 0 = all passed.
//
// Build: g++ -std=c++17 -I../include ../src/*.cpp api_test.cpp -o api_test
#include "taipan.h"

#include <cmath>
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

struct Vector3 {
    float x = 0, y = 0, z = 0;
    Vector3() = default;
    Vector3(float ax, float ay, float az) : x(ax), y(ay), z(az) {}
    Vector3 operator+(const Vector3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vector3 operator-(const Vector3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vector3 operator*(float k) const { return {x * k, y * k, z * k}; }
    bool operator==(const Vector3& o) const { return x == o.x && y == o.y && z == o.z; }
    float magnitude() const { return std::sqrt(x * x + y * y + z * z); }
    static float Distance(Vector3 a, Vector3 b) { return (a - b).magnitude(); }
};

struct Transform {
    Vector3 position;
};

struct GameObject {
    Transform* transform = nullptr;
};

static void registerVector3(TaipanState* s) {
    taipan_register_value<Vector3>(s, "Vector3")
        .constructor<Vector3(float, float, float)>()
        .property("x", &Vector3::x)
        .property("y", &Vector3::y)
        .property("z", &Vector3::z)
        .method("magnitude", &Vector3::magnitude)
        .op("+", [](const Vector3& a, const Vector3& b) { return a + b; })
        .op("-", [](const Vector3& a, const Vector3& b) { return a - b; })
        .op("-", [](const Vector3& a) { return a * -1.0f; })
        .op("*", [](const Vector3& a, float k) { return a * k; })
        .op("*", [](float k, const Vector3& a) { return a * k; })
        .op("==", [](const Vector3& a, const Vector3& b) { return a == b; })
        .staticMethod("Distance", &Vector3::Distance)
        .constant("zero", Vector3(0, 0, 0))
        .constant("up", Vector3(0, 1, 0))
        .toString([](const Vector3& v) {
            char buf[96];
            std::snprintf(buf, sizeof buf, "(%g, %g, %g)", v.x, v.y, v.z);
            return std::string(buf);
        });
}

static void testValueTypes() {
    // Unity-style vector math: value types with operators, constructors and
    // static members, read and written through a native property
    TaipanState* s = taipan_create();
    taipan_set_bytecode_cache(s, false);
    std::string errors, log;
    taipan_set_error_handler(s, [&errors](const std::string& e) { errors += e; });
    registerVector3(s);
    taipan_set_gc_tiny(s, true); // exercise incremental collection of value objects
    Transform player;
    Transform enemyTransform;
    GameObject enemy;
    enemy.transform = &enemyTransform;
    taipan_register_class(s, "Transform").property("position", &Transform::position);
    taipan_register_class(s, "GameObject").property("transform", &GameObject::transform);
    taipan_bind_function(s, "Scene.enemy", [&enemy]() { return TaipanValue::fromObject("GameObject", &enemy); });
    taipan_bind_function(s, "Scene.player", [&player]() { return TaipanValue::fromObject("Transform", &player); });
    taipan_bind_function(s, "Input.GetAxis2", [](std::string axis) {
        return axis == "Move" ? Vector3(0, 0, 1) : Vector3();
    });
    taipan_bind_function(s, "Debug.Log", [&log](std::string msg) { log += msg + "\n"; });

    CHECK(taipan_load_string(s, R"(
float speed = 4.0
func update(Transform self, float dt) {
    Vector3 dir = Input.GetAxis2("Move")
    self.position += dir * speed * dt
}
Transform t = Scene.player()
update(t, 0.5)
t.position = t.position + 2.0 * Vector3.up
float d = Vector3.Distance(t.position, Vector3.zero)
Debug.Log(f"pos {t.position} dist {d}")
Vector3 n = -Input.GetAxis2("Move")
Debug.Log(f"{n.z} {Vector3(1, 0, 0) == Vector3(1, 0, 0)} {Vector3(1, 0, 0) == Vector3.up} {n.magnitude()}")
dict<Vector3, str> cells = {Vector3(1, 2, 3): "rock"}
Debug.Log(cells[Vector3(1, 2, 3)])
list<Vector3> trail = []
for int i in range(2000) {
    trail.add(Vector3(i, 0, 0) * 2.0)
}
func offset(Vector3 v) -> Vector3 { return v + Vector3(0, 0, 10) }
func enemyUpdate(GameObject self, float dt) {
    Vector3 dir = Input.GetAxis2("Move")
    self.transform.position += dir * speed * dt
}
enemyUpdate(Scene.enemy(), 0.25)
)", "unity.tp"));
    CHECK(taipan_run(s));
    CHECK(errors.empty());
    CHECK(player.position == Vector3(0, 2, 2));
    CHECK(enemyTransform.position == Vector3(0, 0, 1));
    CHECK(log.find("pos (0, 2, 2) dist 2.82") != std::string::npos);
    CHECK(log.find("-1.0 true false 1.0") != std::string::npos);
    CHECK(log.find("rock") != std::string::npos);
    if (!errors.empty()) std::printf("%s\n", errors.c_str());
    if (log.find("pos (0, 2, 2)") == std::string::npos) std::printf("log: %s\n", log.c_str());

    // values cross the API by copy
    taipan_gc_collect(s);
    TaipanValue r = taipan_call_function(s, "offset", {TaipanValue::fromValue(Vector3(1, 1, 1))});
    CHECK(r.isValue());
    const Vector3* v = r.asValue<Vector3>();
    CHECK(v && *v == Vector3(1, 1, 11));
    CHECK(r.toString() == "(1, 1, 11)");

    // value types are immutable; operators are type-checked
    CHECK(!taipan_load_string(s, "Vector3 v = Vector3(1, 2, 3)\nv.x = 5\n", "immut.tp"));
    CHECK(errors.find("value types are immutable") != std::string::npos);
    errors.clear();
    CHECK(!taipan_load_string(s, "Vector3 v = Vector3(1, 2, 3) * \"x\"\n", "badop.tp"));
    CHECK(errors.find("cannot apply '*'") != std::string::npos);
    errors.clear();
    CHECK(!taipan_load_string(s, "Vector3 v = Vector3(1, 2)\n", "badctor.tp"));
    CHECK(!errors.empty());
    errors.clear();
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
    registerVector3(host);
    CHECK(taipan_dump_bindings(host, "api_test.tpi"));
    taipan_destroy(host);
    std::ifstream dumped("api_test.tpi");
    std::string tpi((std::istreambuf_iterator<char>(dumped)), std::istreambuf_iterator<char>());
    dumped.close();
    CHECK(tpi.find("value class Vector3 {") != std::string::npos);
    CHECK(tpi.find("func init(float a1, float a2, float a3)") != std::string::npos);
    CHECK(tpi.find("operator *(float a1, Vector3 a2) -> Vector3") != std::string::npos);
    CHECK(tpi.find("operator -() -> Vector3") != std::string::npos);
    CHECK(tpi.find("static func Distance(Vector3 a1, Vector3 a2) -> float") != std::string::npos);
    CHECK(tpi.find("static Vector3 zero") != std::string::npos);
    if (tpi.find("value class Vector3 {") == std::string::npos) std::printf("%s\n", tpi.c_str());

    TaipanState* tool = taipan_create();
    std::string errors;
    taipan_set_error_handler(tool, [&errors](const std::string& e) { errors += e; });
    CHECK(taipan_load_bindings(tool, "api_test.tpi"));
    CHECK(taipan_load_string(tool, "counter c = game.counter()\nc.add(c.value)\n", "stub_ok.tp"));
    CHECK(!taipan_load_string(tool, "counter c = game.counter()\nc.add(\"x\")\n", "stub_bad.tp"));
    CHECK(errors.find("expected 'int', found 'str'") != std::string::npos);
    errors.clear();
    CHECK(taipan_load_string(tool, R"(
Vector3 v = Vector3(1, 2, 3) + Vector3.zero * 2.0
float d = Vector3.Distance(v, Vector3.up) + v.magnitude() + v.x
Vector3 w = 2.0 * -v
bool same = v == w
)", "stub_vec.tp"));
    if (!errors.empty()) std::printf("%s\n", errors.c_str());
    CHECK(!taipan_load_string(tool, "Vector3 v = Vector3(1, 2, 3) * \"x\"\n", "stub_vec_bad.tp"));
    CHECK(!taipan_load_string(tool, "float f = Vector3.Distance(1, 2)\n", "stub_vec_bad2.tp"));
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

// A host file reader serves scripts and their imports that exist nowhere on disk, the
// way an engine reads them out of an asset package.
static void testFileReader() {
    TaipanState* s = taipan_create();
    taipan_set_warnings(s, false);
    taipan_set_bytecode_cache(s, false);
    taipan_set_script_root(s, "virtual_pkg");
    taipan_set_file_reader(s, [](const std::string& path, std::string& out) {
        std::string p = path;
        for (char& c : p) if (c == '\\') c = '/';
        auto ends = [&](const std::string& tail) {
            return p.size() >= tail.size() && p.compare(p.size() - tail.size(), tail.size(), tail) == 0;
        };
        if (ends("virtual_pkg/main.tp")) {
            out = "import util.math2\nint answer = util.math2.twice(21)\n";
            return true;
        }
        if (ends("virtual_pkg/util/math2.tp")) {
            out = "func twice(int v) -> int { return v * 2 }\n";
            return true;
        }
        return false;
    });
    CHECK(taipan_load_file(s, "virtual_pkg/main.tp")); // dotted import resolved through the reader
    CHECK(taipan_run(s));
    CHECK(taipan_get_global(s, "answer").asInt() == 42);
    taipan_set_error_handler(s, [](const std::string&) {}); // the next failure is expected
    CHECK(!taipan_load_file(s, "virtual_pkg/missing.tp"));  // not the reader's, not on disk
    taipan_destroy(s);
}

int main() {
    testHotReload();
    testInstances();
    testValueTypes();
    testBindings2();
    testBindings();
    testRuntimeError();
    testFileReader();
    if (failures == 0) std::printf("api_test: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
