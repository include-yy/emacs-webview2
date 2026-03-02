#pragma once

#include <WebView2.h>
#include <Windows.h>
#include <wrl.h>

#include <map>
#include <memory>

#include "jsonrpc.hpp"

using namespace Microsoft::WRL;

struct WebViewInstance {
  // Unique ID for this instance, used for mapping and communication with Emacs
  int64_t id{0};
  // WebView COM interfaces
  ComPtr<ICoreWebView2Controller> controller;
  ComPtr<ICoreWebView2> webview;

  ~WebViewInstance() {
    if (controller) {
      controller->Close();
    }
  }
};

struct AppContext {
  // JSONRPC server
  jsonrpc::Conn server;
  // All WebView2 instances
  std::map<int64_t, std::shared_ptr<WebViewInstance>> webviews;

  AppContext(jsonrpc::Conn::Waker waker) : server(std::move(waker)) {}

  ~AppContext() { webviews.clear(); }
};

extern std::unique_ptr<AppContext> g_app;

void webview_init();
