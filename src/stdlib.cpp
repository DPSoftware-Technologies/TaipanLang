// stdlib.cpp - the standard library.
//
// Core (always available, embedded or standalone): print, conversions,
// range, math.*, str.*, json.*, time.now, plus list/dict methods.
// Standalone-only (blocking or unrestricted I/O): input, time.sleep, file.*.
#include "internal.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>
#include <thread>

namespace tp {

static std::mt19937_64& rng() {
    static std::mt19937_64 r((uint64_t)std::chrono::high_resolution_clock::now().time_since_epoch().count());
    return r;
}

static bool isStrV(const Value& v) { return v.isObj() && v.o->type == OT::String; }

static const std::string& argStr(VM& vm, Value* a, int argc, int i, const char* fn) {
    if (i >= argc || !isStrV(a[i])) {
        vm.rtError("type mismatch", std::string(fn) + ": argument " + std::to_string(i + 1) + " must be 'str', found '" +
                                        (i < argc ? vm.typeName(a[i]) : std::string("nothing")) + "'");
    }
    return static_cast<ObjString*>(a[i].o)->s;
}

static double argNum(VM& vm, Value* a, int argc, int i, const char* fn) {
    if (i >= argc || !a[i].isNum()) {
        vm.rtError("type mismatch", std::string(fn) + ": argument " + std::to_string(i + 1) + " must be a number, found '" +
                                        (i < argc ? vm.typeName(a[i]) : std::string("nothing")) + "'");
    }
    return a[i].num();
}

static int64_t argInt(VM& vm, Value* a, int argc, int i, const char* fn) {
    if (i >= argc || !a[i].isInt()) {
        vm.rtError("type mismatch", std::string(fn) + ": argument " + std::to_string(i + 1) + " must be 'int', found '" +
                                        (i < argc ? vm.typeName(a[i]) : std::string("nothing")) + "'");
    }
    return a[i].i;
}

ObjNamespace* VM::namespaceFor(const std::string& name, bool create) {
    ObjNamespace* ns = builtins;
    size_t start = 0;
    while (start <= name.size()) {
        size_t dot = name.find('.', start);
        std::string part = name.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
        ObjString* key = str(part);
        auto it = ns->members.find(key);
        if (it != ns->members.end() && it->second.isObj() && it->second.o->type == OT::Namespace) {
            ns = static_cast<ObjNamespace*>(it->second.o);
        } else if (create) {
            ObjNamespace* child = newNamespace(name.substr(0, dot));
            ns->members[key] = Value::object(child);
            nsEpoch++;
            barrier(ns, Value::object(key));
            barrier(ns, Value::object(child));
            ns = child;
        } else {
            return nullptr;
        }
        if (dot == std::string::npos) break;
        start = dot + 1;
    }
    return ns;
}

void VM::defineNative(const std::string& qualified, NativeImpl fn, const std::string& sig) {
    gcPause++;
    size_t dot = qualified.rfind('.');
    ObjNamespace* ns = dot == std::string::npos ? builtins : namespaceFor(qualified.substr(0, dot), true);
    std::string name = dot == std::string::npos ? qualified : qualified.substr(dot + 1);
    ObjNative* n = newNative(qualified, std::move(fn));
    ObjString* key = str(name);
    ns->members[key] = Value::object(n);
    barrier(ns, Value::object(key));
    barrier(ns, Value::object(n));
    if (!sig.empty()) nativeSigs[qualified] = sig;
    else nativeSigs.erase(qualified);
    nsEpoch++;
    gcPause--;
}

// ---------------------------------------------------------------------------
// JSON
// ---------------------------------------------------------------------------
namespace {

struct JsonParser {
    VM& vm;
    const std::string& s;
    size_t i = 0;

    [[noreturn]] void fail(const std::string& msg) {
        vm.rtError("invalid json", msg + " at offset " + std::to_string(i));
    }

    void ws() {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) i++;
    }

    std::string str() {
        i++; // opening quote
        std::string out;
        while (i < s.size() && s[i] != '"') {
            char c = s[i++];
            if (c != '\\') {
                out += c;
                continue;
            }
            if (i >= s.size()) fail("unterminated escape");
            char e = s[i++];
            switch (e) {
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            case 'r': out += '\r'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'u': {
                if (i + 4 > s.size()) fail("bad \\u escape");
                unsigned cp = (unsigned)std::stoul(s.substr(i, 4), nullptr, 16);
                i += 4;
                if (cp < 0x80) out += (char)cp;
                else if (cp < 0x800) {
                    out += (char)(0xC0 | (cp >> 6));
                    out += (char)(0x80 | (cp & 0x3F));
                } else {
                    out += (char)(0xE0 | (cp >> 12));
                    out += (char)(0x80 | ((cp >> 6) & 0x3F));
                    out += (char)(0x80 | (cp & 0x3F));
                }
                break;
            }
            default: out += e;
            }
        }
        if (i >= s.size()) fail("unterminated string");
        i++;
        return out;
    }

    Value value(int depth) {
        if (depth > 200) fail("nesting too deep");
        ws();
        if (i >= s.size()) fail("unexpected end of input");
        char c = s[i];
        if (c == '{') {
            i++;
            ObjDict* d = vm.newDict();
            ws();
            if (i < s.size() && s[i] == '}') {
                i++;
                return Value::object(d);
            }
            while (true) {
                ws();
                if (i >= s.size() || s[i] != '"') fail("expected a string key");
                Value k = vm.strv(str());
                ws();
                if (i >= s.size() || s[i] != ':') fail("expected ':'");
                i++;
                Value v = value(depth + 1);
                d->put(k, v);
                ws();
                if (i < s.size() && s[i] == ',') {
                    i++;
                    continue;
                }
                if (i < s.size() && s[i] == '}') {
                    i++;
                    break;
                }
                fail("expected ',' or '}'");
            }
            return Value::object(d);
        }
        if (c == '[') {
            i++;
            ObjList* l = vm.newList();
            ws();
            if (i < s.size() && s[i] == ']') {
                i++;
                return Value::object(l);
            }
            while (true) {
                l->items.push_back(value(depth + 1));
                ws();
                if (i < s.size() && s[i] == ',') {
                    i++;
                    continue;
                }
                if (i < s.size() && s[i] == ']') {
                    i++;
                    break;
                }
                fail("expected ',' or ']'");
            }
            return Value::object(l);
        }
        if (c == '"') return vm.strv(str());
        if (s.compare(i, 4, "true") == 0) {
            i += 4;
            return Value::boolean(true);
        }
        if (s.compare(i, 5, "false") == 0) {
            i += 5;
            return Value::boolean(false);
        }
        if (s.compare(i, 4, "null") == 0) {
            i += 4;
            return Value();
        }
        size_t start = i;
        bool isFloat = false;
        if (s[i] == '-') i++;
        while (i < s.size() && (isdigit((unsigned char)s[i]) || s[i] == '.' || s[i] == 'e' || s[i] == 'E' ||
                                ((s[i] == '+' || s[i] == '-') && (s[i - 1] == 'e' || s[i - 1] == 'E')))) {
            if (s[i] == '.' || s[i] == 'e' || s[i] == 'E') isFloat = true;
            i++;
        }
        if (start == i) fail("unexpected character");
        std::string num = s.substr(start, i - start);
        if (!isFloat) {
            try {
                return Value::integer(std::stoll(num));
            } catch (...) {
            }
        }
        return Value::number(std::strtod(num.c_str(), nullptr));
    }
};

void jsonWrite(VM& vm, const Value& v, std::string& out, int depth) {
    if (depth > 100) vm.rtError("invalid json", "structure too deep (cycle?)");
    switch (v.t) {
    case VT::Null: out += "null"; return;
    case VT::Bool: out += v.b ? "true" : "false"; return;
    case VT::Int: out += std::to_string(v.i); return;
    case VT::Float:
        if (std::isfinite(v.f)) out += formatFloat(v.f);
        else out += "null";
        return;
    case VT::Obj: break;
    }
    switch (v.o->type) {
    case OT::String: {
        out += '"';
        for (unsigned char c : static_cast<ObjString*>(v.o)->s) {
            switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\t': out += "\\t"; break;
            case '\r': out += "\\r"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", c);
                    out += buf;
                } else {
                    out += (char)c;
                }
            }
        }
        out += '"';
        return;
    }
    case OT::List: case OT::Tuple: {
        out += '[';
        auto& items = static_cast<ObjList*>(v.o)->items;
        for (size_t i = 0; i < items.size(); i++) {
            if (i) out += ',';
            jsonWrite(vm, items[i], out, depth + 1);
        }
        out += ']';
        return;
    }
    case OT::Dict: {
        out += '{';
        auto& es = static_cast<ObjDict*>(v.o)->entries;
        for (size_t i = 0; i < es.size(); i++) {
            if (i) out += ',';
            Value k = es[i].first;
            if (!isStrV(k)) k = vm.strv(vm.toStr(k));
            jsonWrite(vm, k, out, depth + 1);
            out += ':';
            jsonWrite(vm, es[i].second, out, depth + 1);
        }
        out += '}';
        return;
    }
    case OT::Instance: {
        auto* inst = static_cast<ObjInstance*>(v.o);
        out += '{';
        for (size_t i = 0; i < inst->cls->fields.size() && i < inst->fields.size(); i++) {
            if (i) out += ',';
            jsonWrite(vm, Value::object(inst->cls->fields[i].name), out, depth + 1);
            out += ':';
            jsonWrite(vm, inst->fields[i], out, depth + 1);
        }
        out += '}';
        return;
    }
    case OT::EnumValue:
        jsonWrite(vm, vm.strv(vm.toStr(v)), out, depth + 1);
        return;
    default:
        vm.rtError("invalid json", "a value of type '" + vm.typeName(v) + "' cannot be converted to JSON");
    }
}

std::string utf8Upper(const std::string& s, bool upper) {
    std::string r = s;
    for (auto& c : r) c = (char)(upper ? toupper((unsigned char)c) : tolower((unsigned char)c));
    return r;
}

} // namespace

// ---------------------------------------------------------------------------
// List / dict methods
// ---------------------------------------------------------------------------
static bool lessDefault(VM& vm, const Value& a, const Value& b) {
    if (a.isNum() && b.isNum()) return a.num() < b.num();
    if (isStrV(a) && isStrV(b)) return static_cast<ObjString*>(a.o)->s < static_cast<ObjString*>(b.o)->s;
    vm.rtError("cannot sort", "cannot compare '" + vm.typeName(a) + "' with '" + vm.typeName(b) + "' (pass a comparison function)");
}

Value VM::builtinListMethod(ObjList* l, int id, Value* argv, int argc) {
    auto& items = l->items;
    switch (id) {
    case 0: // add
        items.push_back(argv[0]);
        barrier(l, argv[0]);
        return Value();
    case 1: { // insert
        int64_t i = argInt(*this, argv, argc, 0, "insert");
        if (i < 0) i += (int64_t)items.size() + 1;
        if (i < 0 || i > (int64_t)items.size()) rtError("index out of range", "insert position " + std::to_string(argv[0].i) + " is out of range");
        items.insert(items.begin() + i, argv[1]);
        barrier(l, argv[1]);
        return Value();
    }
    case 2: { // removeAt
        int64_t i = argInt(*this, argv, argc, 0, "removeAt");
        if (i < 0) i += (int64_t)items.size();
        if (i < 0 || i >= (int64_t)items.size()) {
            rtError("index out of range", "index " + std::to_string(argv[0].i) + " is out of range for a list of length " + std::to_string(items.size()));
        }
        Value v = items[(size_t)i];
        items.erase(items.begin() + i);
        return v;
    }
    case 3: // remove
        for (size_t i = 0; i < items.size(); i++) {
            if (equals(items[i], argv[0])) {
                items.erase(items.begin() + (long)i);
                return Value::boolean(true);
            }
        }
        return Value::boolean(false);
    case 4: { // pop
        if (items.empty()) rtError("empty list", "pop() from an empty list");
        Value v = items.back();
        items.pop_back();
        return v;
    }
    case 5: // contains
        for (auto& x : items) {
            if (equals(x, argv[0])) return Value::boolean(true);
        }
        return Value::boolean(false);
    case 6: // indexOf
        for (size_t i = 0; i < items.size(); i++) {
            if (equals(items[i], argv[0])) return Value::integer((int64_t)i);
        }
        return Value::integer(-1);
    case 7: items.clear(); return Value();
    case 8: { // forEach
        Value fn = argv[0];
        for (size_t i = 0; i < l->items.size(); i++) callValue(fn, {l->items[i]});
        return Value();
    }
    case 9: { // map
        Value fn = argv[0];
        ObjList* out = newList();
        tempRoots.push_back(Value::object(out));
        for (size_t i = 0; i < l->items.size(); i++) {
            Value r = callValue(fn, {l->items[i]});
            out->items.push_back(r);
            barrier(out, r);
        }
        tempRoots.pop_back();
        return Value::object(out);
    }
    case 10: { // filter
        Value fn = argv[0];
        ObjList* out = newList();
        tempRoots.push_back(Value::object(out));
        for (size_t i = 0; i < l->items.size(); i++) {
            Value item = l->items[i];
            if (truthy(callValue(fn, {item}))) {
                out->items.push_back(item);
                barrier(out, item);
            }
        }
        tempRoots.pop_back();
        return Value::object(out);
    }
    case 11: { // sort
        ObjList* copy = newList();
        copy->items = items;
        tempRoots.push_back(Value::object(copy));
        if (argc > 0 && !argv[0].isNull()) {
            Value fn = argv[0];
            std::stable_sort(copy->items.begin(), copy->items.end(),
                             [&](const Value& a, const Value& b) { return truthy(callValue(fn, {a, b})); });
        } else {
            std::stable_sort(copy->items.begin(), copy->items.end(),
                             [&](const Value& a, const Value& b) { return lessDefault(*this, a, b); });
        }
        l->items = copy->items;
        touch(l);
        tempRoots.pop_back();
        return Value();
    }
    case 12: std::reverse(items.begin(), items.end()); return Value();
    case 13: { // slice
        int64_t n = (int64_t)items.size();
        int64_t a = argInt(*this, argv, argc, 0, "slice");
        int64_t b = argc > 1 ? argInt(*this, argv, argc, 1, "slice") : n;
        if (a < 0) a += n;
        if (b < 0) b += n;
        a = std::max<int64_t>(0, std::min(a, n));
        b = std::max<int64_t>(a, std::min(b, n));
        ObjList* out = newList();
        out->items.assign(items.begin() + a, items.begin() + b);
        return Value::object(out);
    }
    case 14: { // join
        std::string sep = argc > 0 ? argStr(*this, argv, argc, 0, "join") : "";
        std::string r;
        for (size_t i = 0; i < items.size(); i++) {
            if (i) r += sep;
            r += toStr(items[i]);
        }
        return strv(r);
    }
    case 15: { // copy
        ObjList* out = newList();
        out->items = items;
        return Value::object(out);
    }
    case 16: return Value::boolean(items.empty());
    }
    return Value();
}

Value VM::builtinDictMethod(ObjDict* d, int id, Value* argv, int argc) {
    (void)argc;
    switch (id) {
    case 0: case 1: { // keys, values
        ObjList* out = newList();
        for (auto& e : d->entries) out->items.push_back(id == 0 ? e.first : e.second);
        return Value::object(out);
    }
    case 2: case 3: return Value::boolean(d->find(argv[0]) != nullptr);
    case 4: return Value::boolean(d->remove(argv[0]));
    case 5: {
        Value* v = d->find(argv[0]);
        return v ? *v : argv[1];
    }
    case 6: d->clear(); return Value();
    case 7: {
        ObjDict* out = newDict();
        out->entries = d->entries;
        out->index = d->index;
        return Value::object(out);
    }
    case 8: return Value::boolean(d->entries.empty());
    }
    return Value();
}

// ---------------------------------------------------------------------------
// Core library
// ---------------------------------------------------------------------------
void VM::openCore() {
    auto out = [this](const std::string& s) {
        if (printHandler) printHandler(s);
        else {
            std::fwrite(s.data(), 1, s.size(), stdout);
            std::fputc('\n', stdout);
            std::fflush(stdout);
        }
    };

    defineNative("print", [this, out](VM&, Value* a, int argc) {
        std::string s;
        for (int i = 0; i < argc; i++) {
            if (i) s += ' ';
            s += toStr(a[i]);
        }
        out(s);
        return Value();
    }, "(any...)");

    defineNative("range", [this](VM&, Value* a, int argc) {
        int64_t start = 0, end, step = 1;
        if (argc == 1) end = argInt(*this, a, argc, 0, "range");
        else {
            start = argInt(*this, a, argc, 0, "range");
            end = argInt(*this, a, argc, 1, "range");
            if (argc > 2) step = argInt(*this, a, argc, 2, "range");
        }
        if (step == 0) rtError("invalid range", "range() step cannot be 0");
        ObjList* l = newList();
        for (int64_t i = start; step > 0 ? i < end : i > end; i += step) l->items.push_back(Value::integer(i));
        return Value::object(l);
    }, "(int, int=, int=) -> list<int>");

    defineNative("toInt", [this](VM&, Value* a, int) {
        Value v = a[0];
        if (v.isInt()) return v;
        if (v.isFloat()) return convert(v, CV_TO_INT);
        if (v.isBool()) return Value::integer(v.b ? 1 : 0);
        if (isStrV(v)) {
            const std::string& s = static_cast<ObjString*>(v.o)->s;
            try {
                size_t used = 0;
                long long r = std::stoll(s, &used, 0);
                if (used == s.size()) return Value::integer(r);
                double d = std::stod(s, &used);
                if (used == s.size()) return convert(Value::number(d), CV_TO_INT);
            } catch (...) {
            }
            rtError("invalid conversion", "cannot convert " + reprString(s) + " to 'int'");
        }
        rtError("invalid conversion", "cannot convert '" + typeName(v) + "' to 'int'");
    }, "(any) -> int");

    defineNative("toFloat", [this](VM&, Value* a, int) {
        Value v = a[0];
        if (v.isNum()) return Value::number(v.num());
        if (v.isBool()) return Value::number(v.b ? 1 : 0);
        if (isStrV(v)) {
            const std::string& s = static_cast<ObjString*>(v.o)->s;
            try {
                size_t used = 0;
                double d = std::stod(s, &used);
                if (used == s.size()) return Value::number(d);
            } catch (...) {
            }
            rtError("invalid conversion", "cannot convert " + reprString(s) + " to 'float'");
        }
        rtError("invalid conversion", "cannot convert '" + typeName(v) + "' to 'float'");
    }, "(any) -> float");

    defineNative("toStr", [this](VM&, Value* a, int) { return strv(toStr(a[0])); }, "(any) -> str");

    defineNative("toBool", [this](VM&, Value* a, int) {
        Value v = a[0];
        if (v.isBool()) return v;
        if (v.isNull()) return Value::boolean(false);
        if (v.isNum()) return Value::boolean(v.num() != 0);
        if (isStrV(v)) {
            const std::string& s = static_cast<ObjString*>(v.o)->s;
            if (s == "true" || s == "1") return Value::boolean(true);
            if (s == "false" || s == "0" || s.empty()) return Value::boolean(false);
            rtError("invalid conversion", "cannot convert " + reprString(s) + " to 'bool'");
        }
        return Value::boolean(true);
    }, "(any) -> bool");

    defineNative("typeOf", [this](VM&, Value* a, int) { return strv(typeName(a[0])); }, "(any) -> str");

    // math
    defineNative("math.abs", [this](VM&, Value* a, int argc) {
        if (a[0].isInt()) return Value::integer(a[0].i < 0 ? (int64_t)(0ULL - (uint64_t)a[0].i) : a[0].i);
        return Value::number(std::fabs(argNum(*this, a, argc, 0, "math.abs")));
    }, "");
    defineNative("math.min", [this](VM&, Value* a, int argc) {
        if (a[0].isInt() && a[1].isInt()) return a[0].i < a[1].i ? a[0] : a[1];
        double x = argNum(*this, a, argc, 0, "math.min"), y = argNum(*this, a, argc, 1, "math.min");
        return Value::number(x < y ? x : y);
    }, "");
    defineNative("math.max", [this](VM&, Value* a, int argc) {
        if (a[0].isInt() && a[1].isInt()) return a[0].i > a[1].i ? a[0] : a[1];
        double x = argNum(*this, a, argc, 0, "math.max"), y = argNum(*this, a, argc, 1, "math.max");
        return Value::number(x > y ? x : y);
    }, "");
    auto toIntRound = [this](double d) { return convert(Value::number(d), CV_TO_INT); };
    defineNative("math.floor", [this, toIntRound](VM&, Value* a, int argc) {
        return toIntRound(std::floor(argNum(*this, a, argc, 0, "math.floor")));
    }, "(float) -> int");
    defineNative("math.ceil", [this, toIntRound](VM&, Value* a, int argc) {
        return toIntRound(std::ceil(argNum(*this, a, argc, 0, "math.ceil")));
    }, "(float) -> int");
    defineNative("math.round", [this, toIntRound](VM&, Value* a, int argc) {
        return toIntRound(std::round(argNum(*this, a, argc, 0, "math.round")));
    }, "(float) -> int");
    defineNative("math.sqrt", [this](VM&, Value* a, int argc) {
        return Value::number(std::sqrt(argNum(*this, a, argc, 0, "math.sqrt")));
    }, "(float) -> float");
    defineNative("math.pow", [this](VM&, Value* a, int argc) {
        return Value::number(std::pow(argNum(*this, a, argc, 0, "math.pow"), argNum(*this, a, argc, 1, "math.pow")));
    }, "(float, float) -> float");
    defineNative("math.sin", [this](VM&, Value* a, int argc) { return Value::number(std::sin(argNum(*this, a, argc, 0, "math.sin"))); }, "(float) -> float");
    defineNative("math.cos", [this](VM&, Value* a, int argc) { return Value::number(std::cos(argNum(*this, a, argc, 0, "math.cos"))); }, "(float) -> float");
    defineNative("math.tan", [this](VM&, Value* a, int argc) { return Value::number(std::tan(argNum(*this, a, argc, 0, "math.tan"))); }, "(float) -> float");
    defineNative("math.atan2", [this](VM&, Value* a, int argc) {
        return Value::number(std::atan2(argNum(*this, a, argc, 0, "math.atan2"), argNum(*this, a, argc, 1, "math.atan2")));
    }, "(float, float) -> float");
    defineNative("math.clamp", [this](VM&, Value* a, int argc) {
        double v = argNum(*this, a, argc, 0, "math.clamp"), lo = argNum(*this, a, argc, 1, "math.clamp"), hi = argNum(*this, a, argc, 2, "math.clamp");
        return Value::number(v < lo ? lo : v > hi ? hi : v);
    }, "(float, float, float) -> float");
    defineNative("math.lerp", [this](VM&, Value* a, int argc) {
        double x = argNum(*this, a, argc, 0, "math.lerp"), y = argNum(*this, a, argc, 1, "math.lerp"), t = argNum(*this, a, argc, 2, "math.lerp");
        return Value::number(x + (y - x) * t);
    }, "(float, float, float) -> float");
    defineNative("math.random", [](VM&, Value*, int) {
        return Value::number(std::uniform_real_distribution<double>(0.0, 1.0)(rng()));
    }, "() -> float");
    defineNative("math.randomInt", [this](VM&, Value* a, int argc) {
        int64_t lo = argInt(*this, a, argc, 0, "math.randomInt"), hi = argInt(*this, a, argc, 1, "math.randomInt");
        if (hi < lo) rtError("invalid range", "math.randomInt: max is smaller than min");
        return Value::integer(std::uniform_int_distribution<int64_t>(lo, hi)(rng()));
    }, "(int, int) -> int");
    defineNative("math.seed", [this](VM&, Value* a, int argc) {
        rng().seed((uint64_t)argInt(*this, a, argc, 0, "math.seed"));
        return Value();
    }, "(int)");
    {
        ObjNamespace* m = namespaceFor("math", true);
        m->members[str("pi")] = Value::number(3.14159265358979323846);
        m->members[str("inf")] = Value::number(INFINITY);
        nativeSigs["math.pi"] = "float";
        nativeSigs["math.inf"] = "float";
    }

    // str
    defineNative("str.length", [this](VM&, Value* a, int argc) {
        return Value::integer((int64_t)argStr(*this, a, argc, 0, "str.length").size());
    }, "(str) -> int");
    defineNative("str.toUpper", [this](VM&, Value* a, int argc) {
        return strv(utf8Upper(argStr(*this, a, argc, 0, "str.toUpper"), true));
    }, "(str) -> str");
    defineNative("str.toLower", [this](VM&, Value* a, int argc) {
        return strv(utf8Upper(argStr(*this, a, argc, 0, "str.toLower"), false));
    }, "(str) -> str");
    defineNative("str.split", [this](VM&, Value* a, int argc) {
        const std::string& s = argStr(*this, a, argc, 0, "str.split");
        std::string d = argc > 1 ? argStr(*this, a, argc, 1, "str.split") : " ";
        ObjList* l = newList();
        tempRoots.push_back(Value::object(l));
        if (d.empty()) {
            for (char c : s) l->items.push_back(strv(std::string(1, c)));
        } else {
            size_t start = 0, pos;
            while ((pos = s.find(d, start)) != std::string::npos) {
                l->items.push_back(strv(s.substr(start, pos - start)));
                start = pos + d.size();
            }
            l->items.push_back(strv(s.substr(start)));
        }
        tempRoots.pop_back();
        return Value::object(l);
    }, "(str, str=) -> list<str>");
    defineNative("str.trim", [this](VM&, Value* a, int argc) {
        const std::string& s = argStr(*this, a, argc, 0, "str.trim");
        size_t b = s.find_first_not_of(" \t\r\n");
        if (b == std::string::npos) return strv("");
        size_t e = s.find_last_not_of(" \t\r\n");
        return strv(s.substr(b, e - b + 1));
    }, "(str) -> str");
    defineNative("str.contains", [this](VM&, Value* a, int argc) {
        return Value::boolean(argStr(*this, a, argc, 0, "str.contains").find(argStr(*this, a, argc, 1, "str.contains")) != std::string::npos);
    }, "(str, str) -> bool");
    defineNative("str.startsWith", [this](VM&, Value* a, int argc) {
        const std::string& s = argStr(*this, a, argc, 0, "str.startsWith");
        return Value::boolean(s.rfind(argStr(*this, a, argc, 1, "str.startsWith"), 0) == 0);
    }, "(str, str) -> bool");
    defineNative("str.endsWith", [this](VM&, Value* a, int argc) {
        const std::string& s = argStr(*this, a, argc, 0, "str.endsWith");
        const std::string& x = argStr(*this, a, argc, 1, "str.endsWith");
        return Value::boolean(s.size() >= x.size() && s.compare(s.size() - x.size(), x.size(), x) == 0);
    }, "(str, str) -> bool");
    defineNative("str.indexOf", [this](VM&, Value* a, int argc) {
        size_t p = argStr(*this, a, argc, 0, "str.indexOf").find(argStr(*this, a, argc, 1, "str.indexOf"));
        return Value::integer(p == std::string::npos ? -1 : (int64_t)p);
    }, "(str, str) -> int");
    defineNative("str.replace", [this](VM&, Value* a, int argc) {
        std::string s = argStr(*this, a, argc, 0, "str.replace");
        const std::string& from = argStr(*this, a, argc, 1, "str.replace");
        const std::string& to = argStr(*this, a, argc, 2, "str.replace");
        if (from.empty()) return strv(s);
        std::string r;
        size_t start = 0, pos;
        while ((pos = s.find(from, start)) != std::string::npos) {
            r += s.substr(start, pos - start) + to;
            start = pos + from.size();
        }
        r += s.substr(start);
        return strv(r);
    }, "(str, str, str) -> str");
    defineNative("str.sub", [this](VM&, Value* a, int argc) {
        const std::string& s = argStr(*this, a, argc, 0, "str.sub");
        int64_t n = (int64_t)s.size();
        int64_t b = argInt(*this, a, argc, 1, "str.sub");
        int64_t e = argc > 2 ? argInt(*this, a, argc, 2, "str.sub") : n;
        if (b < 0) b += n;
        if (e < 0) e += n;
        b = std::max<int64_t>(0, std::min(b, n));
        e = std::max<int64_t>(b, std::min(e, n));
        return strv(s.substr((size_t)b, (size_t)(e - b)));
    }, "(str, int, int=) -> str");
    defineNative("str.repeat", [this](VM&, Value* a, int argc) {
        const std::string& s = argStr(*this, a, argc, 0, "str.repeat");
        int64_t n = argInt(*this, a, argc, 1, "str.repeat");
        std::string r;
        for (int64_t i = 0; i < n; i++) r += s;
        return strv(r);
    }, "(str, int) -> str");

    // json
    defineNative("json.parse", [this](VM&, Value* a, int argc) {
        const std::string& s = argStr(*this, a, argc, 0, "json.parse");
        JsonParser p {*this, s};
        Value v = p.value(0);
        p.ws();
        if (p.i != s.size()) p.fail("trailing characters");
        return v;
    }, "(str) -> any");
    defineNative("json.stringify", [this](VM&, Value* a, int) {
        std::string out;
        jsonWrite(*this, a[0], out, 0);
        return strv(out);
    }, "(any) -> str");

    // time (non-blocking only; time.sleep is standalone-only)
    defineNative("time.now", [](VM&, Value*, int) {
        auto t = std::chrono::system_clock::now().time_since_epoch();
        return Value::number(std::chrono::duration<double>(t).count());
    }, "() -> float");
    defineNative("time.clock", [](VM&, Value*, int) {
        static auto start = std::chrono::steady_clock::now();
        return Value::number(std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
    }, "() -> float");
    defineNative("time.scriptTime", [this](VM&, Value*, int) { return Value::number(time); }, "() -> float");
}

// ---------------------------------------------------------------------------
// Standalone runner extras: blocking / unrestricted I/O
// ---------------------------------------------------------------------------
void VM::openStandalone() {
    defineNative("input", [this](VM&, Value* a, int argc) {
        if (argc > 0) {
            std::string prompt = toStr(a[0]);
            std::fwrite(prompt.data(), 1, prompt.size(), stdout);
            std::fflush(stdout);
        }
        std::string line;
        if (!std::getline(std::cin, line)) return strv("");
        if (!line.empty() && line.back() == '\r') line.pop_back();
        return strv(line);
    }, "(str=) -> str");
    defineNative("time.sleep", [this](VM&, Value* a, int argc) {
        double s = argNum(*this, a, argc, 0, "time.sleep");
        if (s > 0) std::this_thread::sleep_for(std::chrono::duration<double>(s));
        return Value();
    }, "(float)");
    defineNative("file.read", [this](VM&, Value* a, int argc) {
        const std::string& path = argStr(*this, a, argc, 0, "file.read");
        std::ifstream in(path, std::ios::binary);
        if (!in) rtError("file error", "cannot open '" + path + "' for reading");
        std::stringstream ss;
        ss << in.rdbuf();
        return strv(ss.str());
    }, "(str) -> str");
    defineNative("file.write", [this](VM&, Value* a, int argc) {
        const std::string& path = argStr(*this, a, argc, 0, "file.write");
        std::ofstream o(path, std::ios::binary);
        if (!o) rtError("file error", "cannot open '" + path + "' for writing");
        std::string data = argc > 1 ? toStr(a[1]) : "";
        o.write(data.data(), (std::streamsize)data.size());
        return Value();
    }, "(str, any)");
    defineNative("file.append", [this](VM&, Value* a, int argc) {
        const std::string& path = argStr(*this, a, argc, 0, "file.append");
        std::ofstream o(path, std::ios::binary | std::ios::app);
        if (!o) rtError("file error", "cannot open '" + path + "' for writing");
        std::string data = argc > 1 ? toStr(a[1]) : "";
        o.write(data.data(), (std::streamsize)data.size());
        return Value();
    }, "(str, any)");
    defineNative("file.exists", [this](VM&, Value* a, int argc) {
        std::ifstream in(argStr(*this, a, argc, 0, "file.exists"));
        return Value::boolean((bool)in);
    }, "(str) -> bool");
}

} // namespace tp
