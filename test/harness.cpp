// Minimal D3D12 app used to smoke-test the overlay outside the game.
//
//   test_harness.exe <dll> <out_dir> [steps...]
//
// Steps run in order, one per frame batch:
//   wait:<ms>       keep rendering for <ms> milliseconds
//   key:<vk>        post WM_KEYDOWN/WM_KEYUP for a virtual key (45 = INSERT)
//   env:NAME=VALUE  set an environment variable in this process (the DLL reads ZZ_UI_TEST_*)
//   shot:<name>     save the window contents to <out_dir>/<name>.png
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wincodec.h>

#ifndef PW_RENDERFULLCONTENT
#define PW_RENDERFULLCONTENT 0x00000002
#endif

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

namespace {

constexpr UINT kBuffers = 3;
constexpr int kWidth = 1600;
constexpr int kHeight = 900;

HWND g_hwnd;
ID3D12Device* g_device;
ID3D12CommandQueue* g_queue;
IDXGISwapChain3* g_swapchain;
ID3D12DescriptorHeap* g_rtv_heap;
ID3D12CommandAllocator* g_alloc[kBuffers];
ID3D12GraphicsCommandList* g_list;
ID3D12Resource* g_buffers[kBuffers];
ID3D12Fence* g_fence;
HANDLE g_event;
UINT64 g_fence_value;

LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_DESTROY) PostQuitMessage(0);
    return DefWindowProcW(h, m, w, l);
}

void Check(HRESULT hr, const char* what) {
    if (FAILED(hr)) {
        std::printf("FAILED %s: 0x%08lx\n", what, static_cast<unsigned long>(hr));
        ExitProcess(1);
    }
}

void InitD3D() {
    Check(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g_device)), "device");
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    Check(g_device->CreateCommandQueue(&qd, IID_PPV_ARGS(&g_queue)), "queue");
    IDXGIFactory4* factory = nullptr;
    Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "factory");
    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.BufferCount = kBuffers;
    sd.Width = kWidth;
    sd.Height = kHeight;
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.SampleDesc.Count = 1;
    IDXGISwapChain1* sc1 = nullptr;
    Check(factory->CreateSwapChainForHwnd(g_queue, g_hwnd, &sd, nullptr, nullptr, &sc1), "swapchain");
    Check(sc1->QueryInterface(IID_PPV_ARGS(&g_swapchain)), "swapchain3");
    sc1->Release();
    factory->Release();

    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.NumDescriptors = kBuffers;
    Check(g_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_rtv_heap)), "rtv heap");
    UINT inc = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_CPU_DESCRIPTOR_HANDLE h = g_rtv_heap->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < kBuffers; ++i) {
        Check(g_swapchain->GetBuffer(i, IID_PPV_ARGS(&g_buffers[i])), "buffer");
        g_device->CreateRenderTargetView(g_buffers[i], nullptr, h);
        h.ptr += inc;
        Check(g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                               IID_PPV_ARGS(&g_alloc[i])), "allocator");
    }
    Check(g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_alloc[0], nullptr,
                                      IID_PPV_ARGS(&g_list)), "list");
    g_list->Close();
    Check(g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence)), "fence");
    g_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
}

extern std::wstring g_pending_shot;
extern bool g_shot_ok;
bool CaptureBuffer(UINT i, const std::wstring& path);

void WaitGpu() {
    ++g_fence_value;
    g_queue->Signal(g_fence, g_fence_value);
    if (g_fence->GetCompletedValue() < g_fence_value) {
        g_fence->SetEventOnCompletion(g_fence_value, g_event);
        WaitForSingleObject(g_event, INFINITE);
    }
}

void RenderFrame(int frame) {
    UINT i = g_swapchain->GetCurrentBackBufferIndex();
    g_alloc[i]->Reset();
    g_list->Reset(g_alloc[i], nullptr);
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = g_buffers[i];
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    g_list->ResourceBarrier(1, &b);
    UINT inc = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_CPU_DESCRIPTOR_HANDLE h = g_rtv_heap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += i * inc;
    float pulse = 0.05f * static_cast<float>((frame / 4) % 20) / 20.0f;
    float color[4] = {0.12f + pulse, 0.16f, 0.22f, 1.0f};
    wchar_t bg[8] = {};  // ZZ_BG=0/1: solid black/white, used to recover overlay alpha for screenshots
    if (GetEnvironmentVariableW(L"ZZ_BG", bg, 8)) {
        const float v = bg[0] == L'1' ? 1.0f : 0.0f;
        color[0] = color[1] = color[2] = v;
    }
    g_list->ClearRenderTargetView(h, color, 0, nullptr);
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    g_list->ResourceBarrier(1, &b);
    g_list->Close();
    ID3D12CommandList* lists[] = {g_list};
    g_queue->ExecuteCommandLists(1, lists);
    g_swapchain->Present(1, 0);
    WaitGpu();
    if (!g_pending_shot.empty()) {
        g_shot_ok = CaptureBuffer(i, g_pending_shot);
        g_pending_shot.clear();
    }
}

bool WritePng(const std::wstring& path, const BYTE* bgra, UINT w, UINT h) {
    bool saved = false;
    IWICImagingFactory* wic = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&wic))))
        return false;
    IWICStream* stream = nullptr;
    IWICBitmapEncoder* enc = nullptr;
    IWICBitmapFrameEncode* frame = nullptr;
    if (SUCCEEDED(wic->CreateStream(&stream)) &&
        SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
        SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) &&
        SUCCEEDED(enc->Initialize(stream, WICBitmapEncoderNoCache)) &&
        SUCCEEDED(enc->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr))) {
        frame->SetSize(w, h);
        WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
        frame->SetPixelFormat(&fmt);
        frame->WritePixels(h, w * 4, w * h * 4, const_cast<BYTE*>(bgra));
        frame->Commit();
        enc->Commit();
        saved = true;
    }
    if (frame) frame->Release();
    if (enc) enc->Release();
    if (stream) stream->Release();
    wic->Release();
    return saved;
}

// Copies the buffer that was just presented (still intact with the overlay drawn on it).
bool CaptureBuffer(UINT i, const std::wstring& path) {
    D3D12_RESOURCE_DESC desc = g_buffers[i]->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT64 total = 0;
    g_device->GetCopyableFootprints(&desc, 0, 1, 0, &fp, nullptr, nullptr, &total);
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = total;
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* readback = nullptr;
    if (FAILED(g_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &rd,
                                                 D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                 IID_PPV_ARGS(&readback))))
        return false;
    g_alloc[i]->Reset();
    g_list->Reset(g_alloc[i], nullptr);
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = g_buffers[i];
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    g_list->ResourceBarrier(1, &b);
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = readback;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = fp;
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = g_buffers[i];
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;
    g_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    g_list->ResourceBarrier(1, &b);
    g_list->Close();
    ID3D12CommandList* lists[] = {g_list};
    g_queue->ExecuteCommandLists(1, lists);
    WaitGpu();

    const UINT w = static_cast<UINT>(desc.Width), h = desc.Height;
    std::vector<BYTE> pixels(static_cast<size_t>(w) * h * 4);
    BYTE* mapped = nullptr;
    D3D12_RANGE range{0, static_cast<SIZE_T>(total)};
    bool ok = false;
    if (SUCCEEDED(readback->Map(0, &range, reinterpret_cast<void**>(&mapped)))) {
        for (UINT y = 0; y < h; ++y) {
            const BYTE* row = mapped + fp.Offset + static_cast<size_t>(y) * fp.Footprint.RowPitch;
            BYTE* out = pixels.data() + static_cast<size_t>(y) * w * 4;
            for (UINT x = 0; x < w; ++x) {  // RGBA -> BGRA
                out[x * 4 + 0] = row[x * 4 + 2];
                out[x * 4 + 1] = row[x * 4 + 1];
                out[x * 4 + 2] = row[x * 4 + 0];
                out[x * 4 + 3] = 255;
            }
        }
        D3D12_RANGE none{0, 0};
        readback->Unmap(0, &none);
        ok = WritePng(path, pixels.data(), w, h);
    }
    readback->Release();
    return ok;
}

std::wstring g_pending_shot;
bool g_shot_ok = false;

}  // namespace

double g_worst_ms = 0;
int g_slow_frames = 0;

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) {
        std::printf("usage: test_harness <dll> <out_dir> [steps...]\n");
        return 2;
    }
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"zz_harness";
    RegisterClassExW(&wc);
    RECT r{0, 0, kWidth, kHeight};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    g_hwnd = CreateWindowExW(0, wc.lpszClassName, L"zzostoom harness", WS_OVERLAPPEDWINDOW, 40, 40,
                             r.right - r.left, r.bottom - r.top, nullptr, nullptr, wc.hInstance,
                             nullptr);
    ShowWindow(g_hwnd, SW_SHOWNORMAL);
    InitD3D();

    if (!LoadLibraryW(argv[1])) {
        std::printf("LoadLibrary failed: %lu\n", GetLastError());
        return 1;
    }
    std::wstring out_dir = argv[2];
    int frame = 0;
    auto pump = [&](int ms) {
        auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while (std::chrono::steady_clock::now() < end) {
            MSG msg;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            auto f0 = std::chrono::steady_clock::now();
            RenderFrame(frame++);
            double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - f0).count();
            if (ms > g_worst_ms) g_worst_ms = ms;
            if (ms > 40.0) ++g_slow_frames;
        }
    };
    pump(4000);  // the DLL waits ~1.5 s after the window appears before hooking
    for (int i = 3; i < argc; ++i) {
        std::wstring step = argv[i];
        std::wstring arg = step.substr(step.find(L':') + 1);
        if (step.rfind(L"wait:", 0) == 0) {
            pump(_wtoi(arg.c_str()));
        } else if (step == L"resetstats") {
            g_worst_ms = 0;
            g_slow_frames = 0;
        } else if (step.rfind(L"key:", 0) == 0) {
            WPARAM vk = static_cast<WPARAM>(_wtoi(arg.c_str()));
            PostMessageW(g_hwnd, WM_KEYDOWN, vk, 1);
            PostMessageW(g_hwnd, WM_KEYUP, vk, 0xC0000001);
            pump(300);
        } else if (step.rfind(L"env:", 0) == 0) {
            size_t eq = arg.find(L'=');
            SetEnvironmentVariableW(arg.substr(0, eq).c_str(), arg.substr(eq + 1).c_str());
            pump(400);
        } else if (step.rfind(L"shot:", 0) == 0) {
            pump(200);
            g_pending_shot = out_dir + L"\\" + arg + L".png";
            g_shot_ok = false;
            pump(100);
            std::printf("shot %ls: %s\n", arg.c_str(), g_shot_ok ? "ok" : "FAILED");
        }
    }
    std::printf("frames rendered: %d, worst frame %.1f ms, frames > 40 ms: %d\n", frame,
                g_worst_ms, g_slow_frames);
    DestroyWindow(g_hwnd);
    return 0;
}
