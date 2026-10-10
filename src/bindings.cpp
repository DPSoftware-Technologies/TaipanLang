// bindings.cpp - binding stub files (.tpi).
//
// A game registers its native API (engine.*, Entity, ...) at runtime, so
// tools that never run the engine - the checker and the language server -
// cannot see it. A .tpi file declares that API. taipan_dump_bindings()
// writes one from what the host actually registered, so it never drifts.
#include "internal.h"
#include "parser.h"

#include <algorithm>
#include <fstream>
#include <map>
#include <sstream>

namespace tp {

static bool memberExists(VM& vm, const std::string& qualified) {
    size_t dot = qualified.rfind('.');
    ObjNamespace* ns = dot == std::string::npos ? vm.builtins : vm.namespaceFor(qualified.substr(0, dot), false);
    if (!ns) return false;
    std::string name = dot == std::string::npos ? qualified : qualified.substr(dot + 1);
    for (auto& kv : ns->members) {
        if (kv.first->s == name) return true;
    }
    return false;
}

bool VM::loadBindings(const std::string& path, std::vector<Diagnostic>& diags) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        Diagnostic d;
        d.title = "cannot read file";
        d.label = "could not open binding file '" + path + "'";
        diags.push_back(d);
        return false;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    int fileId = addSource(path, path, ss.str());
    BindingFile file;
    if (!parseBindings(sources[fileId], fileId, file, diags)) return false;

    gcPause++;
    for (auto& ns : file.namespaces) namespaceFor(ns, true);
    // Declarations never replace what the host registered for real.
    for (auto& c : file.classes) {
        auto it = nativeClasses.find(c.name);
        NativeClass* nc;
        if (it != nativeClasses.end()) {
            nc = it->second.get();
        } else {
            auto created = std::make_unique<NativeClass>();
            created->name = c.name;
            nc = created.get();
            nativeClasses[c.name] = std::move(created);
        }
        nativeTypeNames[c.name] = nc;
        for (auto& a : c.aliases) {
            if (std::find(nc->aliases.begin(), nc->aliases.end(), a) == nc->aliases.end()) nc->aliases.push_back(a);
            nativeTypeNames[a] = nc;
        }
        for (auto& p : c.props) {
            if (nc->props.count(p.name)) continue;
            NativeProp np;
            np.type = typeExprText(p.type.get());
            np.writable = p.writable;
            nc->props[p.name] = np;
        }
        nc->isValue |= c.isValue;
        for (auto& m : c.methods) {
            std::string name = m.qualified;
            if (name == "init") {
                if (!nc->ctorSig.empty()) continue;
                nc->ctorSig = bindingSignature(m) + " -> " + c.name;
                ObjNamespace* ns = namespaceFor(c.name, true);
                if (ns->call.t == VT::Null) {
                    std::string q = c.name;
                    ns->call = Value::object(newNative(q, [q](VM& vm, Value*, int) -> Value {
                        vm.rtError("missing native", "constructor of '" + q +
                                                         "' is declared in a binding file but the host did not register it");
                    }));
                }
                continue;
            }
            if (nc->methods.count(name)) continue;
            nc->methods[name] = NativeMethod {nullptr, bindingSignature(m)};
        }
        for (auto& o : c.ops) {
            NativeOp op;
            op.op = o.symbol;
            op.reversed = false;
            if (o.params.empty()) {
                if (o.symbol == "-") op.op = "neg";
            } else if (o.params.size() == 1) {
                op.other = typeExprText(o.params[0].type.get());
            } else {
                op.reversed = true;
                op.other = typeExprText(o.params[0].type.get());
            }
            op.result = o.rets.empty() ? "any" : typeExprText(o.rets[0].get());
            bool exists = false;
            for (auto& e : nc->ops) exists |= e.op == op.op && e.reversed == op.reversed && e.other == op.other;
            if (!exists) nc->ops.push_back(std::move(op));
        }
    }
    for (auto& f : file.funcs) {
        if (memberExists(*this, f.qualified)) continue;
        std::string q = f.qualified;
        defineNative(q, [q](VM& vm, Value*, int) -> Value {
            vm.rtError("missing native", "'" + q + "' is declared in a binding file but the host did not register it");
        }, bindingSignature(f));
    }
    for (auto& v : file.values) {
        if (memberExists(*this, v.qualified)) continue;
        size_t dot = v.qualified.rfind('.');
        ObjNamespace* ns = dot == std::string::npos ? builtins : namespaceFor(v.qualified.substr(0, dot), true);
        ns->members[str(dot == std::string::npos ? v.qualified : v.qualified.substr(dot + 1))] = Value();
        nativeSigs[v.qualified] = typeExprText(v.type.get());
    }
    nsEpoch++;
    gcPause--;
    return true;
}

// "(str, int=) -> Entity" -> "(str a1, int a2 = 0) -> Entity"
static std::string declFromSig(const std::string& name, const std::string& sig) {
    if (sig.empty() || sig[0] != '(') return name + "(any... args) -> any";
    SigParse sp = parseSignature(sig);
    if (!sp.ok) return name + "(any... args) -> any";
    std::string s = name + "(";
    for (size_t i = 0; i < sp.params.size(); i++) {
        if (i) s += ", ";
        std::string t = typeExprText(sp.params[i].get());
        bool variadic = sp.variadic && i + 1 == sp.params.size();
        s += t + (variadic ? "... " : " ") + "a" + std::to_string(i + 1);
        if (sp.optional[i] && !variadic) {
            std::string def = "null";
            if (t == "int") def = "0";
            else if (t == "float") def = "0.0";
            else if (t == "bool") def = "false";
            else if (t == "str") def = "\"\"";
            s += " = " + def;
        }
    }
    s += ")";
    if (sp.rets.size() == 1) {
        s += " -> " + typeExprText(sp.rets[0].get());
    } else if (sp.rets.size() > 1) {
        s += " -> (";
        for (size_t i = 0; i < sp.rets.size(); i++) s += (i ? ", " : "") + typeExprText(sp.rets[i].get());
        s += ")";
    }
    return s;
}

std::string VM::dumpBindings() {
    std::string out = "// Native API of this host - generated by taipan_dump_bindings().\n"
                      "// Editors and `taipan --check --bindings` use it to type-check scripts.\n";
    // functions grouped by namespace; a namespace named after a native class
    // holds that class's static members
    std::map<std::string, std::vector<std::string>> byNs, statics;
    for (auto& q : hostNatives) {
        size_t dot = q.rfind('.');
        std::string ns = dot == std::string::npos ? "" : q.substr(0, dot);
        std::string name = dot == std::string::npos ? q : q.substr(dot + 1);
        auto si = nativeSigs.find(q);
        std::string decl = "func " + declFromSig(name, si != nativeSigs.end() ? si->second : "");
        if (nativeClasses.count(ns)) statics[ns].push_back("static " + decl);
        else byNs[ns].push_back(decl);
    }
    for (auto& q : hostConstants) {
        size_t dot = q.rfind('.');
        auto si = nativeSigs.find(q);
        statics[q.substr(0, dot)].push_back("static " + (si != nativeSigs.end() ? si->second : std::string("any")) + " " +
                                            q.substr(dot + 1));
    }
    for (auto& kv : byNs) {
        std::sort(kv.second.begin(), kv.second.end());
        out += "\n";
        if (kv.first.empty()) {
            for (auto& line : kv.second) out += line + "\n";
        } else {
            out += "namespace " + kv.first + " {\n";
            for (auto& line : kv.second) out += "    " + line + "\n";
            out += "}\n";
        }
    }
    // classes
    std::vector<const NativeClass*> classes;
    for (auto& kv : nativeClasses) {
        if (!kv.second->props.empty() || !kv.second->methods.empty() || !kv.second->aliases.empty() ||
            kv.second->isValue || !kv.second->ops.empty() || statics.count(kv.first)) {
            classes.push_back(kv.second.get());
        }
    }
    std::sort(classes.begin(), classes.end(), [](const NativeClass* a, const NativeClass* b) { return a->name < b->name; });
    for (const NativeClass* c : classes) {
        out += std::string("\n") + (c->isValue ? "value class " : "class ") + c->name;
        for (size_t i = 0; i < c->aliases.size(); i++) out += (i ? ", " : " as ") + c->aliases[i];
        out += " {\n";
        std::vector<std::string> props, methods, ops;
        for (auto& p : c->props) {
            bool ro = !p.second.writable && !c->isValue; // value types are immutable anyway
            props.push_back(std::string(ro ? "readonly " : "") + p.second.type + " " + p.first);
        }
        for (auto& m : c->methods) methods.push_back("func " + declFromSig(m.first, m.second.sig));
        for (auto& o : c->ops) {
            std::string params;
            if (o.op != "neg") params = o.reversed ? o.other + " a1, " + c->name + " a2" : o.other + " a1";
            ops.push_back("operator " + (o.op == "neg" ? std::string("-") : o.op) + "(" + params + ") -> " + o.result);
        }
        std::vector<std::string> st = statics[c->name];
        std::sort(props.begin(), props.end());
        std::sort(methods.begin(), methods.end());
        std::sort(st.begin(), st.end());
        for (auto& p : props) out += "    " + p + "\n";
        if (!c->ctorSig.empty()) {
            std::string ctor = c->ctorSig.substr(0, c->ctorSig.rfind(" -> "));
            out += "    func " + declFromSig("init", ctor) + "\n";
        }
        for (auto& m : methods) out += "    " + m + "\n";
        for (auto& o : ops) out += "    " + o + "\n";
        for (auto& s : st) out += "    " + s + "\n";
        out += "}\n";
    }
    return out;
}

} // namespace tp
