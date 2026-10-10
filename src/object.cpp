// object.cpp - heap object helpers: strings, containers, equality, printing.
#include "internal.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <new>

namespace tp {

uint64_t fnv1a(const void* data, size_t n, uint64_t h) {
    const unsigned char* p = static_cast<const unsigned char*>(data);
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

std::string formatFloat(double d) {
    if (std::isnan(d)) return "nan";
    if (std::isinf(d)) return d < 0 ? "-inf" : "inf";
    char buf[40];
    std::snprintf(buf, sizeof buf, "%.15g", d);
    if (std::strtod(buf, nullptr) != d) std::snprintf(buf, sizeof buf, "%.17g", d);
    std::string s = buf;
    if (s.find_first_of(".en") == std::string::npos) s += ".0";
    return s;
}

// ---------------------------------------------------------------------------
// Value hashing / equality (dict keys)
// ---------------------------------------------------------------------------
size_t ValueHash::operator()(const Value& v) const {
    switch (v.t) {
    case VT::Null: return 0x9e3779b9u;
    case VT::Bool: return v.b ? 1231 : 1237;
    case VT::Int: return std::hash<int64_t>()(v.i);
    case VT::Float: {
        double d = v.f;
        if (d == std::floor(d) && d >= -9.2e18 && d <= 9.2e18) return std::hash<int64_t>()((int64_t)d);
        return std::hash<double>()(d);
    }
    case VT::Obj:
        if (v.o->type == OT::String) return (size_t)static_cast<ObjString*>(v.o)->hash;
        if (v.o->type == OT::Value) {
            auto* nv = static_cast<ObjValue*>(v.o);
            return (size_t)fnv1a(nv->data, nv->cls->size);
        }
        return std::hash<const void*>()(v.o);
    }
    return 0;
}

// Native value types compare and hash by content (their bytes).
static bool sameNativeValue(const Value& a, const Value& b) {
    if (a.t != VT::Obj || b.t != VT::Obj || a.o->type != OT::Value || b.o->type != OT::Value) return false;
    auto* x = static_cast<ObjValue*>(a.o);
    auto* y = static_cast<ObjValue*>(b.o);
    return x->cls == y->cls && std::memcmp(x->data, y->data, x->cls->size) == 0;
}

static bool valuesEqual(const Value& a, const Value& b) {
    if (a.isNum() && b.isNum()) {
        if (a.t == VT::Int && b.t == VT::Int) return a.i == b.i;
        return a.num() == b.num();
    }
    if (a.t != b.t) return false;
    switch (a.t) {
    case VT::Null: return true;
    case VT::Bool: return a.b == b.b;
    case VT::Obj: return a.o == b.o || sameNativeValue(a, b); // strings are interned
    default: return false;
    }
}

bool ValueEq::operator()(const Value& a, const Value& b) const { return valuesEqual(a, b); }

bool VM::equals(const Value& a, const Value& b) const { return valuesEqual(a, b); }

// ---------------------------------------------------------------------------
// Dict
// ---------------------------------------------------------------------------
Value* ObjDict::find(const Value& k) {
    auto it = index.find(k);
    return it == index.end() ? nullptr : &entries[it->second].second;
}

void ObjDict::put(const Value& k, const Value& v) {
    auto it = index.find(k);
    if (it != index.end()) {
        entries[it->second].second = v;
        return;
    }
    index[k] = entries.size();
    entries.emplace_back(k, v);
}

bool ObjDict::remove(const Value& k) {
    auto it = index.find(k);
    if (it == index.end()) return false;
    entries.erase(entries.begin() + (long)it->second);
    index.clear();
    for (size_t i = 0; i < entries.size(); i++) index[entries[i].first] = i;
    return true;
}

Value& ObjUpvalue::ref() { return open ? th->stack[slot] : closed; }

ObjValue::~ObjValue() {
    if (!data) return;
    if (cls && cls->destroy) cls->destroy(data);
    ::operator delete(data, std::align_val_t(cls && cls->align > alignof(std::max_align_t) ? cls->align
                                                                                       : alignof(std::max_align_t)));
}

Value VM::newValueObj(NativeClass* cls, const void* src) {
    ObjValue* v = alloc<ObjValue>(cls->size);
    size_t align = cls->align > alignof(std::max_align_t) ? cls->align : alignof(std::max_align_t);
    void* data = ::operator new(cls->size ? cls->size : 1, std::align_val_t(align));
    cls->copy(data, src);
    v->cls = cls;
    v->data = data;
    return Value::object(v);
}

// ---------------------------------------------------------------------------
// Allocation helpers
// ---------------------------------------------------------------------------
ObjString* VM::str(std::string_view s) {
    auto it = strings.find(s);
    if (it != strings.end()) {
        ObjString* o = it->second;
        // Found a string the incremental sweep has not reached yet but has
        // already judged dead: resurrect it.
        if (phase == GcPhase::Sweep && o->mark != curMark) o->mark = curMark;
        return o;
    }
    ObjString* o = alloc<ObjString>(s.size());
    o->s.assign(s.data(), s.size());
    o->hash = fnv1a(s.data(), s.size());
    strings.emplace(std::string_view(o->s), o);
    return o;
}

ObjList* VM::newList(OT t) {
    ObjList* l = new ObjList(t);
    link(l, sizeof(ObjList));
    return l;
}

ObjDict* VM::newDict() { return alloc<ObjDict>(); }

ObjThread* VM::newThread() {
    ObjThread* t = alloc<ObjThread>(256);
    t->stack.reserve(64);
    return t;
}

ObjNamespace* VM::newNamespace(const std::string& name) {
    ObjNamespace* ns = alloc<ObjNamespace>();
    ns->name = str(name);
    return ns;
}

ObjNative* VM::newNative(const std::string& name, NativeImpl fn) {
    ObjNative* n = alloc<ObjNative>();
    n->name = str(name);
    n->fn = std::move(fn);
    return n;
}

// ---------------------------------------------------------------------------
// Printing
// ---------------------------------------------------------------------------
std::string VM::reprString(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\t': out += "\\t"; break;
        case '\r': out += "\\r"; break;
        default: out += c;
        }
    }
    return out + "\"";
}

std::string VM::toStr(const Value& v, int depth) {
    switch (v.t) {
    case VT::Null: return "null";
    case VT::Bool: return v.b ? "true" : "false";
    case VT::Int: return std::to_string(v.i);
    case VT::Float: return formatFloat(v.f);
    case VT::Obj: break;
    }
    if (depth > 8) return "...";
    auto inner = [&](const Value& x) {
        if (x.isObj() && x.o->type == OT::String) return reprString(static_cast<ObjString*>(x.o)->s);
        return toStr(x, depth + 1);
    };
    Obj* o = v.o;
    switch (o->type) {
    case OT::String: return static_cast<ObjString*>(o)->s;
    case OT::List: case OT::Tuple: {
        auto* l = static_cast<ObjList*>(o);
        std::string s = o->type == OT::List ? "[" : "(";
        for (size_t i = 0; i < l->items.size(); i++) {
            if (i) s += ", ";
            s += inner(l->items[i]);
        }
        return s + (o->type == OT::List ? "]" : ")");
    }
    case OT::Dict: {
        auto* d = static_cast<ObjDict*>(o);
        std::string s = "{";
        for (size_t i = 0; i < d->entries.size(); i++) {
            if (i) s += ", ";
            s += inner(d->entries[i].first) + ": " + inner(d->entries[i].second);
        }
        return s + "}";
    }
    case OT::Closure: return "<func " + static_cast<ObjClosure*>(o)->proto->name + ">";
    case OT::Native: return "<native func " + static_cast<ObjNative*>(o)->name->s + ">";
    case OT::Class: return "<class " + static_cast<ObjClass*>(o)->name->s + ">";
    case OT::Instance: {
        auto* inst = static_cast<ObjInstance*>(o);
        std::string s = inst->cls->name->s + "(";
        for (size_t i = 0; i < inst->cls->fields.size() && i < inst->fields.size(); i++) {
            if (i) s += ", ";
            s += inst->cls->fields[i].name->s + "=" + inner(inst->fields[i]);
        }
        return s + ")";
    }
    case OT::Bound: return "<bound method>";
    case OT::Module: return "<module " + static_cast<ObjModule*>(o)->name->s + ">";
    case OT::Namespace: return "<namespace " + static_cast<ObjNamespace*>(o)->name->s + ">";
    case OT::Enum: return "<enum " + static_cast<ObjEnum*>(o)->name->s + ">";
    case OT::EnumValue: {
        auto* ev = static_cast<ObjEnumValue*>(o);
        return ev->en->name->s + "." + ev->name->s;
    }
    case OT::Handle: {
        auto* h = static_cast<ObjHandle*>(o);
        std::string n = h->cls ? h->cls->name : "native";
        return h->alive ? "<" + n + ">" : "<" + n + " (destroyed)>";
    }
    case OT::Thread: return "<coroutine>";
    case OT::Upvalue: return "<upvalue>";
    case OT::Value: {
        auto* v = static_cast<ObjValue*>(o);
        if (v->cls->toString) return v->cls->toString(v->data);
        std::vector<std::string> names;
        for (auto& kv : v->cls->props) names.push_back(kv.first);
        std::sort(names.begin(), names.end());
        std::string s = v->cls->name + "(";
        for (size_t i = 0; i < names.size(); i++) {
            const NativeProp& p = v->cls->props[names[i]];
            if (!p.get) continue;
            if (i) s += ", ";
            s += names[i] + "=" + p.get(v->data).toString();
        }
        return s + ")";
    }
    }
    return "<object>";
}

std::string VM::typeName(const Value& v) {
    switch (v.t) {
    case VT::Null: return "null";
    case VT::Bool: return "bool";
    case VT::Int: return "int";
    case VT::Float: return "float";
    case VT::Obj: break;
    }
    switch (v.o->type) {
    case OT::String: return "str";
    case OT::List: return "list";
    case OT::Tuple: return "tuple";
    case OT::Dict: return "dict";
    case OT::Closure: case OT::Native: case OT::Bound: return "func";
    case OT::Class: return "class";
    case OT::Instance: return static_cast<ObjInstance*>(v.o)->cls->name->s;
    case OT::Module: return "module";
    case OT::Namespace: return "namespace";
    case OT::Enum: return "enum";
    case OT::EnumValue: return static_cast<ObjEnumValue*>(v.o)->en->name->s;
    case OT::Handle: {
        auto* h = static_cast<ObjHandle*>(v.o);
        return h->cls ? h->cls->name : "native";
    }
    case OT::Thread: return "coroutine";
    case OT::Upvalue: return "upvalue";
    case OT::Value: return static_cast<ObjValue*>(v.o)->cls->name;
    }
    return "object";
}

} // namespace tp
