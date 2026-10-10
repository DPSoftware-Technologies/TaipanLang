// Taipan for VS Code: starts the taipan-lsp language server for .tp files.
const fs = require("fs");
const path = require("path");
const vscode = require("vscode");
const { LanguageClient, TransportKind } = require("vscode-languageclient/node");

let client;
let output;

function serverPath(context) {
    const configured = vscode.workspace.getConfiguration("taipan").get("server.path");
    if (configured) return configured;
    const exe = process.platform === "win32" ? "taipan-lsp.exe" : "taipan-lsp";
    const bundled = path.join(context.extensionPath, "bin", exe);
    if (fs.existsSync(bundled)) return bundled;
    return "taipan-lsp"; // from PATH
}

function initializationOptions() {
    const cfg = vscode.workspace.getConfiguration("taipan");
    return {
        scriptRoot: cfg.get("scriptRoot"),
        bindings: cfg.get("bindings"),
        standalone: cfg.get("standalone"),
    };
}

async function startClient(context) {
    const command = serverPath(context);
    const serverOptions = {
        run: { command, transport: TransportKind.stdio },
        debug: { command, transport: TransportKind.stdio },
    };
    const clientOptions = {
        documentSelector: [{ scheme: "file", language: "taipan" }],
        initializationOptions: initializationOptions(),
        outputChannel: output,
        synchronize: {
            fileEvents: vscode.workspace.createFileSystemWatcher("**/*.{tp,tpi}"),
        },
    };
    client = new LanguageClient("taipan", "Taipan", serverOptions, clientOptions);
    try {
        await client.start();
    } catch (err) {
        vscode.window.showErrorMessage(
            `Taipan: could not start the language server (${command}). ` +
            "Build it (taipan-lsp) and set 'taipan.server.path', or put it on PATH.");
        client = undefined;
    }
}

async function restartClient(context) {
    if (client) {
        await client.stop();
        client = undefined;
    }
    await startClient(context);
}

function runFile() {
    const editor = vscode.window.activeTextEditor;
    if (!editor || editor.document.languageId !== "taipan") {
        vscode.window.showWarningMessage("Taipan: open a .tp file first.");
        return;
    }
    editor.document.save().then(() => {
        const runtime = vscode.workspace.getConfiguration("taipan").get("runtime.path") || "taipan";
        const terminal = vscode.window.terminals.find((t) => t.name === "Taipan") ||
            vscode.window.createTerminal("Taipan");
        terminal.show(true);
        terminal.sendText(`"${runtime}" "${editor.document.fileName}"`);
    });
}

async function activate(context) {
    output = vscode.window.createOutputChannel("Taipan");
    context.subscriptions.push(output);
    context.subscriptions.push(vscode.commands.registerCommand("taipan.restartServer", () => restartClient(context)));
    context.subscriptions.push(vscode.commands.registerCommand("taipan.runFile", runFile));
    context.subscriptions.push(vscode.workspace.onDidChangeConfiguration((e) => {
        if (e.affectsConfiguration("taipan")) restartClient(context);
    }));
    await startClient(context);
}

async function deactivate() {
    if (client) await client.stop();
}

module.exports = { activate, deactivate };
