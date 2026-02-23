#include "pch.h"
#include "wv2_mgmt.h"

std::unique_ptr<AppContext> g_app;

int main() {
    // Initialize COM for the main thread
    (void)CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    g_app = std::make_unique<AppContext>();
    webview_init();
    // Start the JSON-RPC server
    g_app->server.start();
    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) {
        if (msg.message == WM_JSONRPC_MESSAGE) {
            g_app->server.process_queue();
        } else {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }
    // Free resources before COM uninitialize.
    g_app.reset();
    CoUninitialize();
    return 0;
}
