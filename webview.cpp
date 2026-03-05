#include "pch.h"
#include "wv2_mgmt.h"
#include <WebView2EnvironmentOptions.h>
#include <format>

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

namespace utils {
static auto add(const jsonrpc::json& params) -> jsonrpc::json {
    if (!params.is_array() || params.size() != 2) {
        throw jsonrpc::JsonRpcException(jsonrpc::spec::kInvalidParams, "Argument must be array of length 2");
    }
    if (!params[0].is_number() || !params[1].is_number()) {
        throw jsonrpc::JsonRpcException(jsonrpc::spec::kInvalidParams, "Argument is not number");
    }
    return params[0].get<double>() + params[1].get<double>();
}

static std::wstring utf8_to_wstring(const std::string& utf8_str) {
    if (utf8_str.empty()) {
        return std::wstring();
    }
    // calculate the size of the wide string buffer needed
    int size_needed = MultiByteToWideChar(CP_UTF8, 0, utf8_str.data(),
        (int)utf8_str.size(), NULL, 0);
    // failure case, return empty string
    if (size_needed <= 0) {
        return std::wstring();
    }
    // allocate a buffer for the wide string
    std::wstring wstr(size_needed, 0);
    // perform the actual conversion
    MultiByteToWideChar(CP_UTF8, 0, utf8_str.data(), (int)utf8_str.size(),
        wstr.data(), size_needed);
    return wstr;
}

static std::string wstring_to_utf8(const std::wstring& wstr) {
    if (wstr.empty()) {
        return std::string();
    }
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, wstr.data(),
        (int)wstr.size(), NULL, 0, NULL, NULL);
    if (size_needed == 0) {
        return std::string();
    }
    std::string str(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.data(), (int)wstr.size(), &str[0],
        size_needed, NULL, NULL);
    return str;
}

// A simple atomic ID generator for new WebView instances
static int64_t next_webview_id() {
    static std::atomic<int64_t> s_id{ 1 };
    return s_id.fetch_add(1);
}
}  // namespace utils

namespace u = utils;

template <typename T>
static T get_opt(const jsonrpc::json& j, const std::string& key, T default_val) {
    if (j.is_object() && j.count(key) && !j[key].is_null()) {
        return j[key].get<T>();
    }
    return default_val;
}

static void handle_env_create(jsonrpc::Context ctx, const jsonrpc::json& params) {
    if (!params.is_object() && !params.is_null()) {
        ctx.error(jsonrpc::spec::kInvalidParams, "Invalid params: expect a config object");
        return;
    }
    std::string env_name = get_opt<std::string>(params, "name", "default");
    if (g_app->envs.find(env_name) != g_app->envs.end()) {
        ctx.reply(true);
        return;
    }
    std::wstring user_data_dir = u::utf8_to_wstring(get_opt<std::string>(params, "user_data_dir", ""));
    std::wstring lang = u::utf8_to_wstring(get_opt<std::string>(params, "language", ""));
    std::wstring args = u::utf8_to_wstring(get_opt<std::string>(params, "additional_browser_arguments", ""));

    auto options = Microsoft::WRL::Make<CoreWebView2EnvironmentOptions>();
    if (!lang.empty()) {
        options->put_Language(lang.c_str());
    }
    if (!args.empty()) {
        options->put_AdditionalBrowserArguments(args.c_str());
    }

    CreateCoreWebView2EnvironmentWithOptions(nullptr,
        user_data_dir.empty() ? nullptr : user_data_dir.c_str(),
        options.Get(),
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
        [ctx, env_name](HRESULT result, ICoreWebView2Environment* env) mutable {
            if (FAILED(result) || !env) {
                std::stringstream ss;
                ss << "Failed to create environment (HRESULT: 0x" << std::hex << result << ")";
                ctx.error(jsonrpc::spec::kInternalError, ss.str());
                return result;
            }
            if (g_app) {
                g_app->envs[env_name] = env;
                ctx.reply(true);
            }
            return S_OK;
        }).Get());
}

static void handle_webview_create(jsonrpc::Context ctx, const jsonrpc::json& params) {
    HWND hwnd = (HWND)params[0].get<int64_t>();
    RECT bounds = {
        params[1][0].get<LONG>(), params[1][1].get<LONG>(),
        params[1][2].get<LONG>(), params[1][3].get<LONG>()
    };
    std::wstring url = u::utf8_to_wstring(params[2].is_null() ? "" : params[2].get<std::string>());

    std::string env_name = "default";
    if (params.size() > 3 && !params[3].is_null()) {
        env_name = params[3].get<std::string>();
    }
    auto it = g_app->envs.find(env_name);
    if (it == g_app->envs.end()) {
        ctx.error(jsonrpc::spec::kInternalError, "Environment not found, Call env/create first");
        return;
    }
    int64_t newId = u::next_webview_id();
    it->second->CreateCoreWebView2Controller(hwnd, Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
        [ctx, newId, bounds, url](HRESULT result, ICoreWebView2Controller* controller) mutable -> HRESULT {
            if (FAILED(result) || !controller) {
                ctx.error(jsonrpc::spec::kInternalError, "Failed to create controller");
                return result;
            }

            auto instance = std::make_shared<WebViewInstance>();
            instance->id = newId;
            instance->controller = controller;
            instance->controller->get_CoreWebView2(&instance->webview);
            instance->controller->put_Bounds(bounds);
            instance->controller->put_IsVisible(TRUE);

            g_app->webviews[newId] = instance;
            if (!url.empty()) {
                instance->webview->Navigate(url.c_str());
            } else {
                instance->webview->Navigate(L"https://google.com");
            }
            ctx.reply(newId);
            return S_OK;
        }).Get());
}

using WebViewHandler = std::function<jsonrpc::json(WebViewInstance* inst, const jsonrpc::json& params)>;

static inline jsonrpc::Conn::RequestHandler with_webview(WebViewHandler handler) {
    return [handler](const jsonrpc::json& params) -> jsonrpc::json {
        if (params.empty() || !params[0].is_number_integer()) {
            throw std::runtime_error("Invalid parameters: missing webview ID");
        }

        int64_t id = params[0].get<int64_t>();

        if (g_app) {
            auto it = g_app->webviews.find(id);
            if (it != g_app->webviews.end()) {
                return handler(it->second.get(), params);
            }
        }
        return false;
        };
}

using WebViewNotificationHandler = std::function<void(WebViewInstance* inst, const jsonrpc::json& params)>;

static inline jsonrpc::Conn::NotificationHandler with_webview_n(WebViewNotificationHandler handler) {
    return [handler](const jsonrpc::json& params) {
        if (params.empty() || !params[0].is_number_integer()) {
            throw std::runtime_error("Invalid parameters: missing webview ID");
        }

        int64_t id = params[0].get<int64_t>();

        if (g_app) {
            auto it = g_app->webviews.find(id);
            if (it != g_app->webviews.end()) {
                handler(it->second.get(), params);
            }
        }
        };
}

auto webview_init() -> void {
    using WI = WebViewInstance*;
    using PA = const jsonrpc::json&;
    using RT = jsonrpc::json;
    using CTX = jsonrpc::Context;
    auto& server = g_app->server;
    // Example method to add two numbers
    server.register_method("add", u::add);
    // Exit method to stop the server and exit the message loop
    server.register_notification("app/exit", [](PA) {
        PostThreadMessage(GetCurrentThreadId(), WM_QUIT, 0, 0);
        });
    server.register_notification("app/set-focus", [](PA params) {
        HWND hwnd = (HWND)params[0].get<int64_t>();
        // Darkart, use MENU key to work around the SetForegroundWindow restriction
        // in Windows, which requires the caller to be the foreground process or to
        // have received the last input event. By simulating a key press, we can
        // temporarily allow our process to become the foreground window and set
        // focus to the WebView.
        //
        // But actually we found that SetFocus works without SetForegroundWindow, so
        // we can skip that step for better compatibility with different Windows
        // versions and configurations.
        // keybd_event(VK_MENU, 0, 0, 0);
        // auto res = SetForegroundWindow(hwnd);
        // keybd_event(VK_MENU, 0, KEYEVENTF_KEYUP, 0);
        SetFocus(hwnd);
        });
    // Create WebView2 Environment
    server.register_async_method("env/create", [](CTX ctx, PA params) {
        handle_env_create(ctx, params);
        });
    server.register_method("env/list-names", [](PA) -> RT {
        std::vector<std::string> names;
        for (const auto& pair : g_app->envs) {
            names.push_back(pair.first);
        }
        return names;
        });
    server.register_async_method("wv/create", [](CTX ctx, PA params) {
        handle_webview_create(ctx, params);
        });
    server.register_method("wv/close", [](PA params) -> RT {
        int64_t id = params[0].get<int64_t>();
        return g_app->webviews.erase(id) > 0;
        });
    server.register_notification("wv/resize", with_webview_n([](WI it, PA params) {
        RECT newBounds = {
            params[1][0].get<LONG>(), params[1][1].get<LONG>(),
            params[1][2].get<LONG>(), params[1][3].get<LONG>()
        };
        it->controller->put_Bounds(newBounds);
        }));
    server.register_notification("wv/set-visible", with_webview_n([](WI it, PA params) {
        bool visible = params[1].get<bool>();
        it->controller->put_IsVisible(visible ? TRUE : FALSE);
        }));
    server.register_method("wv/visible-p", with_webview([](WI it, PA params) -> RT {
        BOOL visible = false;
        it->controller->get_IsVisible(&visible);
        return visible == 1;
        }));
    server.register_notification("wv/reparent", with_webview_n([](WI it, PA params) {
        HWND newParent = (HWND)params[1].get<int64_t>();
        it->controller->put_ParentWindow(newParent);
        }));
    server.register_method("wv/get-title", with_webview([](WI it, PA params) -> RT {
        wil::unique_cotaskmem_string title;
        it->webview->get_DocumentTitle(&title);
        return u::wstring_to_utf8(title.get());
        }));
    return;
}
