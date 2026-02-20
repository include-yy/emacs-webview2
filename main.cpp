#include "pch.h"
#include "webview.h"

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