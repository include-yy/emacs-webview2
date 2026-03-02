#include "pch.h"
#include "wv2_mgmt.h"

#define WM_JSONRPC_MESSAGE (WM_USER + 114514)
std::unique_ptr<AppContext> g_app;

int main() {
    // Initialize COM for the main thread
    (void)CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    g_app = std::make_unique<AppContext>([main_thread_id = GetCurrentThreadId()]() {
        PostThreadMessage(main_thread_id, WM_JSONRPC_MESSAGE, 0, 0);
        });
    webview_init();
    // Start the JSON-RPC server
    g_app->server.start();
    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) {
        if (msg.message == WM_JSONRPC_MESSAGE) {
            g_app->server.process_queue();
            if (!g_app->server.is_running()) {
                break;
            }
        } else {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }
    HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
    if (hIn != INVALID_HANDLE_VALUE) {
        CancelIoEx(hIn, nullptr); // Forcefully abort pending I/O on the reader thread
    }
    // Free resources before COM uninitialize.
    g_app.reset();
    CoUninitialize();
    return 0;
}
