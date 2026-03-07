#pragma once

#include <WebView2.h>
#include <Windows.h>
#include <wrl.h>

#include <map>
#include <memory>
#include <unordered_set>
#include "jsonrpc.hpp"

using namespace Microsoft::WRL;

struct WebViewInstance : public std::enable_shared_from_this<WebViewInstance> {
    // Unique ID for this instance, used for mapping and communication with Emacs
    int64_t id{ 0 };
    // WebView COM interfaces
    ComPtr<ICoreWebView2Controller> controller;
    ComPtr<ICoreWebView2> webview;
    // intercept keys
    std::unordered_set<uint32_t> intercept_keys;
    // Callbacks cleanup
    std::vector <std::function<void()>> cleanup_tasks;

    template <typename IHandler, typename... Args>
    auto create_safe_callback(HRESULT(WebViewInstance::* func)(Args...)) {
        std::weak_ptr<WebViewInstance> weak_self = weak_from_this();
        return Callback<IHandler>([weak_self, func](Args... args) -> HRESULT {
            if (auto self = weak_self.lock()) {
                return (self.get()->*func)(std::forward<Args>(args)...);
            }
            return S_OK; // object is freed.
            });
    }

    template <typename IHandler, typename TObj, typename TAdd, typename TRemove, typename TFunc>
    void bind_event(ComPtr<TObj> obj, TAdd add_method, TRemove remove_method, TFunc func) {
        EventRegistrationToken token;
        auto callback = create_safe_callback<IHandler>(func);
        (obj.Get()->*add_method)(callback.Get(), &token);
        cleanup_tasks.push_back([obj, remove_method, token]() {
            (obj.Get()->*remove_method)(token);
            });
    }

    void setup_all_events();
    void close();

    HRESULT on_title_changed(ICoreWebView2* sender, IUnknown* args);
    HRESULT on_key_pressed(ICoreWebView2Controller* sender, ICoreWebView2AcceleratorKeyPressedEventArgs* args);
    HRESULT on_new_window(ICoreWebView2* sender, ICoreWebView2NewWindowRequestedEventArgs* args);

    ~WebViewInstance() { close(); };
};

struct AppContext {
    // JSONRPC server
    jsonrpc::Conn server;
    // WebView2 environments
    std::map<std::string, Microsoft::WRL::ComPtr<ICoreWebView2Environment>> envs;
    // All WebView2 instances
    std::map<int64_t, std::shared_ptr<WebViewInstance>> webviews;

    AppContext(jsonrpc::Conn::Waker waker) : server(std::move(waker)) {}

    ~AppContext() { webviews.clear(); }
};

extern std::unique_ptr<AppContext> g_app;

void webview_init();
