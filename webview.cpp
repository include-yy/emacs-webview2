#include "pch.h"

using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Callback;

// 模仿 Rust 的 Webview 结构体
struct WebViewInstance {
    int64_t id{ 0 }; // 与 Emacs 侧的 ewv-id 对应
    // 核心组件，必须保留以进行后续操作
    ComPtr<ICoreWebView2Controller> controller;
    ComPtr<ICoreWebView2> webview;

    void resize(RECT newBounds) const {
        if (controller) {
            controller->put_Bounds(newBounds);
        }
    }
};

// 全局实例管理器
// 使用 map 通过 ID 索引，支持 Emacs 端的多 Buffer 对应
static std::map<int64_t, std::shared_ptr<WebViewInstance>> g_webviews;

// ID 生成器，模仿 Rust 的 AtomicI64
static int64_t next_webview_id() {
    static std::atomic<int64_t> s_id{ 1 };
    return s_id.fetch_add(1);
}

static void create_webview_instance(HWND parentHwnd, RECT initialBounds, std::wstring url, 
    std::function<void(int64_t)> on_created) {
    int64_t newId = next_webview_id();

    // 创建环境
    CreateCoreWebView2EnvironmentWithOptions(nullptr, nullptr, nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [=](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
                // 创建控制器
                env->CreateCoreWebView2Controller(parentHwnd,
                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [=](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
                            // 初始化结构体实例
                            auto instance = std::make_shared<WebViewInstance>();
                            instance->id = newId;
                            instance->controller = controller;
                            instance->controller->get_CoreWebView2(&instance->webview);

                            // 配置基础属性
                            instance->controller->put_Bounds(initialBounds);
                            instance->controller->put_IsVisible(TRUE);

                            // 存入全局管理器
                            g_webviews[newId] = instance;
                            std::wstring url2 = url;
                            if (url2.empty()) {
                                url2 = std::wstring(L"https://www.example.com");
                            }
                            instance->webview->Navigate(url2.c_str());
                            // 触发回调，通知 Emacs 实例已准备就绪
                            if (on_created) on_created(newId);

                            return S_OK;
                        }).Get());
                return S_OK;
            }).Get());
}

// 将 UTF-8 std::string 转换为 UTF-16 std::wstring
static std::wstring utf8_to_wstring(const std::string& utf8_str) {
    if (utf8_str.empty()) {
        return std::wstring();
    }

    // 1. 第一次调用：计算转换后需要的宽字符数量
    int size_needed = MultiByteToWideChar(
        CP_UTF8, 0,
        utf8_str.data(), (int)utf8_str.size(),
        NULL, 0
    );

    if (size_needed <= 0) {
        return std::wstring(); // 转换失败处理
    }

    // 2. 分配足够大的 std::wstring
    std::wstring wstr(size_needed, 0);

    // 3. 第二次调用：执行真正的转换，写入到 wstr 中
    MultiByteToWideChar(
        CP_UTF8, 0,
        utf8_str.data(), (int)utf8_str.size(),
        wstr.data(), size_needed
    );

    return wstr;
}

static std::string wstring_to_utf8(const std::wstring& wstr) {
    if (wstr.empty()) return std::string();

    // 第一次调用获取需要的缓冲区大小
    int size_needed = WideCharToMultiByte(
        CP_UTF8, 0, 
        wstr.data(), (int)wstr.size(), 
        NULL, 0, NULL, NULL);

    // 分配空间并进行真正的转换
    std::string strTo(size_needed, 0);
    WideCharToMultiByte(
        CP_UTF8, 0, 
        wstr.data(), (int)wstr.size(),
        &strTo[0], size_needed, NULL, NULL);

    return strTo;
}


static auto add(const jsonrpc::json& params) -> jsonrpc::json {
    return params[0].get<double>() + params[1].get<double>();
}

auto webview_init(jsonrpc::Conn& server) -> void {
    // Initialize COM for the main thread
    (void)CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    // Exit method to stop the server and exit the message loop
    server.register_method("exit", [&server](const jsonrpc::json& params) -> jsonrpc::json {
        PostThreadMessage(GetCurrentThreadId(), WM_QUIT, 0, 0);
        //server.stop();
        return nullptr;
        });
    server.register_async_method("new", [](jsonrpc::Context ctx, const jsonrpc::json& params) {
        // 假设参数 0 是 HWND，参数 1 是 Bounds 数组
        HWND hwnd = (HWND)params[0].get<int64_t>();
        RECT bounds = { params[1][0].get<LONG>(), params[1][1].get<LONG>(), 
            params[1][2].get<LONG>(), params[1][3].get<LONG>() };
        std::string url = params[2].is_null() ? "" : params[2].get<std::string>();
        std::wstring wurl = utf8_to_wstring(url);
        create_webview_instance(hwnd, bounds, wurl, [ctx](int64_t id) mutable {
            ctx.reply(id);
            });
        return;
        });
    server.register_method("set-focus", [](const jsonrpc::json& params) -> jsonrpc::json {
        HWND hwnd = (HWND)params[0].get<size_t>();
        //keybd_event(VK_MENU, 0, 0, 0);
        //auto res = SetForegroundWindow(hwnd);
        //keybd_event(VK_MENU, 0, KEYEVENTF_KEYUP, 0);
        SetFocus(hwnd);
        // return res ? true : false;
        return nullptr;
        });
    server.register_method("close", [](const jsonrpc::json& params) -> jsonrpc::json {
        int64_t id = params[0].get<int64_t>();
        auto it = g_webviews.find(id);
        if (it != g_webviews.end()) {
            // 释放资源，关闭 WebView
            it->second->controller->Close();
            g_webviews.erase(it);
            return true;
        }
        return false;
        });
    server.register_method("resize", [](const jsonrpc::json& params) -> jsonrpc::json {
        int64_t id = params[0].get<int64_t>();
        RECT newBounds = { params[1][0].get<LONG>(), params[1][1].get<LONG>(),
            params[1][2].get<LONG>(), params[1][3].get<LONG>() };
        auto it = g_webviews.find(id);
        if (it != g_webviews.end()) {
            it->second->resize(newBounds);
            return true;
        }
        return false;
        });
    server.register_method("get-title", [](const jsonrpc::json& params) -> jsonrpc::json {
        int64_t id = params[0].get<int64_t>();
        auto it = g_webviews.find(id);
        if (it != g_webviews.end()) {
            wil::unique_cotaskmem_string title;
            it->second->webview->get_DocumentTitle(&title);
            return wstring_to_utf8(title.get());
        }
        return nullptr;
        });
    server.register_method("set-visible", [](const jsonrpc::json& params) -> jsonrpc::json {
        int64_t id = params[0].get<int64_t>();
        bool visible = params[1].get<bool>();
        auto it = g_webviews.find(id);
        if (it != g_webviews.end()) {
            it->second->controller->put_IsVisible(visible ? TRUE : FALSE);
            return true;
        }
        return false;
        });
    server.register_method("reparent", [](const jsonrpc::json& params) -> jsonrpc::json {
        int64_t id = params[0].get<int64_t>();
        HWND newParent = (HWND)params[1].get<int64_t>();
        auto it = g_webviews.find(id);
        if (it != g_webviews.end()) {
            it->second->controller->put_ParentWindow(newParent);
            return true;
        }
        return false;
        });
    // Example method to add two numbers
    server.register_method("add", add);
    
    return;
}