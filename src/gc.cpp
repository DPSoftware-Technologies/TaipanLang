// gc.cpp - incremental, generational garbage collector.
//
// Generational: new objects start in the young list. A minor collection
// (stop-the-world, but only touching young objects) marks from the roots plus
// the remembered set (old objects that had a young object stored into them,
// recorded by the write barrier) and promotes every survivor to old.
//
// Incremental: when the old generation grows past its limit a major cycle
// starts. Marking (tri-colour, with a gray stack) and sweeping are spread over
// many small steps triggered by allocation. A Dijkstra-style write barrier
// keeps marking correct while the script mutates the heap; thread stacks are
// not barriered, so they are re-scanned in a short atomic phase before
// sweeping. Objects allocated while marking are born white; objects
// allocated while sweeping are born marked so the sweep keeps them.
#include "internal.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace tp {

// Calls f(Obj*) for every heap object directly referenced by `o`.
template <class F> static void eachChild(Obj* o, F&& f) {
    auto val = [&](const Value& v) {
        if (v.t == VT::Obj) f(v.o);
    };
    switch (o->type) {
    case OT::String: break;
    case OT::List: case OT::Tuple:
        for (auto& v : static_cast<ObjList*>(o)->items) val(v);
        break;
    case OT::Dict:
        for (auto& e : static_cast<ObjDict*>(o)->entries) {
            val(e.first);
            val(e.second);
        }
        break;
    case OT::Closure: {
        auto* c = static_cast<ObjClosure*>(o);
        for (auto* u : c->upvals) {
            if (u) f(u);
        }
        if (c->module) f(c->module);
        break;
    }
    case OT::Upvalue: {
        auto* u = static_cast<ObjUpvalue*>(o);
        if (u->open) {
            if (u->th) f(u->th);
        } else {
            val(u->closed);
        }
        break;
    }
    case OT::Native:
        if (static_cast<ObjNative*>(o)->name) f(static_cast<ObjNative*>(o)->name);
        break;
    case OT::Class: {
        auto* c = static_cast<ObjClass*>(o);
        if (c->name) f(c->name);
        if (c->qual) f(c->qual);
        for (auto& fd : c->fields) f(fd.name);
        for (auto& kv : c->methods) {
            f(kv.first);
            val(kv.second);
        }
        val(c->init);
        for (auto* s : c->interfaces) f(s);
        break;
    }
    case OT::Instance: {
        auto* i = static_cast<ObjInstance*>(o);
        f(i->cls);
        for (auto& v : i->fields) val(v);
        break;
    }
    case OT::Bound:
        val(static_cast<ObjBound*>(o)->recv);
        val(static_cast<ObjBound*>(o)->method);
        break;
    case OT::Module: {
        auto* m = static_cast<ObjModule*>(o);
        if (m->name) f(m->name);
        for (auto& v : m->globals) val(v);
        for (auto& kv : m->names) f(kv.first);
        val(m->mainFn);
        break;
    }
    case OT::Namespace: {
        auto* n = static_cast<ObjNamespace*>(o);
        if (n->name) f(n->name);
        for (auto& kv : n->members) {
            f(kv.first);
            val(kv.second);
        }
        break;
    }
    case OT::Enum: {
        auto* e = static_cast<ObjEnum*>(o);
        if (e->name) f(e->name);
        if (e->qual) f(e->qual);
        for (auto* v : e->values) f(v);
        break;
    }
    case OT::EnumValue: {
        auto* ev = static_cast<ObjEnumValue*>(o);
        f(ev->en);
        f(ev->name);
        break;
    }
    case OT::Handle: break;
    case OT::Thread: {
        auto* t = static_cast<ObjThread*>(o);
        for (auto& v : t->stack) val(v);
        for (auto& fr : t->frames) f(fr.closure);
        for (ObjUpvalue* u = t->openUpvals; u; u = u->nextOpen) f(u);
        break;
    }
    }
}

void VM::link(Obj* o, size_t bytes) {
    o->gcSize = (uint32_t)bytes;
    o->next = youngList;
    youngList = o;
    youngBytes += bytes;
    objectCount++;
    debt += bytes;
    // During marking new objects are born white (unmarked): they are found
    // through the stack re-scan in the atomic phase or through the write
    // barrier, so marking work does not grow with the allocation rate.
    if (phase == GcPhase::Sweep) {
        o->mark = curMark; // survives the sweep in progress
    }
    if (phase != GcPhase::Idle) {
        if (debt >= stepBytes) gcRequested = true;
    } else if (youngBytes >= minorLimit || oldBytes >= majorLimit) {
        gcRequested = true;
    }
}

// ---------------------------------------------------------------------------
// Major (incremental) cycle
// ---------------------------------------------------------------------------
#ifdef TAIPAN_GC_DEBUG
// Debug builds keep freed objects poisoned instead of deleting them, so a
// dangling reference is detected the next time the collector reaches it.
static const OT kPoison = (OT)0xEE;
static void checkLive(Obj* o) {
    if (o->type == kPoison) {
        std::fputs("taipan: GC bug - reachable object was freed\n", stderr);
        std::abort();
    }
}
#else
static inline void checkLive(Obj*) {}
#endif

void VM::markObj(Obj* o) {
    checkLive(o);
    if (o->mark == curMark) return;
    o->mark = curMark;
    if (o->type != OT::String) gray.push_back(o);
}

void VM::traverse(Obj* o) {
    eachChild(o, [this](Obj* c) { markObj(c); });
}

void VM::markRoots() {
    if (builtins) markObj(builtins);
    if (mainThread) markObj(mainThread);
    if (current) markObj(current);
    for (auto* t : coroutines) markObj(t);
    for (auto& kv : modules) markObj(kv.second);
    for (auto* m : instances) markObj(m);
    for (auto& p : protos) {
        for (auto& v : p->consts) markValue(v);
        for (auto& c : p->caches) markValue(c.value);
    }
    for (auto& v : tempRoots) markValue(v);
    for (auto& v : pins) markValue(v);
}

void VM::atomicPhase() {
    // Stacks change without write barriers: re-scan every live thread.
    markRoots();
    if (mainThread) traverse(mainThread);
    if (current) traverse(current);
    for (auto* t : coroutines) traverse(t);
    while (!gray.empty()) {
        Obj* o = gray.back();
        gray.pop_back();
        traverse(o);
    }
    phase = GcPhase::Sweep;
    sweepList = 0;
    sweepLink = &oldList;
}

bool VM::sweepSome(size_t budget) {
    while (budget-- > 0) {
        if (sweepList == 0 && *sweepLink == nullptr) {
            sweepList = 1;
            sweepLink = &youngList;
        }
        if (sweepList == 1 && *sweepLink == nullptr) {
            sweepList = 2;
            return true;
        }
        Obj* o = *sweepLink;
        if (o->mark != curMark) {
            *sweepLink = o->next;
            freeObject(o);
        } else {
            sweepLink = &o->next;
        }
    }
    return false;
}

void VM::finishMajor() {
    // Promote everything that survived; the remembered set starts empty.
    if (youngList) {
        Obj* tail = youngList;
        for (Obj* o = youngList; o; o = o->next) {
            o->old = true;
            tail = o;
        }
        tail->next = oldList;
        oldList = youngList;
        youngList = nullptr;
        oldBytes += youngBytes;
        youngBytes = 0;
    }
    for (Obj* o : remembered) o->remembered = false;
    remembered.clear();
    phase = GcPhase::Idle;
    majorCount++;
    majorLimit = std::max(minMajorLimit, oldBytes * 2);
}

void VM::freeObject(Obj* o) {
    if (o->old) oldBytes -= std::min<size_t>(oldBytes, o->gcSize);
    else youngBytes -= std::min<size_t>(youngBytes, o->gcSize);
    objectCount--;
    freedCount++;
    if (o->type == OT::String) {
        auto* s = static_cast<ObjString*>(o);
        auto it = strings.find(std::string_view(s->s));
        if (it != strings.end() && it->second == s) strings.erase(it);
        checkCache.erase(s);
    } else if (o->type == OT::Handle) {
        auto* h = static_cast<ObjHandle*>(o);
        auto it = handles.find(h->ptr);
        if (it != handles.end() && it->second == h) handles.erase(it);
    }
    if (o->remembered) {
        auto it = std::find(remembered.begin(), remembered.end(), o);
        if (it != remembered.end()) remembered.erase(it);
    }
#ifdef TAIPAN_GC_DEBUG
    o->type = kPoison;
    o->next = nullptr;
#else
    delete o;
#endif
}

// ---------------------------------------------------------------------------
// Minor (young generation) collection
// ---------------------------------------------------------------------------
void VM::minorVisitObj(Obj* o) {
    checkLive(o);
    if (o->old || o->minorMark) return;
    o->minorMark = true;
    gray.push_back(o);
}

void VM::minorVisit(const Value& v) {
    if (v.t == VT::Obj) minorVisitObj(v.o);
}

void VM::minorTraverse(Obj* o) {
    eachChild(o, [this](Obj* c) { minorVisitObj(c); });
}

void VM::minorCollect() {
    minorCount++;
    gray.clear();
    // roots
    if (builtins) minorVisitObj(builtins);
    for (auto& kv : modules) minorVisitObj(kv.second);
    for (auto* m : instances) minorVisitObj(m);
    for (auto& p : protos) {
        for (auto& v : p->consts) minorVisit(v);
        for (auto& c : p->caches) minorVisit(c.value);
    }
    for (auto& v : tempRoots) minorVisit(v);
    for (auto& v : pins) minorVisit(v);
    // threads are always scanned (their stacks have no write barrier)
    std::vector<ObjThread*> threads(coroutines.begin(), coroutines.end());
    if (mainThread) threads.push_back(mainThread);
    if (current) threads.push_back(current);
    for (auto* t : threads) {
        minorVisitObj(t);
        minorTraverse(t);
    }
    // old objects holding young references
    for (Obj* o : remembered) minorTraverse(o);
    while (!gray.empty()) {
        Obj* o = gray.back();
        gray.pop_back();
        minorTraverse(o);
    }
    // Sweep the young list: free the dead, age the survivors, and promote
    // those that have now survived two minor collections.
    std::vector<Obj*> promoted;
    Obj* o = youngList;
    youngList = nullptr;
    Obj** youngTail = &youngList;
    size_t keptBytes = 0;
    while (o) {
        Obj* next = o->next;
        if (!o->minorMark) {
            freeObject(o);
        } else if (o->age >= 1) {
            o->minorMark = false;
            o->old = true;
            oldBytes += o->gcSize;
            o->next = oldList;
            oldList = o;
            promoted.push_back(o);
        } else {
            o->minorMark = false;
            o->age++;
            keptBytes += o->gcSize;
            o->next = nullptr;
            *youngTail = o;
            youngTail = &o->next;
        }
        o = next;
    }
    youngBytes = keptBytes;
    // Rebuild the remembered set: old objects (including the freshly
    // promoted ones) that still reference young objects.
    auto hasYoungChild = [](Obj* x) {
        bool found = false;
        eachChild(x, [&found](Obj* c) { found |= !c->old; });
        return found;
    };
    std::vector<Obj*> keep;
    for (Obj* r : remembered) {
        if (hasYoungChild(r)) keep.push_back(r);
        else r->remembered = false;
    }
    for (Obj* p : promoted) {
        if (!p->remembered && hasYoungChild(p)) {
            p->remembered = true;
            keep.push_back(p);
        }
    }
    remembered.swap(keep);
}

// ---------------------------------------------------------------------------
// Driver
// ---------------------------------------------------------------------------
void VM::collectStep() {
    gcRequested = false;
    if (gcPause > 0) return;
    if (gcStress) {
        // alternate minor and full collections to exercise both paths
        if (++stressCounter % 2) {
            if (phase == GcPhase::Idle) minorCollect();
        } else {
            fullCollect();
        }
        return;
    }
    if (phase == GcPhase::Idle) {
        if (oldBytes >= majorLimit) {
            curMark = curMark == 1 ? 2 : 1;
            phase = GcPhase::Mark;
            gray.clear();
            markRoots();
        } else if (youngBytes >= minorLimit) {
            minorCollect();
            if (oldBytes >= majorLimit) gcRequested = true;
            debt = 0;
            return;
        } else {
            return;
        }
    }
    debt = 0;
    if (phase == GcPhase::Mark) {
        size_t budget = stepBudget;
        while (!gray.empty() && budget-- > 0) {
            Obj* o = gray.back();
            gray.pop_back();
            traverse(o);
        }
        if (gray.empty()) atomicPhase();
    } else if (phase == GcPhase::Sweep) {
        if (sweepSome(stepBudget)) finishMajor();
    }
}

void VM::fullCollect() {
    if (gcPause > 0) return;
    // finish a cycle in progress
    if (phase == GcPhase::Mark) {
        while (!gray.empty()) {
            Obj* o = gray.back();
            gray.pop_back();
            traverse(o);
        }
        atomicPhase();
    }
    if (phase == GcPhase::Sweep) {
        while (!sweepSome(1u << 30)) {
        }
        finishMajor();
    }
    // a complete cycle
    curMark = curMark == 1 ? 2 : 1;
    phase = GcPhase::Mark;
    gray.clear();
    markRoots();
    while (!gray.empty()) {
        Obj* o = gray.back();
        gray.pop_back();
        traverse(o);
    }
    atomicPhase();
    while (!sweepSome(1u << 30)) {
    }
    finishMajor();
    debt = 0;
    gcRequested = false;
}

} // namespace tp
