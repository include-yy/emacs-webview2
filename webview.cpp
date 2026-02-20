#include "pch.h"

using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Callback;

namespace utils {
    static auto add(const jsonrpc::json& params) -> jsonrpc::json {
        return params[0].get<double>() + params[1].get<double>();
    }

    static std::wstring utf8_to_wstring(const std::string& utf8_str) {
        if (utf8_str.empty()) {
            return std::wstring();
        }
        // calculate the size of the wide string buffer needed
        int size_needed = MultiByteToWideChar(
            CP_UTF8, 0,
            utf8_str.data(), (int)utf8_str.size(),
            NULL, 0
        );
        // failure case, return empty string
        if (size_needed <= 0) {
            return std::wstring();
        }
        // allocate a buffer for the wide string
        std::wstring wstr(size_needed, 0);
        // perform the actual conversion
        MultiByteToWideChar(
            CP_UTF8, 0,
            utf8_str.data(), (int)utf8_str.size(),
            wstr.data(), size_needed
        );
        return wstr;
    }

    static std::string wstring_to_utf8(const std::wstring& wstr) {
        if (wstr.empty()) {
            return std::string();
        }
        int size_needed = WideCharToMultiByte(
            CP_UTF8, 0,
            wstr.data(), (int)wstr.size(),
            NULL, 0, NULL, NULL
        );
        if (size_needed == 0) {
            return std::string();
        }
        std::string str(size_needed, 0);
        WideCharToMultiByte(
            CP_UTF8, 0,
            wstr.data(), (int)wstr.size(),
            &str[0], size_needed, NULL, NULL
        );
        return str;
    }
}

namespace u = utils;

struct WebViewInstance {
    // Unique ID for this instance, used for mapping and communication with Emacs
    int64_t id{ 0 };
    // WebView COM interfaces
    ComPtr<ICoreWebView2Controller> controller;
    ComPtr<ICoreWebView2> webview;
};

// global instance manager, using a map indexed by ID
// to support multiple buffers on the Emacs side
static std::map<int64_t, std::shared_ptr<WebViewInstance>> g_webviews;

// A simple atomic ID generator for new WebView instances
static int64_t next_webview_id() {
    static std::atomic<int64_t> s_id{ 1 };
    return s_id.fetch_add(1);
}

static void create_webview_instance(HWND parentHwnd, RECT initialBounds, std::wstring url,
    std::function<void(int64_t)> on_created) {
    int64_t newId = next_webview_id();

    CreateCoreWebView2EnvironmentWithOptions(nullptr, nullptr, nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [=](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
                env->CreateCoreWebView2Controller(parentHwnd,
                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [=](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
                            auto instance = std::make_shared<WebViewInstance>();
                            instance->id = newId;
                            instance->controller = controller;
                            instance->controller->get_CoreWebView2(&instance->webview);

                            instance->controller->put_Bounds(initialBounds);
                            instance->controller->put_IsVisible(TRUE);

                            g_webviews[newId] = instance;
                            std::wstring url2 = url;
                            if (url2.empty()) {
                                url2 = std::wstring(L"https://www.example.com");
                            }
                            instance->webview->Navigate(url2.c_str());
                            if (on_created) on_created(newId);
                            return S_OK;
                        }).Get());
                return S_OK;
            }).Get());
}

auto webview_init(jsonrpc::Conn& server) -> void {
    // Initialize COM for the main thread
    (void)CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    auto with_webview = [](auto handler) {
        return [handler](const jsonrpc::json& params) -> jsonrpc::json {
            if (params.empty() || !params[0].is_number_integer()) {
                throw std::runtime_error("Invalid parameters");
            }
            int64_t id = params[0].get<int64_t>();
            auto it = g_webviews.find(id);
            if (it != g_webviews.end()) {
                return handler(it->second, params);
            }
            return false;
            };
        };
    // Example method to add two numbers
    server.register_method("add", u::add);
    // Exit method to stop the server and exit the message loop
    server.register_method("exit", [&server](const jsonrpc::json& params) -> jsonrpc::json {
        PostThreadMessage(GetCurrentThreadId(), WM_QUIT, 0, 0);
        return nullptr;
        });
    server.register_async_method("new", [](jsonrpc::Context ctx, const jsonrpc::json& params) {
        HWND hwnd = (HWND)params[0].get<int64_t>();
        RECT bounds = { params[1][0].get<LONG>(), params[1][1].get<LONG>(),
                        params[1][2].get<LONG>(), params[1][3].get<LONG>() };
        std::string url = params[2].is_null() ? "" : params[2].get<std::string>();
        std::wstring wurl = u::utf8_to_wstring(url);
        create_webview_instance(hwnd, bounds, wurl, [ctx](int64_t id) mutable {
            ctx.reply(id);
            });
        return;
        });
    server.register_method("set-focus", [](const jsonrpc::json& params) -> jsonrpc::json {
        HWND hwnd = (HWND)params[0].get<size_t>();
        // Darkart, use MENU key to work around the SetForegroundWindow restriction in Windows,
        // which requires the caller to be the foreground process or to have received the last
        // input event. By simulating a key press, we can temporarily allow our process to become
        // the foreground window and set focus to the WebView.
        //
        // But actually we found that SetFocus works without SetForegroundWindow, so we can skip
        // that step for better compatibility with different Windows versions and configurations.
        //keybd_event(VK_MENU, 0, 0, 0);
        //auto res = SetForegroundWindow(hwnd);
        //keybd_event(VK_MENU, 0, KEYEVENTF_KEYUP, 0);
        SetFocus(hwnd);
        return nullptr;
        });
    server.register_method("close", [](const jsonrpc::json& params) -> jsonrpc::json {
        int64_t id = params[0].get<int64_t>();
        auto it = g_webviews.find(id);
        if (it != g_webviews.end()) {
            it->second->controller->Close();
            g_webviews.erase(it);
            return true;
        }
        return false;
        });
    server.register_method("resize", with_webview([](auto it, const jsonrpc::json& params) -> jsonrpc::json {
        RECT newBounds = { params[1][0].get<LONG>(), params[1][1].get<LONG>(),
                           params[1][2].get<LONG>(), params[1][3].get<LONG>() };
        it->controller->put_Bounds(newBounds);
        return true;
        }));
    server.register_method("set-visible", with_webview([](auto it, const jsonrpc::json& params) -> jsonrpc::json {
        bool visible = params[1].get<bool>();
        it->controller->put_IsVisible(visible ? TRUE : FALSE);
        return true;
        }));
    server.register_method("reparent", with_webview([](auto it, const jsonrpc::json& params) -> jsonrpc::json {
        HWND newParent = (HWND)params[1].get<int64_t>();
        it->controller->put_ParentWindow(newParent);
        return true;
        }));
    server.register_method("get-title", with_webview([](auto it, const jsonrpc::json& params) -> jsonrpc::json {
        wil::unique_cotaskmem_string title;
        it->webview->get_DocumentTitle(&title);
        return u::wstring_to_utf8(title.get());
        }));
    return;
}
