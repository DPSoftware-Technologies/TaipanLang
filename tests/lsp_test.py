"""End-to-end test of taipan-lsp: speaks real LSP (JSON-RPC over stdio).

Run:  python tests/lsp_test.py [path/to/taipan-lsp]
"""
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
WORKSPACE = os.path.join(HERE, "lsp")
DEFAULT_SERVER = os.path.join(ROOT, "build", "taipan-lsp" + (".exe" if os.name == "nt" else ""))

failures = 0


def check(cond, what):
    global failures
    if not cond:
        failures += 1
        print("FAIL:", what)


def uri(path):
    p = os.path.abspath(path).replace("\\", "/")
    if p[1:2] == ":":
        p = "/" + p[0].lower() + "%3A" + p[2:]
    return "file://" + p


class Client:
    def __init__(self, exe):
        self.p = subprocess.Popen([exe], stdin=subprocess.PIPE, stdout=subprocess.PIPE)
        self.next_id = 1
        self.notifications = []

    def send(self, msg):
        body = json.dumps(msg).encode("utf-8")
        self.p.stdin.write(b"Content-Length: %d\r\n\r\n" % len(body) + body)
        self.p.stdin.flush()

    def read(self):
        length = 0
        while True:
            line = self.p.stdout.readline()
            if not line:
                raise EOFError("server closed the connection")
            line = line.strip()
            if not line:
                break
            if line.lower().startswith(b"content-length:"):
                length = int(line.split(b":")[1])
        return json.loads(self.p.stdout.read(length))

    def request(self, method, params):
        rid = self.next_id
        self.next_id += 1
        self.send({"jsonrpc": "2.0", "id": rid, "method": method, "params": params})
        while True:
            msg = self.read()
            if msg.get("id") == rid:
                return msg
            self.notifications.append(msg)

    def notify(self, method, params):
        self.send({"jsonrpc": "2.0", "method": method, "params": params})

    def diagnostics(self, doc_uri):
        """Waits for the next publishDiagnostics for doc_uri."""
        while True:
            for i, n in enumerate(self.notifications):
                if n.get("method") == "textDocument/publishDiagnostics" and n["params"]["uri"] == doc_uri:
                    del self.notifications[i]
                    return n["params"]["diagnostics"]
            self.notifications.append(self.read())


def pos(text, needle, delta=0, nth=0):
    """LSP position of the nth occurrence of needle (+delta characters)."""
    idx = -1
    for _ in range(nth + 1):
        idx = text.index(needle, idx + 1)
    idx += delta
    line = text.count("\n", 0, idx)
    col = idx - (text.rfind("\n", 0, idx) + 1)
    return {"line": line, "character": col}


SOURCE = """import helpers

class Enemy {
    int hp = 100
    private int secret = 1
    func takeDamage(int amount) {
        hp -= amount
    }
}

enum Mood { CALM, ANGRY }

func heal(int amount = 10) -> int {
    int total = amount + 1
    return total
}

Enemy goblin = Enemy()
goblin.takeDamage(5)
Mood m = Mood.ANGRY
list<int> xs = [1, 2]
int bad = "oops"
print(helpers.double(2))
entity e = engine.spawn("orc")
e.takeDamage(heal())
"""


def main():
    exe = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_SERVER
    c = Client(exe)
    path = os.path.join(WORKSPACE, "main.tp")
    doc = uri(path)

    init = c.request("initialize", {"processId": None, "rootUri": uri(WORKSPACE), "capabilities": {}})
    caps = init["result"]["capabilities"]
    check(caps.get("hoverProvider") and caps.get("definitionProvider") and caps.get("documentSymbolProvider"),
          "initialize advertises hover/definition/symbols")
    check("." in caps["completionProvider"]["triggerCharacters"], "completion triggers on '.'")
    c.notify("initialized", {})

    # --- diagnostics -------------------------------------------------------
    c.notify("textDocument/didOpen", {"textDocument": {"uri": doc, "languageId": "taipan", "version": 1, "text": SOURCE}})
    diags = c.diagnostics(doc)
    errors = [d for d in diags if d["severity"] == 1]
    check(len(errors) == 1, "exactly one error (got %r)" % [d["message"] for d in errors])
    if errors:
        check("expected 'int', found 'str'" in errors[0]["message"], "type mismatch message")
        check(errors[0]["range"]["start"] == pos(SOURCE, '"oops"'), "error range points at the string")

    # --- hover -------------------------------------------------------------
    def hover(needle, delta=0, nth=0):
        r = c.request("textDocument/hover", {"textDocument": {"uri": doc}, "position": pos(SOURCE, needle, delta, nth)})
        res = r.get("result")
        return res["contents"]["value"] if res else ""

    check("Enemy goblin" in hover("goblin.takeDamage"), "hover on a global variable")
    check("func Enemy.takeDamage(int amount)" in hover("takeDamage(5)"), "hover on a method call")
    check("local int total" in hover("total", nth=1), "hover on a local")
    check("(parameter) int amount" in hover("amount + 1"), "hover on a parameter")
    check("func heal(int amount = ...) -> int" in hover("heal()"), "hover on a function call")
    check("enum member Mood.ANGRY" in hover("ANGRY", nth=1), "hover on an enum member")
    check("func helpers.double(int x) -> int" in hover("double"), "hover on an imported function")
    check("Entity.takeDamage" in hover("takeDamage(heal"), "hover on a native method from the binding file")
    check("(field) int Enemy.hp" in hover("hp -= "), "hover on an implicit-this field")

    # --- go to definition --------------------------------------------------
    def definition(needle, delta=0, nth=0):
        r = c.request("textDocument/definition", {"textDocument": {"uri": doc}, "position": pos(SOURCE, needle, delta, nth)})
        return r.get("result")

    d = definition("takeDamage(5)")
    check(d and d["uri"] == doc and d["range"]["start"] == pos(SOURCE, "takeDamage(int"), "definition of a method")
    d = definition("Enemy()")
    check(d and d["range"]["start"] == pos(SOURCE, "Enemy {"), "definition of a class (constructor call)")
    d = definition("double")
    check(d and d["uri"].endswith("helpers.tp") and d["range"]["start"] == {"line": 0, "character": 5},
          "definition in an imported file")
    d = definition("total", nth=1)
    check(d and d["range"]["start"] == pos(SOURCE, "total ="), "definition of a local")

    # --- document symbols --------------------------------------------------
    syms = c.request("textDocument/documentSymbol", {"textDocument": {"uri": doc}})["result"]
    names = [s["name"] for s in syms]
    check(names[:4] == ["Enemy", "Mood", "heal", "goblin"], "outline order (got %r)" % names[:4])
    enemy = syms[0]
    check([k["name"] for k in enemy.get("children", [])] == ["hp", "secret", "takeDamage"], "class children in outline")

    # --- completion --------------------------------------------------------
    def complete(text, needle, delta):
        c.notify("textDocument/didChange", {"textDocument": {"uri": doc, "version": 2},
                                            "contentChanges": [{"text": text}]})
        c.diagnostics(doc)
        r = c.request("textDocument/completion", {"textDocument": {"uri": doc}, "position": pos(text, needle, delta)})
        return {i["label"]: i for i in r["result"]["items"]}

    items = complete(SOURCE + "goblin.\n", "goblin.\n", 7)
    check("hp" in items and "takeDamage" in items, "member completion on a class instance")
    check("secret" not in items, "private members hidden outside the class")
    check(items.get("takeDamage", {}).get("kind") == 2, "methods have the Method kind")

    items = complete(SOURCE + "xs.ad\n", "xs.ad", 5)
    check("add" in items and "length" in items and "sort" in items, "member completion on a list")

    items = complete(SOURCE + "Mood.\n", "Mood.\n", 5)
    check(set(items) == {"CALM", "ANGRY"}, "member completion on an enum (got %r)" % list(items))

    items = complete(SOURCE + "math.\n", "math.\n", 5)
    check("sqrt" in items and "pi" in items, "member completion on a namespace")

    items = complete(SOURCE + "helpers.\n", "helpers.\n", 8)
    check("double" in items, "member completion on an imported module")

    items = complete(SOURCE + "e.\n", "e.\n", 2)
    check("hp" in items and "takeDamage" in items, "member completion on a native class from bindings")

    text = SOURCE.replace("    return total", "    to\n    return total")
    items = complete(text, "    to\n", 6)
    check("total" in items and "amount" in items, "locals and parameters in scope")
    check("heal" in items and "Enemy" in items and "print" in items and "goblin" in items, "globals and built-ins")
    check("while" in items, "keywords")
    items = complete(text, "Enemy goblin", 0)
    check("total" not in items, "a function's locals are not visible outside it")

    # --- a syntax error keeps the rest working -----------------------------
    broken = SOURCE.replace("Mood m = Mood.ANGRY", "Mood m = = Mood.ANGRY")
    c.notify("textDocument/didChange", {"textDocument": {"uri": doc, "version": 3}, "contentChanges": [{"text": broken}]})
    diags = c.diagnostics(doc)
    check(any("syntax error" in d["message"] for d in diags), "syntax error reported")
    check(len([d for d in diags if d["severity"] == 1]) == 1, "only the syntax error is reported")
    r = c.request("textDocument/hover", {"textDocument": {"uri": doc}, "position": pos(broken, "goblin.takeDamage")})
    check(r.get("result") and "Enemy goblin" in r["result"]["contents"]["value"], "hover still works with a syntax error")

    # --- unsaved edits in another open file are used for imports ------------
    helpers_uri = uri(os.path.join(WORKSPACE, "helpers.tp"))
    c.notify("textDocument/didOpen", {"textDocument": {"uri": helpers_uri, "languageId": "taipan", "version": 1,
                                                       "text": "func double(int x) -> str {\n    return \"\" + x\n}\n"}})
    c.diagnostics(helpers_uri)
    c.notify("textDocument/didChange", {"textDocument": {"uri": doc, "version": 4},
                                        "contentChanges": [{"text": "import helpers\nint n = helpers.double(2)\n"}]})
    diags = c.diagnostics(doc)
    check(any("expected 'int', found 'str'" in d["message"] for d in diags), "uses the unsaved version of an import")

    # --- unknown request + shutdown ----------------------------------------
    r = c.request("textDocument/formatting", {"textDocument": {"uri": doc}})
    check(r.get("error", {}).get("code") == -32601, "unsupported request returns MethodNotFound")
    c.request("shutdown", None)
    c.notify("exit", None)
    code = c.p.wait(timeout=10)
    check(code == 0, "exit code 0 after shutdown")

    if failures == 0:
        print("lsp_test: all checks passed")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
