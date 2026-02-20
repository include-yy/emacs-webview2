#include "pch.h"

auto webview_init(jsonrpc::Conn& server) -> void;

int main() {
    jsonrpc::Conn server;
    webview_init(server);
    // Start the JSON-RPC server
    server.start();
    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) {
        if (msg.message == WM_JSONRPC_MESSAGE) {
            server.process_queue();
        }
        else {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }
    return 0;
}
