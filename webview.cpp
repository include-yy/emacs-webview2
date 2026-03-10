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

template <typename T>
static T get_opt(const jsonrpc::json& j, const std::string& key, T default_val) {
    if (j.is_object() && j.count(key) && !j[key].is_null()) {
        return j[key].get<T>();
    }
    return default_val;
}

static uint32_t pack_emacs_key(UINT vkey, bool c, bool m, bool s, bool w) {
    uint32_t packed = vkey;
    if (w) packed |= (1 << 23); // Super
    if (s) packed |= (1 << 25); // Shift
    if (c) packed |= (1 << 26); // Control
    if (m) packed |= (1 << 27); // Meta
    return packed;
}

}  // namespace utils

namespace u = utils;

void WebViewInstance::setup_all_events() {
    bind_event<ICoreWebView2DocumentTitleChangedEventHandler>(
        webview,
        &ICoreWebView2::add_DocumentTitleChanged,
        &ICoreWebView2::remove_DocumentTitleChanged,
        &WebViewInstance::on_title_changed
    );
    bind_event<ICoreWebView2AcceleratorKeyPressedEventHandler>(
        controller,
        &ICoreWebView2Controller::add_AcceleratorKeyPressed,
        &ICoreWebView2Controller::remove_AcceleratorKeyPressed,
        &WebViewInstance::on_key_pressed
    );
    bind_event<ICoreWebView2NewWindowRequestedEventHandler>(
        webview,
        &ICoreWebView2::add_NewWindowRequested,
        &ICoreWebView2::remove_NewWindowRequested,
        &WebViewInstance::on_new_window
    );
}

HRESULT WebViewInstance::on_title_changed(ICoreWebView2* sender, IUnknown* args) {
    wil::unique_cotaskmem_string title;
    sender->get_DocumentTitle(&title);

    jsonrpc::json params;
    params["id"] = this->id;
    params["title"] = u::wstring_to_utf8(title.get());
    g_app->server.send_notification("wv/title-changed", params);

    return S_OK;
}

HRESULT WebViewInstance::on_key_pressed(ICoreWebView2Controller* sender, ICoreWebView2AcceleratorKeyPressedEventArgs* args) {
    COREWEBVIEW2_KEY_EVENT_KIND kind;
    args->get_KeyEventKind(&kind);
    if (kind != COREWEBVIEW2_KEY_EVENT_KIND_KEY_DOWN &&
        kind != COREWEBVIEW2_KEY_EVENT_KIND_SYSTEM_KEY_DOWN) {
        return S_OK;
    }
    UINT vkey;
    args->get_VirtualKey(&vkey);

    bool c = GetKeyState(VK_CONTROL) & 0x8000;
    bool m = GetKeyState(VK_MENU) & 0x8000;
    bool s = GetKeyState(VK_SHIFT) & 0x8000;
    bool w = (GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000;
    uint32_t current_packed = u::pack_emacs_key(vkey, c, m, s, w);

    if (intercept_keys.count(current_packed)) {
        args->put_Handled(TRUE);

        jsonrpc::json params;
        params["id"] = this->id;
        params["key"] = current_packed;
        g_app->server.send_notification("input/event", params);
    }
    return S_OK;
}

HRESULT WebViewInstance::on_new_window(ICoreWebView2* sender, ICoreWebView2NewWindowRequestedEventArgs* args) {
    wil::unique_cotaskmem_string uri;
    args->get_Uri(&uri);
    args->put_Handled(TRUE);

    jsonrpc::json params;
    params["url"] = u::wstring_to_utf8(uri.get());
    g_app->server.send_notification("wv/new-window-requested", params);

    return S_OK;
}

void WebViewInstance::Create(WebViewInitParams params) {
    auto env = params.env;
    HWND hwnd = params.hwnd;
    env->CreateCoreWebView2Controller(hwnd, Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
        [p = std::move(params)](HRESULT result, ICoreWebView2Controller* controller) mutable -> HRESULT {
            if (FAILED(result)) {
                p.on_error(result);
                return result;
            }
            auto instance = std::make_shared<WebViewInstance>();
            instance->id = p.id;
            instance->controller = controller;
            instance->controller->get_CoreWebView2(&instance->webview);
            instance->controller->put_Bounds(p.bounds);
            instance->controller->put_IsVisible(p.visible);
            instance->setup_all_events();

            g_app->webviews[p.id] = instance;
            if (!p.url.empty()) {
                instance->webview->Navigate(p.url.c_str());
            }
            p.on_created(p.id);
            return S_OK;
        }).Get());
}

void WebViewInstance::close() {
    for (auto it = cleanup_tasks.rbegin(); it != cleanup_tasks.rend(); it++) {
        (*it)();
    }
    cleanup_tasks.clear();
    if (controller) {
        controller->Close();
        controller = nullptr;
    }
    webview = nullptr;
}

static void handle_env_create(jsonrpc::Context ctx, const jsonrpc::json& params) {
    if (!params.is_object() && !params.is_null()) {
        ctx.error(jsonrpc::spec::kInvalidParams, "Invalid params: expect a config object");
        return;
    }
    std::string env_name = u::get_opt<std::string>(params, "name", "default");
    if (g_app->envs.find(env_name) != g_app->envs.end()) {
        ctx.reply(true);
        return;
    }
    std::wstring user_data_dir = u::utf8_to_wstring(u::get_opt<std::string>(params, "user_data_dir", ""));
    std::wstring lang = u::utf8_to_wstring(u::get_opt<std::string>(params, "language", ""));
    std::wstring args = u::utf8_to_wstring(u::get_opt<std::string>(params, "additional_browser_arguments", ""));

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
    const jsonrpc::json& params2 = params.is_null() ? jsonrpc::json::object() : params;
    int64_t hwnd_val = params2.value("hwnd", 0);
    bool visible_val = params2.value("visible", false);
    auto rect_json = params2.value("bounds", std::vector<long>{0, 0, 0, 0});
    std::string url_value = params2.value("url", "");
    std::string env_name = params2.value("environment", "default");

    WebViewInitParams init_args;
    init_args.id = u::next_webview_id();
    init_args.hwnd = (hwnd_val == 0) ? g_app->dummy_hwnd : (HWND)hwnd_val;
    init_args.visible = FALSE;
    if (hwnd_val != 0 && visible_val) {
        init_args.visible = TRUE;
    }
    init_args.bounds = { 0, 0, 0, 0 };
    if (hwnd_val != 0 && visible_val) {
        auto b = params2["bounds"].get<std::vector<long>>();
        init_args.bounds = { b[0], b[1], b[2], b[3] };
    }
    init_args.url = url_value.empty() ? L"" : u::utf8_to_wstring(url_value);

    auto it = g_app->envs.find(env_name);
    if (it == g_app->envs.end()) {
        ctx.error(jsonrpc::spec::kInternalError, std::format("WebView2 Environment not exist: {}", env_name));
        return;
    }
    init_args.env = it->second;
    init_args.on_created = [ctx](int64_t id) mutable { ctx.reply(id); };
    init_args.on_error = [ctx](HRESULT result) mutable {ctx.error(jsonrpc::spec::kInternalError, "Failed to create controller", std::format("{}", result)); };

    WebViewInstance::Create(std::move(init_args));
}

static void handle_sync_ui_batch(const jsonrpc::json& params) {
    if (!params.is_array()) return;

    auto parse_rect = [](const jsonrpc::json& j) -> RECT {
        RECT rc = { 0, 0, 0, 0 };
        if (j.is_array() && j.size() == 4) {
            rc.left = j[0].get<long>();
            rc.top = j[1].get<long>();
            rc.right = j[2].get<long>();
            rc.bottom = j[3].get<long>();
        }
        return rc;
        };

    for (const auto& item : params) {
        if (!item.is_array() || item.size() < 4) continue;

        int id = item[0].get<int>();
        auto it = g_app->webviews.find(id);
        if (it == g_app->webviews.end()) continue;

        auto& controller = it->second->controller;
        if (!controller) continue;

        bool has_vis_change = !item[1].is_null();
        bool has_rect_change = !item[2].is_null();
        bool has_parent_change = !item[3].is_null();
        int target_vis = has_vis_change ? item[1].get<int>() : -1;

        if (target_vis == FALSE) {
            controller->put_IsVisible(FALSE);
            if (has_parent_change) {
                uint64_t raw_hwnd = item[3].get<uint64_t>();
                HWND target_hwnd = (HWND)raw_hwnd;
                if (target_hwnd == 0) {
                    target_hwnd = g_app->dummy_hwnd;
                }
                controller->put_ParentWindow(target_hwnd);
                controller->NotifyParentWindowPositionChanged();
            }
            if (has_rect_change) {
                controller->put_Bounds(parse_rect(item[2]));
            }
        } else {
            if (has_parent_change) {
                uint64_t raw_hwnd = item[3].get<uint64_t>();
                HWND target_hwnd = (HWND)raw_hwnd;
                if (target_hwnd == 0) target_hwnd = g_app->dummy_hwnd;
                controller->put_ParentWindow(target_hwnd);
                controller->NotifyParentWindowPositionChanged();

            }
            if (has_rect_change) {
                controller->put_Bounds(parse_rect(item[2]));
            }
            if (target_vis == TRUE) {
                controller->put_IsVisible(TRUE);
            }
        }
    }
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
    server.register_method("echo", [](PA params) -> RT {
        return params;
        });
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

        // select-frame-set-input-focus can grab the focus, we don't need this method.
        // SetFocus(hwnd);
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
    server.register_method("wv/set-intercept-keys", with_webview([](WI it, PA params) -> RT {
        it->intercept_keys.clear();
        for (auto& k : params[1]) {
            it->intercept_keys.insert(k.get<uint32_t>());
        }
        return true;
        }));
    server.register_notification("wv/focus", with_webview_n([](WI it, PA) {
        it->controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
        }));
    server.register_notification("wv/navigate", with_webview_n([](WI it, PA params) {
        std::string url = params[1].get<std::string>();
        std::wstring wurl = u::utf8_to_wstring(url);
        it->webview->Navigate(wurl.c_str());
        }));
    server.register_notification("wv/sync-ui-batch", handle_sync_ui_batch);
    server.register_method("wv/ssync-ui-batch", [](PA params) -> RT {
        handle_sync_ui_batch(params);
        return true;
        });
    server.register_method("wv/paste", with_webview([](WI it, PA) {
        // it->controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
        // it->webview->ExecuteScript(L"document.execCommand('paste')", nullptr);
        jsonrpc::json args;
        args["type"] = "rawKeyDown";
        args["windowsVirtualKeyCode"] = 86; // V
        args["modifiers"] = 2;              // Ctrl

        // JSON->str
        std::string json_str = args.dump();
        std::wstring wjson_str = u::utf8_to_wstring(json_str);

        // 3. call CDP
        it->webview->CallDevToolsProtocolMethod(
            L"Input.dispatchKeyEvent",
            wjson_str.c_str(),
            nullptr // no callback
        );
        return true;
        }));
    return;
}
