#define DIRECTINPUT_VERSION 0x0800
#include "overlay.h"

#include <d3d12.h>
#include <dinput.h>
#include <dxgi1_4.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "MinHook.h"
#include "config.h"
#include "features.h"
#include "fps.h"
#include "imgui.h"
#include "imgui_impl_dx12.h"
#include "imgui_impl_win32.h"
#include "log.h"
#include "memory.h"
#include "pad.h"
#include "ui.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam,
                                                             LPARAM lParam);

namespace zz::overlay {

namespace {

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain3*, UINT, UINT);
using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain3*, UINT, UINT,
                                               const DXGI_PRESENT_PARAMETERS*);
using ResizeBuffersFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain3*, UINT, UINT, UINT,
                                                    DXGI_FORMAT, UINT);
using ResizeBuffers1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain3*, UINT, UINT, UINT,
                                                     DXGI_FORMAT, UINT, const UINT*,
                                                     IUnknown* const*);
using ExecuteCommandListsFn = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT,
                                                       ID3D12CommandList* const*);
using GetDeviceStateFn = HRESULT(STDMETHODCALLTYPE*)(IDirectInputDevice8W*, DWORD, LPVOID);
using GetDeviceDataFn = HRESULT(STDMETHODCALLTYPE*)(IDirectInputDevice8W*, DWORD,
                                                    LPDIDEVICEOBJECTDATA, LPDWORD, DWORD);

PresentFn o_present = nullptr;
Present1Fn o_present1 = nullptr;
ResizeBuffersFn o_resize = nullptr;
ResizeBuffers1Fn o_resize1 = nullptr;
ExecuteCommandListsFn o_execute = nullptr;
GetDeviceStateFn o_get_state = nullptr;
GetDeviceDataFn o_get_data = nullptr;
WNDPROC o_wndproc = nullptr;

std::atomic<bool> g_menu_open{false};
std::atomic<ID3D12CommandQueue*> g_last_direct_queue{nullptr};
thread_local ID3D12CommandQueue* t_last_direct_queue = nullptr;
std::atomic<bool> g_queue_locked{false};
std::atomic<HWND> g_hwnd{nullptr};

struct FrameContext {
    ID3D12Resource* back_buffer = nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv{};
};

// Command allocators used in a ring. The CPU never waits for the GPU: if the oldest one is
// still in use (the GPU is that many frames behind), the overlay skips one frame instead.
// Waiting here used to stall the game's Present for up to a frame while the GPU was busy
// (shader compilation in the first minutes after start).
constexpr UINT kSlots = 8;
struct Slot {
    ID3D12CommandAllocator* allocator = nullptr;
    UINT64 fence_value = 0;
};

struct Renderer {
    bool initialized = false;
    bool failed = false;
    HWND hwnd = nullptr;
    IDXGISwapChain3* swapchain = nullptr;  // not owned
    ID3D12CommandQueue* queue = nullptr;   // not owned
    ID3D12Device* device = nullptr;
    ID3D12DescriptorHeap* rtv_heap = nullptr;
    ID3D12DescriptorHeap* srv_heap = nullptr;
    UINT srv_increment = 0;
    UINT srv_next = 0;
    std::vector<UINT> srv_free;
    ID3D12GraphicsCommandList* cmd_list = nullptr;
    ID3D12Fence* fence = nullptr;
    HANDLE fence_event = nullptr;
    UINT64 fence_counter = 0;
    std::vector<FrameContext> frames;
    Slot slots[kSlots];
    UINT slot_next = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    std::string imgui_ini_utf8;
};
Renderer g_r;
std::recursive_mutex g_render_mutex;

constexpr UINT kSrvHeapSize = 64;

template <typename T>
void SafeRelease(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

void SrvAlloc(ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE* cpu,
              D3D12_GPU_DESCRIPTOR_HANDLE* gpu) {
    UINT index;
    if (!g_r.srv_free.empty()) {
        index = g_r.srv_free.back();
        g_r.srv_free.pop_back();
    } else {
        index = g_r.srv_next < kSrvHeapSize ? g_r.srv_next++ : kSrvHeapSize - 1;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE c = g_r.srv_heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE g = g_r.srv_heap->GetGPUDescriptorHandleForHeapStart();
    cpu->ptr = c.ptr + static_cast<SIZE_T>(index) * g_r.srv_increment;
    gpu->ptr = g.ptr + static_cast<UINT64>(index) * g_r.srv_increment;
}

void SrvFree(ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE cpu,
             D3D12_GPU_DESCRIPTOR_HANDLE) {
    D3D12_CPU_DESCRIPTOR_HANDLE c = g_r.srv_heap->GetCPUDescriptorHandleForHeapStart();
    g_r.srv_free.push_back(static_cast<UINT>((cpu.ptr - c.ptr) / g_r.srv_increment));
}

void WaitForGpu() {
    if (!g_r.queue || !g_r.fence) return;
    const UINT64 value = ++g_r.fence_counter;
    if (FAILED(g_r.queue->Signal(g_r.fence, value))) return;
    if (g_r.fence->GetCompletedValue() < value) {
        g_r.fence->SetEventOnCompletion(value, g_r.fence_event);
        WaitForSingleObject(g_r.fence_event, 2000);
    }
}

void ReleaseRenderTargets() {
    WaitForGpu();
    for (auto& f : g_r.frames) SafeRelease(f.back_buffer);
}

bool CreateFrameResources(IDXGISwapChain3* sc) {
    DXGI_SWAP_CHAIN_DESC desc{};
    if (FAILED(sc->GetDesc(&desc))) return false;
    const UINT count = desc.BufferCount;
    if (count == 0 || count > 16) return false;

    if (g_r.frames.size() != count) {
        SafeRelease(g_r.rtv_heap);
        g_r.frames.assign(count, FrameContext{});
        D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{};
        rtv_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rtv_desc.NumDescriptors = count;
        if (FAILED(g_r.device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&g_r.rtv_heap))))
            return false;
    }
    const UINT rtv_size =
        g_r.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_CPU_DESCRIPTOR_HANDLE handle = g_r.rtv_heap->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < count; ++i) {
        auto& f = g_r.frames[i];
        f.rtv = handle;
        handle.ptr += rtv_size;
        if (FAILED(sc->GetBuffer(i, IID_PPV_ARGS(&f.back_buffer)))) return false;
        g_r.device->CreateRenderTargetView(f.back_buffer, nullptr, f.rtv);
    }
    g_r.format = desc.BufferDesc.Format;
    return true;
}

bool InitImGuiBackend() {
    ImGui_ImplDX12_InitInfo info;
    info.Device = g_r.device;
    info.CommandQueue = g_r.queue;
    info.NumFramesInFlight = static_cast<int>(kSlots);
    info.RTVFormat = g_r.format;
    info.DSVFormat = DXGI_FORMAT_UNKNOWN;
    info.SrvDescriptorHeap = g_r.srv_heap;
    info.SrvDescriptorAllocFn = SrvAlloc;
    info.SrvDescriptorFreeFn = SrvFree;
    return ImGui_ImplDX12_Init(&info);
}

LRESULT CALLBACK HookedWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

bool InitRenderer(IDXGISwapChain3* sc, ID3D12CommandQueue* queue) {
    g_r.swapchain = sc;
    g_r.queue = queue;
    if (FAILED(sc->GetDevice(IID_PPV_ARGS(&g_r.device)))) return false;
    DXGI_SWAP_CHAIN_DESC desc{};
    sc->GetDesc(&desc);
    g_r.hwnd = desc.OutputWindow;
    g_hwnd = g_r.hwnd;

    D3D12_DESCRIPTOR_HEAP_DESC srv_desc{};
    srv_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srv_desc.NumDescriptors = kSrvHeapSize;
    srv_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(g_r.device->CreateDescriptorHeap(&srv_desc, IID_PPV_ARGS(&g_r.srv_heap))))
        return false;
    g_r.srv_increment =
        g_r.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    if (!CreateFrameResources(sc)) return false;
    for (auto& s : g_r.slots)
        if (FAILED(g_r.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                      IID_PPV_ARGS(&s.allocator))))
            return false;
    if (FAILED(g_r.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                             g_r.slots[0].allocator, nullptr,
                                             IID_PPV_ARGS(&g_r.cmd_list))))
        return false;
    g_r.cmd_list->Close();
    if (FAILED(g_r.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_r.fence))))
        return false;
    g_r.fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = g_r.imgui_ini_utf8.empty() ? nullptr : g_r.imgui_ini_utf8.c_str();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;

    RECT rc{};
    GetClientRect(g_r.hwnd, &rc);
    float scale = config::Get().ui_scale;
    if (scale <= 0.0f) {
        float h = static_cast<float>(rc.bottom - rc.top);
        scale = h > 0 ? h / 1080.0f : 1.0f;
        if (scale < 1.0f) scale = 1.0f;
        if (scale > 2.5f) scale = 2.5f;
    }
    ui::Init(scale);

    if (!ImGui_ImplWin32_Init(g_r.hwnd) || !InitImGuiBackend()) return false;

    o_wndproc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(g_r.hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(HookedWndProc)));
    ZZ_LOG("Renderer ready: %u buffers, format %d, %ldx%ld, ui scale %.2f",
           static_cast<unsigned>(g_r.frames.size()), static_cast<int>(g_r.format),
           rc.right - rc.left, rc.bottom - rc.top, scale);
    return true;
}

void UpdateCursor() {
    ImGuiIO& io = ImGui::GetIO();
    if (!g_menu_open) {
        io.MouseDrawCursor = false;
        return;
    }
    CURSORINFO ci{sizeof(ci)};
    bool os_visible = GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING);
    io.MouseDrawCursor = !os_visible;
}

// Controller navigation inside the menu (the game sees an idle pad meanwhile).
void FeedGamepad() {
    ImGuiIO& io = ImGui::GetIO();
    static uint32_t prev = 0;
    pad::RawState raw = g_menu_open ? pad::ReadRaw() : pad::RawState{};
    if (raw.connected)
        io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    else
        io.BackendFlags &= ~ImGuiBackendFlags_HasGamepad;

    uint32_t b = raw.buttons;
    // Buttons already held when the menu opens (View + Start) don't count as new presses,
    // otherwise Start would close the menu in the same frame it opened.
    static bool was_open = false;
    if (g_menu_open && !was_open) prev = b;
    was_open = g_menu_open;
    const uint32_t pressed = b & ~prev;
    prev = b;
    if (pressed & pad::kMenu) {  // Start / Options closes the menu
        SetMenuOpen(false);
        b = 0;
    }
    if (pressed & pad::kLB) ui::SwitchTab(-1);
    if (pressed & pad::kRB) ui::SwitchTab(+1);
    if (b & (pad::kL3 | pad::kR3)) b = 0;  // a combo is being pressed, don't navigate

    io.AddKeyEvent(ImGuiKey_GamepadDpadUp, b & pad::kUp);
    io.AddKeyEvent(ImGuiKey_GamepadDpadDown, b & pad::kDown);
    io.AddKeyEvent(ImGuiKey_GamepadDpadLeft, b & pad::kLeft);
    io.AddKeyEvent(ImGuiKey_GamepadDpadRight, b & pad::kRight);
    io.AddKeyEvent(ImGuiKey_GamepadFaceDown, b & pad::kA);
    io.AddKeyEvent(ImGuiKey_GamepadFaceRight, b & pad::kB);
    io.AddKeyEvent(ImGuiKey_GamepadFaceLeft, b & pad::kX);
    io.AddKeyEvent(ImGuiKey_GamepadFaceUp, b & pad::kY);
    io.AddKeyEvent(ImGuiKey_GamepadBack, b & pad::kView);
    const bool sticks = b == raw.buttons;
    auto axis = [&](ImGuiKey negative, ImGuiKey positive, float v) {
        if (!sticks) v = 0.0f;
        io.AddKeyAnalogEvent(negative, v < -0.25f, v < 0 ? -v : 0.0f);
        io.AddKeyAnalogEvent(positive, v > 0.25f, v > 0 ? v : 0.0f);
    };
    axis(ImGuiKey_GamepadLStickLeft, ImGuiKey_GamepadLStickRight, raw.lx);
    axis(ImGuiKey_GamepadLStickDown, ImGuiKey_GamepadLStickUp, raw.ly);
    axis(ImGuiKey_GamepadRStickLeft, ImGuiKey_GamepadRStickRight, raw.rx);
    axis(ImGuiKey_GamepadRStickDown, ImGuiKey_GamepadRStickUp, raw.ry);
    io.AddKeyAnalogEvent(ImGuiKey_GamepadL2, sticks && raw.lt > 0.3f, sticks ? raw.lt : 0.0f);
    io.AddKeyAnalogEvent(ImGuiKey_GamepadR2, sticks && raw.rt > 0.3f, sticks ? raw.rt : 0.0f);
}

void RenderFrame(IDXGISwapChain3* sc) {
    using clock = std::chrono::steady_clock;
    static clock::time_point last = clock::now();
    const auto now = clock::now();
    const float dt = std::chrono::duration<float>(now - last).count();
    last = now;

    if (pad::TakeMenuToggle()) SetMenuOpen(!g_menu_open);
    if (pad::TakePauseToggle()) features::TogglePause();
    features::Tick(dt, g_menu_open);

    std::lock_guard lock(g_render_mutex);
    if (!g_r.initialized) {
        if (g_r.failed) return;
        ID3D12CommandQueue* queue =
            t_last_direct_queue ? t_last_direct_queue : g_last_direct_queue.load();
        if (!queue) return;  // wait until the game submitted work on a direct queue
        g_queue_locked = true;
        if (!InitRenderer(sc, queue)) {
            ZZ_LOG("Renderer init failed, overlay disabled");
            g_r.failed = true;
            return;
        }
        g_r.initialized = true;
    }
    if (sc != g_r.swapchain) return;  // a different swap chain (e.g. another overlay)
    if (g_r.frames.empty() || !g_r.frames[0].back_buffer) return;

    static bool cursor_forced = false;
    if (g_menu_open) {
        features::SetGameCursorVisible(true);
        cursor_forced = true;
    } else if (cursor_forced) {
        features::SetGameCursorVisible(false);
        cursor_forced = false;
    }
    UpdateCursor();
    FeedGamepad();

    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    ui::Draw(g_menu_open);
    ImGui::Render();

    ImDrawData* draw_data = ImGui::GetDrawData();
    if (!draw_data || draw_data->CmdListsCount == 0) return;  // nothing on screen this frame
    const UINT index = sc->GetCurrentBackBufferIndex();
    if (index >= g_r.frames.size()) return;
    FrameContext& frame = g_r.frames[index];
    if (!frame.back_buffer) return;
    Slot& slot = g_r.slots[g_r.slot_next];
    if (slot.fence_value && g_r.fence->GetCompletedValue() < slot.fence_value) {
        static int skipped = 0;
        if (++skipped % 600 == 1) ZZ_LOG("Overlay: GPU busy, frame skipped (%d so far)", skipped);
        return;
    }
    g_r.slot_next = (g_r.slot_next + 1) % kSlots;
    slot.allocator->Reset();

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = frame.back_buffer;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;

    g_r.cmd_list->Reset(slot.allocator, nullptr);
    g_r.cmd_list->ResourceBarrier(1, &barrier);
    g_r.cmd_list->OMSetRenderTargets(1, &frame.rtv, FALSE, nullptr);
    g_r.cmd_list->SetDescriptorHeaps(1, &g_r.srv_heap);
    ImGui_ImplDX12_RenderDrawData(draw_data, g_r.cmd_list);
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    g_r.cmd_list->ResourceBarrier(1, &barrier);
    g_r.cmd_list->Close();

    ID3D12CommandList* lists[] = {g_r.cmd_list};
    o_execute(g_r.queue, 1, lists);
    slot.fence_value = ++g_r.fence_counter;
    g_r.queue->Signal(g_r.fence, slot.fence_value);
}

void OnPresent(IDXGISwapChain3* sc) {
    fps::OnPresent();
    RenderFrame(sc);
}

HRESULT STDMETHODCALLTYPE HookedPresent(IDXGISwapChain3* sc, UINT sync, UINT flags) {
    if (!(flags & DXGI_PRESENT_TEST)) OnPresent(sc);
    return o_present(sc, sync, flags);
}

HRESULT STDMETHODCALLTYPE HookedPresent1(IDXGISwapChain3* sc, UINT sync, UINT flags,
                                         const DXGI_PRESENT_PARAMETERS* params) {
    if (!(flags & DXGI_PRESENT_TEST)) OnPresent(sc);
    return o_present1(sc, sync, flags, params);
}

void AfterResize(IDXGISwapChain3* sc) {
    DXGI_FORMAT old_format = g_r.format;
    if (!CreateFrameResources(sc)) {
        ZZ_LOG("Recreating render targets failed");
        return;
    }
    if (g_r.format != old_format) {
        ImGui_ImplDX12_Shutdown();
        InitImGuiBackend();
    }
}

HRESULT STDMETHODCALLTYPE HookedResizeBuffers(IDXGISwapChain3* sc, UINT count, UINT w, UINT h,
                                              DXGI_FORMAT fmt, UINT flags) {
    std::lock_guard lock(g_render_mutex);
    const bool ours = g_r.initialized && sc == g_r.swapchain;
    if (ours) ReleaseRenderTargets();
    HRESULT hr = o_resize(sc, count, w, h, fmt, flags);
    if (ours && SUCCEEDED(hr)) AfterResize(sc);
    return hr;
}

HRESULT STDMETHODCALLTYPE HookedResizeBuffers1(IDXGISwapChain3* sc, UINT count, UINT w, UINT h,
                                               DXGI_FORMAT fmt, UINT flags, const UINT* masks,
                                               IUnknown* const* queues) {
    std::lock_guard lock(g_render_mutex);
    const bool ours = g_r.initialized && sc == g_r.swapchain;
    if (ours) ReleaseRenderTargets();
    HRESULT hr = o_resize1(sc, count, w, h, fmt, flags, masks, queues);
    if (ours && SUCCEEDED(hr)) AfterResize(sc);
    return hr;
}

void STDMETHODCALLTYPE HookedExecuteCommandLists(ID3D12CommandQueue* queue, UINT count,
                                                 ID3D12CommandList* const* lists) {
    if (!g_queue_locked) {
        D3D12_COMMAND_QUEUE_DESC desc = queue->GetDesc();
        if (desc.Type == D3D12_COMMAND_LIST_TYPE_DIRECT) {
            t_last_direct_queue = queue;
            g_last_direct_queue = queue;
        }
    }
    o_execute(queue, count, lists);
}

// ---- Input -------------------------------------------------------------------------------

bool IsInputMessage(UINT msg) {
    return msg == WM_INPUT || (msg >= WM_KEYFIRST && msg <= WM_KEYLAST) ||
           (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST);
}

bool g_swallow_char = false;

LRESULT CALLBACK HookedWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    const auto& cfg = config::Get();
    if (msg == WM_CHAR && g_swallow_char) {
        g_swallow_char = false;
        return 0;
    }
    if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) && !(lparam & (1 << 30))) {
        if (config::MatchesKey(cfg.menu_key, wparam, lparam)) {
            SetMenuOpen(!g_menu_open);
            g_swallow_char = true;  // don't type the toggle key into a text box
            return 0;
        }
        if (!g_menu_open && cfg.pause_key_enabled &&
            config::MatchesKey(cfg.pause_key, wparam, lparam)) {
            features::TogglePause();
            return 0;
        }
    }
    if (g_menu_open && ImGui::GetCurrentContext()) {
        ImGui_ImplWin32_WndProcHandler(hwnd, msg, wparam, lparam);
        if (cfg.block_game_input && IsInputMessage(msg)) {
            if (msg == WM_INPUT) return DefWindowProcW(hwnd, msg, wparam, lparam);
            return 0;
        }
    }
    return CallWindowProcW(o_wndproc, hwnd, msg, wparam, lparam);
}

// DirectInput keyboard/mouse devices see "nothing pressed" while the menu is open.
std::mutex g_device_mutex;
std::unordered_map<void*, bool> g_device_is_kbm;

bool IsKeyboardOrMouse(IDirectInputDevice8W* dev) {
    std::lock_guard lock(g_device_mutex);
    auto it = g_device_is_kbm.find(dev);
    if (it != g_device_is_kbm.end()) return it->second;
    DIDEVCAPS caps{};
    caps.dwSize = sizeof(caps);
    bool kbm = false;
    if (SUCCEEDED(dev->GetCapabilities(&caps))) {
        BYTE type = GET_DIDEVICE_TYPE(caps.dwDevType);
        kbm = type == DI8DEVTYPE_KEYBOARD || type == DI8DEVTYPE_MOUSE;
    }
    g_device_is_kbm[dev] = kbm;
    return kbm;
}

bool ShouldBlock() { return g_menu_open && config::Get().block_game_input; }

HRESULT STDMETHODCALLTYPE HookedGetDeviceState(IDirectInputDevice8W* dev, DWORD size, LPVOID data) {
    HRESULT hr = o_get_state(dev, size, data);
    if (SUCCEEDED(hr) && data && ShouldBlock() && IsKeyboardOrMouse(dev)) std::memset(data, 0, size);
    return hr;
}

HRESULT STDMETHODCALLTYPE HookedGetDeviceData(IDirectInputDevice8W* dev, DWORD object_size,
                                              LPDIDEVICEOBJECTDATA data, LPDWORD in_out,
                                              DWORD flags) {
    HRESULT hr = o_get_data(dev, object_size, data, in_out, flags);
    if (SUCCEEDED(hr) && in_out && !(flags & DIGDD_PEEK) && ShouldBlock() && IsKeyboardOrMouse(dev))
        *in_out = 0;
    return hr;
}

// ---- Hook installation --------------------------------------------------------------------

bool Hook(void* target, void* detour, void** original, const char* name) {
    if (!target) {
        ZZ_LOG("Hook %s: target missing", name);
        return false;
    }
    MH_STATUS s = MH_CreateHook(target, detour, original);
    if (s != MH_OK) {
        ZZ_LOG("Hook %s: MH_CreateHook failed (%d)", name, static_cast<int>(s));
        return false;
    }
    return true;
}

LRESULT CALLBACK DummyWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    return DefWindowProcW(h, m, w, l);
}

bool HookDx12() {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = DummyWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"zzostoom_qol_dummy";
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64,
                                nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) return false;

    bool ok = false;
    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    IDXGIFactory2* factory = nullptr;
    IDXGISwapChain1* swapchain = nullptr;
    do {
        if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)))) break;
        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (FAILED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)))) break;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) break;
        DXGI_SWAP_CHAIN_DESC1 sd{};
        sd.BufferCount = 2;
        sd.Width = 64;
        sd.Height = 64;
        sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        sd.SampleDesc.Count = 1;
        if (FAILED(factory->CreateSwapChainForHwnd(queue, hwnd, &sd, nullptr, nullptr, &swapchain)))
            break;
        void** sc_vt = *reinterpret_cast<void***>(swapchain);
        void** q_vt = *reinterpret_cast<void***>(queue);
        ok = Hook(sc_vt[8], reinterpret_cast<void*>(HookedPresent),
                  reinterpret_cast<void**>(&o_present), "Present") &&
             Hook(sc_vt[13], reinterpret_cast<void*>(HookedResizeBuffers),
                  reinterpret_cast<void**>(&o_resize), "ResizeBuffers") &&
             Hook(q_vt[10], reinterpret_cast<void*>(HookedExecuteCommandLists),
                  reinterpret_cast<void**>(&o_execute), "ExecuteCommandLists");
        Hook(sc_vt[22], reinterpret_cast<void*>(HookedPresent1),
             reinterpret_cast<void**>(&o_present1), "Present1");
        IDXGISwapChain3* sc3 = nullptr;
        if (SUCCEEDED(swapchain->QueryInterface(IID_PPV_ARGS(&sc3)))) {
            void** vt3 = *reinterpret_cast<void***>(sc3);
            Hook(vt3[39], reinterpret_cast<void*>(HookedResizeBuffers1),
                 reinterpret_cast<void**>(&o_resize1), "ResizeBuffers1");
            sc3->Release();
        }
    } while (false);

    SafeRelease(swapchain);
    SafeRelease(factory);
    SafeRelease(queue);
    SafeRelease(device);
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return ok;
}

void HookDirectInput() {
    HMODULE dinput = LoadLibraryW(L"dinput8.dll");
    if (!dinput) return;
    using CreateFn = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
    auto create = reinterpret_cast<CreateFn>(GetProcAddress(dinput, "DirectInput8Create"));
    if (!create) return;
    IDirectInput8W* di = nullptr;
    if (FAILED(create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8W,
                      reinterpret_cast<void**>(&di), nullptr)))
        return;
    IDirectInputDevice8W* keyboard = nullptr;
    if (SUCCEEDED(di->CreateDevice(GUID_SysKeyboard, &keyboard, nullptr))) {
        void** vt = *reinterpret_cast<void***>(keyboard);
        Hook(vt[9], reinterpret_cast<void*>(HookedGetDeviceState),
             reinterpret_cast<void**>(&o_get_state), "DI GetDeviceState");
        Hook(vt[10], reinterpret_cast<void*>(HookedGetDeviceData),
             reinterpret_cast<void**>(&o_get_data), "DI GetDeviceData");
        keyboard->Release();
    }
    di->Release();
}

}  // namespace

bool MenuOpen() { return g_menu_open; }
HWND GameWindow() { return g_hwnd; }
bool GameFocused() {
    HWND h = g_hwnd;
    return h && GetForegroundWindow() == h;
}

void SetMenuOpen(bool open) {
    g_menu_open = open;
    pad::SetMenuOpen(open);
    ui::OnMenuToggled(open);
}

bool Install() {
    wchar_t dll_path[MAX_PATH];
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&Install), &self);
    GetModuleFileNameW(self, dll_path, MAX_PATH);
    std::wstring dir(dll_path);
    dir = dir.substr(0, dir.find_last_of(L"\\/"));
    const std::wstring ini = dir + L"\\zzostoom_qol_layout.ini";
    int n = WideCharToMultiByte(CP_UTF8, 0, ini.c_str(), -1, nullptr, 0, nullptr, nullptr);
    g_r.imgui_ini_utf8.assign(n > 0 ? n - 1 : 0, '\0');
    WideCharToMultiByte(CP_UTF8, 0, ini.c_str(), -1, g_r.imgui_ini_utf8.data(), n, nullptr,
                        nullptr);

    MH_STATUS s = MH_Initialize();
    if (s != MH_OK && s != MH_ERROR_ALREADY_INITIALIZED) {
        ZZ_LOG("MH_Initialize failed (%d)", static_cast<int>(s));
        return false;
    }
    if (!HookDx12()) {
        ZZ_LOG("DX12 hook setup failed");
        return false;
    }
    HookDirectInput();
    s = MH_EnableHook(MH_ALL_HOOKS);
    ZZ_LOG("Hooks enabled (%d)", static_cast<int>(s));
    return s == MH_OK;
}

}  // namespace zz::overlay
