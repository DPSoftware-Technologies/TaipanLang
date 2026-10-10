// embed_demo.cpp - embedding Taipan in a (pretend) game engine.
//
// Build:  g++ -std=c++17 -O2 -I../include ../src/*.cpp embed_demo.cpp -o embed_demo
// Run:    ./embed_demo            (from the examples folder)
#include "taipan.h"

#include <algorithm>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

// ----- the engine's own object model (the engine owns these) -----
struct Entity {
    std::string name;
    int x = 0;
    int y = 0;
    int hp = 100;

    void takeDamage(int amount) { hp -= amount; }
    bool isAlive() const { return hp > 0; }
    std::string describe() const {
        return name + " at (" + std::to_string(x) + ", " + std::to_string(y) + ") hp=" + std::to_string(hp);
    }
};

static std::vector<std::unique_ptr<Entity>> world;
static TaipanState* state = nullptr;

// ----- native functions: every one has the signature TaipanValue f (TaipanArgs) -----
TaipanValue native_spawn(TaipanArgs args) {
    auto e = std::make_unique<Entity>();
    e->name = args.getString(0);
    e->x = args.count() > 1 ? (int)args.getInt(1) : 0;
    e->y = args.count() > 2 ? (int)args.getInt(2) : 0;
    Entity* raw = e.get();
    world.push_back(std::move(e));
    return TaipanValue::fromObject("Entity", raw);
}

TaipanValue native_destroy(TaipanArgs args) {
    Entity* e = args.getObject<Entity>(0);
    taipan_invalidate_object(state, e); // script handles to it become stale (catchable error)
    world.erase(std::remove_if(world.begin(), world.end(), [e](auto& p) { return p.get() == e; }), world.end());
    return TaipanValue();
}

TaipanValue native_print(TaipanArgs args) {
    std::printf("[engine] %s\n", args[0].toString().c_str());
    return TaipanValue();
}

int main() {
    state = taipan_create();

    // Expose the native class. The script can call it "Entity" or "entity".
    taipan_register_class(state, "Entity")
        .alias("entity")
        .property("name", &Entity::name)
        .property("x", &Entity::x)
        .property("y", &Entity::y)
        .property("hp", &Entity::hp)
        .method("takeDamage", &Entity::takeDamage)
        .method("isAlive", &Entity::isAlive)
        .method("describe", &Entity::describe);

    // Signatures are optional; with them the script is type-checked at load time.
    taipan_register_function(state, "engine.spawn", native_spawn, "(str, int=, int=) -> Entity");
    taipan_register_function(state, "engine.destroy", native_destroy, "(Entity)");
    taipan_register_function(state, "engine.print", native_print, "(any)");

    // Write the registered API as a binding stub so editors and the checker
    // know engine.* and Entity without running the engine.
    taipan_dump_bindings(state, "engine.tpi");

    if (!taipan_load_file(state, "enemy_ai.tp") || !taipan_run(state)) {
        taipan_destroy(state);
        return 1;
    }

    // Read / write script globals from C++.
    TaipanValue v = taipan_get_global(state, "playerHealth");
    if (v.isInt()) std::printf("[engine] playerHealth = %lld\n", (long long)v.asInt());
    taipan_set_global_int(state, "playerHealth", 150);

    // The game loop: 60 fps for 2 simulated seconds.
    for (int frame = 0; frame < 120; frame++) {
        taipan_update(state, 1.0 / 60.0);
        if (frame == 70) taipan_call_function(state, "killGoblin");
    }

    taipan_call_function(state, "onPlayerDeath");
    TaipanValue total = taipan_call_function(state, "sumHp", {TaipanValue::fromInt(5)});
    std::printf("[engine] sumHp(5) returned %s\n", total.toString().c_str());
    std::printf("[engine] entities left: %zu\n", world.size());
    taipan_destroy(state);
    return 0;
}
