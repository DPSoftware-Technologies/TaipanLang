// host_bench.cpp - cost of calling C++ from Taipan: a free function and a
// bound class method/property, 1M calls each.
#include "taipan.h"

#include <cstdio>

struct Counter {
    int value = 0;
    void add(int n) { value += n; }
};

static Counter counter;

static TaipanValue nativeAdd(TaipanArgs args) {
    return TaipanValue::fromInt(args.getInt(0) + args.getInt(1));
}

int main() {
    TaipanState* s = taipan_create();
    taipan_register_function(s, "engine.add", nativeAdd, "(int, int) -> int");
    taipan_register_function(s, "engine.counter", [](TaipanArgs) {
        return TaipanValue::fromObject("Counter", &counter);
    }, "() -> Counter");
    taipan_register_class(s, "Counter").property("value", &Counter::value).method("add", &Counter::add);
    const char* script = R"(
func run() {
    int acc = 0
    float t = time.clock()
    for int i in range(1000000) {
        acc = engine.add(acc, 1)
    }
    print(f"1M host function calls   {math.round((time.clock() - t) * 10000.0) / 10.0} ms")
    Counter c = engine.counter()
    t = time.clock()
    for int i in range(1000000) {
        c.add(1)
    }
    print(f"1M bound method calls    {math.round((time.clock() - t) * 10000.0) / 10.0} ms")
    t = time.clock()
    int total = 0
    for int i in range(1000000) {
        total += c.value
    }
    print(f"1M property reads        {math.round((time.clock() - t) * 10000.0) / 10.0} ms")
}
run()
)";
    taipan_set_warnings(s, false);
    if (!taipan_load_string(s, script, "host_bench.tp") || !taipan_run(s)) return 1;
    taipan_destroy(s);
    return 0;
}
