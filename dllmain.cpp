#include "pch.h"
#include <Windows.h>
#include <cstdio>
#include <cmath>
#include <vector>
#include <mutex>
#include <d3d12.h>
#include <dxgi1_4.h>
#include "MinHook.h"
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx12.h"
#include <atomic> 

// --- DATA STRUCTURES ---
struct Vector3 { float x, y, z; };
struct Vector2 { float x, y; };

struct ItemData {
    Vector3 position;
    int hash;
    unsigned int fp;
    int sourceOffset;
};

// --- GLOBAL SHARED DATA ---
std::mutex g_DataMutex;
std::vector<ItemData> g_SafeDrawList;
std::atomic<bool> g_Unload = false;
uintptr_t g_GameBase = 0;
bool g_ShowMenu = false;
bool g_ProbeMode = false;

// Config
float g_FOVMultiplier = 1.0f;
float g_DetectionRange = 50000.0f;
bool g_ShowW2SDebug = false;
bool g_ShowPlayerInfo = true;
bool g_ShowRadar = true;
bool g_ShowSupplyESP = false;

// Dummy options
bool g_DummyEnemyESP = false;
bool g_DummyLootESP = false;
bool g_DummyShrineESP = false;

// --- SAFE READ HELPERS ---
bool SafeRead(uintptr_t addr, void* dest, size_t size) {
    if (addr < 0x10000) return false;
    __try {
        memcpy(dest, (void*)addr, size);
        return true;
    }
    __except (1) { return false; }
}

uintptr_t ReadPtr(uintptr_t addr) {
    uintptr_t val = 0;
    SafeRead(addr, &val, sizeof(uintptr_t));
    return val;
}

unsigned int ReadUInt(uintptr_t addr) {
    unsigned int val = 0;
    SafeRead(addr, &val, sizeof(unsigned int));
    return val;
}

int ReadInt(uintptr_t addr) {
    int val = 0;
    SafeRead(addr, &val, sizeof(int));
    return val;
}

// --- MATH & UTILS ---
Vector3 GetPlayerPosition() {
    Vector3 pos = { 0, 0, 0 };
    if (g_GameBase == 0) g_GameBase = (uintptr_t)GetModuleHandleA("GhostOfTsushima.exe");
    uintptr_t ptr = ReadPtr(g_GameBase + 0x017F5F60);
    if (ptr) SafeRead(ptr + 0x50, &pos, sizeof(Vector3));
    return pos;
}

float Distance3D(Vector3 a, Vector3 b) {
    float dx = a.x - b.x;
    float dy = a.y - b.y;
    float dz = a.z - b.z;
    return sqrtf(dx * dx + dy * dy + dz * dz);
}

bool WorldToScreen(Vector3 worldPos, Vector3 playerPos, Vector2& screenPos, float screenWidth, float screenHeight) {
    static uintptr_t viewMatrixAddr = 0;
    if (viewMatrixAddr == 0) viewMatrixAddr = g_GameBase + 0x3487540;

    float matrix[16] = { 0 };
    if (!SafeRead(viewMatrixAddr, matrix, sizeof(matrix))) return false;

    Vector3 camPos = { matrix[12], matrix[13], matrix[14] };
    Vector3 forward = { matrix[0], matrix[1], matrix[2] };
    Vector3 right = { matrix[4], matrix[5], matrix[6] };
    Vector3 up = { matrix[8], matrix[9], matrix[10] };

    float dx = worldPos.x - camPos.x;
    float dy = worldPos.y - camPos.y;
    float dz = worldPos.z - camPos.z;

    float dotForward = dx * forward.x + dy * forward.y + dz * forward.z;
    float dotRight = -(dx * right.x + dy * right.y + dz * right.z);
    float dotUp = -(dx * up.x + dy * up.y + dz * up.z);

    if (dotForward <= 0.1f) return false;

    float baseFOV = 60.0f;
    float fov = baseFOV * g_FOVMultiplier;
    float aspectRatio = screenWidth / screenHeight;
    float fovScale = tanf(fov * 3.14159f / 180.0f / 2.0f);

    screenPos.x = (screenWidth / 2.0f) + (dotRight / (dotForward * fovScale * aspectRatio)) * (screenWidth / 2.0f);
    screenPos.y = (screenHeight / 2.0f) + (dotUp / (dotForward * fovScale)) * (screenHeight / 2.0f);

    return (screenPos.x >= -200 && screenPos.x <= screenWidth + 200 && screenPos.y >= -200 && screenPos.y <= screenHeight + 200);
}

// --- WORKER THREAD ---
DWORD WINAPI ScanThread(LPVOID lpParam) {
    static std::vector<ItemData> backgroundBuffer;
    int currentScanIndex = 0;
    const int CHUNK_SIZE = 5000;
    const int MAX_DEPTH = 150000;

    while (!g_Unload) {
        g_ProbeMode = (GetAsyncKeyState(VK_F3) & 0x8000);

        if (!g_ShowSupplyESP) { Sleep(500); continue; }

        Vector3 playerPos = GetPlayerPosition();
        if (g_GameBase == 0) { Sleep(100); continue; }

        uintptr_t chainBases[] = { g_GameBase + 0x033B4E18, g_GameBase + 0x033B4E30 };

        std::vector<uintptr_t> listOffsets;
        if (g_ProbeMode) {
            // F3 Probe: Scan everything from 0x0 to 0x1000
            for (int i = 0; i < 512; i++) listOffsets.push_back(i * 8);
        }
        else {
            // Standard Scan: All known lists (Including your new find 0x5A0)
            listOffsets = { 0x88, 0x98, 0xB8, 0xC0, 0xC8, 0xD0, 0x2D0, 0x5A0, 0x768 };
        }

        for (uintptr_t baseAddr : chainBases) {
            uintptr_t ptr1 = ReadPtr(baseAddr);
            if (!ptr1) continue;
            uintptr_t ptr2 = (baseAddr == chainBases[0]) ? ReadPtr(ptr1 + 0x718) : ReadPtr(ptr1 + 0x0);
            if (!ptr2) continue;
            uintptr_t ptr3 = ReadPtr(ptr2 + 0x38);
            if (!ptr3) continue;

            for (uintptr_t listOffset : listOffsets) {
                uintptr_t ptr4 = ReadPtr(ptr3 + listOffset);
                if (!ptr4) continue;
                uintptr_t firstItemAddr = ptr4 + 0xA0;

                int loopEnd = (g_ProbeMode) ? 3000 : (currentScanIndex + CHUNK_SIZE);
                if (!g_ProbeMode && loopEnd > MAX_DEPTH) loopEnd = MAX_DEPTH;
                int loopStart = (g_ProbeMode) ? 0 : currentScanIndex;

                for (int i = loopStart; i < loopEnd; i++) {
                    uintptr_t itemAddr = firstItemAddr + (i * 0x180);
                    Vector3 pos = { 0, 0, 0 };

                    if (!SafeRead(itemAddr, &pos, sizeof(Vector3))) continue;
                    if (pos.x == 0 || isnan(pos.x) || fabs(pos.x) > 500000) continue;

                    float dist = Distance3D(playerPos, pos);

                    // Normal Range Check
                    if (!g_ProbeMode && dist > g_DetectionRange) continue;

                    uintptr_t defPtr = ReadPtr(itemAddr + 0x40);
                    unsigned int fp = ReadUInt(defPtr);

                    bool isValid = false;
                    bool isKnownLoot = (fp == 0xA7D07D10 || fp == 0xA79C8B40);

                    // 1. Normal Mode Logic
                    if (!g_ProbeMode && isKnownLoot) isValid = true;

                    // 2. F3 Probe Logic (True Sight)
                    if (g_ProbeMode) {
                        if (isKnownLoot) isValid = true;
                        // [FIX] Restored 6000m range for unknowns!
                        else if (dist < 6000.0f) isValid = true;
                    }

                    if (isValid) {
                        ItemData item;
                        item.position = pos;
                        item.fp = fp;
                        item.hash = ReadInt(itemAddr + 0x98);
                        item.sourceOffset = (int)listOffset;

                        bool exists = false;
                        for (auto& existing : backgroundBuffer) {
                            if (Distance3D(existing.position, pos) < 0.5f) { exists = true; break; }
                        }
                        if (!exists) backgroundBuffer.push_back(item);
                    }
                }
            }
        }

        if (g_ProbeMode) {
            g_DataMutex.lock();
            g_SafeDrawList = backgroundBuffer;
            g_DataMutex.unlock();
            backgroundBuffer.clear();
        }
        else {
            currentScanIndex += CHUNK_SIZE;
            if (currentScanIndex >= MAX_DEPTH) {
                currentScanIndex = 0;
                g_DataMutex.lock();
                g_SafeDrawList = backgroundBuffer;
                g_DataMutex.unlock();
                backgroundBuffer.clear();
            }
        }
        Sleep(10);
    }
    return 0;
}

void DrawTextWithOutline(ImDrawList* draw, const ImVec2& pos, ImU32 textColor, const char* text) {
    ImU32 outlineColor = IM_COL32(0, 0, 0, 255);
    draw->AddText(ImVec2(pos.x - 1, pos.y), outlineColor, text);
    draw->AddText(ImVec2(pos.x + 1, pos.y), outlineColor, text);
    draw->AddText(ImVec2(pos.x, pos.y - 1), outlineColor, text);
    draw->AddText(ImVec2(pos.x, pos.y + 1), outlineColor, text);
    draw->AddText(pos, textColor, text);
}

void RenderESP() {
    ImGuiIO& io = ImGui::GetIO();

    if (g_ShowPlayerInfo) {
        ImGui::SetNextWindowPos(ImVec2(10, 10));
        ImGui::SetNextWindowBgAlpha(0.8f);
        ImGui::Begin("##PlayerInfo", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize);
        if (g_ProbeMode) ImGui::TextColored(ImVec4(1, 0, 1, 1), "!!! TRUE SIGHT (F3) !!!");
        else ImGui::TextColored(ImVec4(0, 1, 0, 1), "Active ESP");
        ImGui::End();
    }

    if (g_ShowSupplyESP) {
        Vector3 playerPos = GetPlayerPosition();
        ImDrawList* draw = ImGui::GetBackgroundDrawList();

        std::vector<ItemData> localDrawList;
        if (g_DataMutex.try_lock()) {
            localDrawList = g_SafeDrawList;
            g_DataMutex.unlock();
        }
        else { return; }

        for (const ItemData& item : localDrawList) {
            float distance = Distance3D(playerPos, item.position);

            if (!g_ProbeMode && distance > g_DetectionRange) continue;

            Vector2 screenPos;
            if (WorldToScreen(item.position, playerPos, screenPos, io.DisplaySize.x, io.DisplaySize.y)) {

                ImU32 color = IM_COL32(255, 255, 255, 255);
                const char* name = "Unknown";
                bool drawItem = false;

                if (item.fp == 0xA7D07D10) {
                    color = IM_COL32(255, 255, 255, 255);
                    name = "Loot";
                    drawItem = true;
                }
                else if (item.fp == 0xA79C8B40) {
                    if (item.hash == 0xA98425A3) { color = IM_COL32(50, 255, 50, 255); name = "BAMBOO"; }
                    else if (item.hash == 0xA51C1E5B) { color = IM_COL32(255, 100, 100, 255); name = "YEW"; }
                    else if (item.hash == 0xC0BEE076) { color = IM_COL32(220, 50, 220, 255); name = "FLOWER"; }
                    else { color = IM_COL32(200, 200, 200, 255); name = "Harvest"; }
                    drawItem = true;
                }

                // If Probing, show unknown items in Cyan
                if (g_ProbeMode) {
                    drawItem = true;
                    if (name == "Unknown") color = IM_COL32(0, 255, 255, 255);
                }

                if (drawItem) {
                    draw->AddCircleFilled(ImVec2(screenPos.x, screenPos.y), 5.0f, color);
                    draw->AddCircle(ImVec2(screenPos.x, screenPos.y), 6.0f, IM_COL32(0, 0, 0, 255), 6, 2.0f);

                    char line1[64], line2[64];
                    if (g_ProbeMode) {
                        // [MODIFIED] SHOW FP (ID) AND OFFSET
                        sprintf_s(line1, "FP:%X Off:0x%X", item.fp, item.sourceOffset);
                        sprintf_s(line2, "Dist: %.0f", distance);
                    }
                    else {
                        sprintf_s(line1, "%s", name);
                        sprintf_s(line2, "%.0fm", distance);
                    }

                    DrawTextWithOutline(draw, ImVec2(screenPos.x + 12, screenPos.y - 18), color, line1);
                    DrawTextWithOutline(draw, ImVec2(screenPos.x + 12, screenPos.y - 4), IM_COL32(255, 255, 255, 255), line2);
                }
            }
        }
    }
}

void RenderMenu() {
    if (!g_ShowMenu) return;
    ImGui::SetNextWindowSize(ImVec2(450, 400), ImGuiCond_FirstUseEver);
    ImGui::Begin("Ghost of Tsushima - Scan ESP", &g_ShowMenu);
    ImGui::TextColored(ImVec4(1, 0, 0, 1), "Press END to Eject");

    if (ImGui::CollapsingHeader("ESP Options", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Checkbox("Show Radar", &g_ShowRadar);
        ImGui::Checkbox("Supply Bundle ESP", &g_ShowSupplyESP);
        ImGui::SliderFloat("Range", &g_DetectionRange, 100.0f, 200000.0f, "%.0fm");
    }
    ImGui::Separator();
    ImGui::TextColored(ImVec4(1, 1, 0, 1), "DEBUG:");
    ImGui::Text("Hold [F3] to see Unknowns (6000m)");
    ImGui::End();
}

ID3D12Device* g_pd3dDevice = nullptr;
ID3D12DescriptorHeap* g_pd3dSrvDescHeap = nullptr;
ID3D12DescriptorHeap* g_pd3dRtvDescHeap = nullptr;
ID3D12CommandQueue* g_pd3dCommandQueue = nullptr;
ID3D12GraphicsCommandList* g_pd3dCommandList = nullptr;
ID3D12CommandAllocator* g_commandAllocators[3] = {};
ID3D12Resource* g_mainRenderTargetResource[3] = {};
D3D12_CPU_DESCRIPTOR_HANDLE g_mainRenderTargetDescriptor[3] = {};
UINT g_buffersCounts = -1;
bool g_Initialized = false;
HWND g_hWnd = nullptr;

typedef void(__stdcall* ExecuteCommandLists)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
ExecuteCommandLists oExecuteCommandLists = nullptr;

void __stdcall hkExecuteCommandLists(ID3D12CommandQueue* queue, UINT NumCommandLists, ID3D12CommandList* const* ppCommandLists) {
    if (!g_pd3dCommandQueue) g_pd3dCommandQueue = queue;
    oExecuteCommandLists(queue, NumCommandLists, ppCommandLists);
}

typedef HRESULT(__stdcall* D3D12Present)(IDXGISwapChain3*, UINT, UINT);
D3D12Present oPresent = nullptr;

extern LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
typedef LRESULT(CALLBACK* WNDPROC)(HWND, UINT, WPARAM, LPARAM);
WNDPROC oWndProc = nullptr;

LRESULT CALLBACK hkWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    if (uMsg == WM_KEYDOWN && wParam == VK_INSERT) g_ShowMenu = !g_ShowMenu;
    if (g_ShowMenu) { ImGui_ImplWin32_WndProcHandler(hWnd, uMsg, wParam, lParam); return true; }
    return CallWindowProc(oWndProc, hWnd, uMsg, wParam, lParam);
}

HRESULT __stdcall hkPresent(IDXGISwapChain3* pSwapChain, UINT SyncInterval, UINT Flags) {
    if (g_Unload) return oPresent(pSwapChain, SyncInterval, Flags);

    if (!g_Initialized) {
        if (SUCCEEDED(pSwapChain->GetDevice(__uuidof(ID3D12Device), (void**)&g_pd3dDevice))) {
            DXGI_SWAP_CHAIN_DESC sd;
            pSwapChain->GetDesc(&sd);
            g_hWnd = sd.OutputWindow;
            g_buffersCounts = sd.BufferCount;

            D3D12_DESCRIPTOR_HEAP_DESC rtvDesc = {};
            rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
            rtvDesc.NumDescriptors = g_buffersCounts;
            rtvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
            rtvDesc.NodeMask = 1;
            g_pd3dDevice->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&g_pd3dRtvDescHeap));

            SIZE_T rtvSize = g_pd3dDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
            D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = g_pd3dRtvDescHeap->GetCPUDescriptorHandleForHeapStart();

            for (UINT i = 0; i < g_buffersCounts; i++) {
                g_mainRenderTargetDescriptor[i] = rtvHandle;
                rtvHandle.ptr += rtvSize;
            }

            D3D12_DESCRIPTOR_HEAP_DESC srvDesc = {};
            srvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            srvDesc.NumDescriptors = 1;
            srvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            g_pd3dDevice->CreateDescriptorHeap(&srvDesc, IID_PPV_ARGS(&g_pd3dSrvDescHeap));

            for (UINT i = 0; i < g_buffersCounts; i++) {
                g_pd3dDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_commandAllocators[i]));
            }

            g_pd3dDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_commandAllocators[0], NULL, IID_PPV_ARGS(&g_pd3dCommandList));
            g_pd3dCommandList->Close();

            for (UINT i = 0; i < g_buffersCounts; i++) {
                pSwapChain->GetBuffer(i, IID_PPV_ARGS(&g_mainRenderTargetResource[i]));
                g_pd3dDevice->CreateRenderTargetView(g_mainRenderTargetResource[i], NULL, g_mainRenderTargetDescriptor[i]);
            }

            ImGui::CreateContext();
            ImGuiIO& io = ImGui::GetIO();
            io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
            unsigned char* pixels;
            int width, height;
            io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
            ImGui::StyleColorsDark();
            ImGui_ImplWin32_Init(g_hWnd);
            ImGui_ImplDX12_Init(g_pd3dDevice, g_buffersCounts, DXGI_FORMAT_R8G8B8A8_UNORM, g_pd3dSrvDescHeap,
                g_pd3dSrvDescHeap->GetCPUDescriptorHandleForHeapStart(), g_pd3dSrvDescHeap->GetGPUDescriptorHandleForHeapStart());
            oWndProc = (WNDPROC)SetWindowLongPtr(g_hWnd, GWLP_WNDPROC, (LONG_PTR)hkWndProc);

            CreateThread(nullptr, 0, ScanThread, nullptr, 0, nullptr);
            g_Initialized = true;
        }
    }

    if (g_Initialized && g_pd3dCommandQueue) {
        ImGui_ImplDX12_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        RenderMenu();
        RenderESP();
        ImGui::Render();

        UINT bufferIndex = pSwapChain->GetCurrentBackBufferIndex();
        g_commandAllocators[bufferIndex]->Reset();
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        barrier.Transition.pResource = g_mainRenderTargetResource[bufferIndex];
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;

        g_pd3dCommandList->Reset(g_commandAllocators[bufferIndex], NULL);
        g_pd3dCommandList->ResourceBarrier(1, &barrier);
        g_pd3dCommandList->OMSetRenderTargets(1, &g_mainRenderTargetDescriptor[bufferIndex], FALSE, NULL);
        g_pd3dCommandList->SetDescriptorHeaps(1, &g_pd3dSrvDescHeap);
        ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), g_pd3dCommandList);
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        g_pd3dCommandList->ResourceBarrier(1, &barrier);
        g_pd3dCommandList->Close();
        ID3D12CommandList* ppCommandLists[] = { g_pd3dCommandList };
        g_pd3dCommandQueue->ExecuteCommandLists(1, ppCommandLists);
    }
    return oPresent(pSwapChain, SyncInterval, Flags);
}

void** GetVTable(void* obj) { return *reinterpret_cast<void***>(obj); }

HWND CreateInvisibleWindow() {
    WNDCLASSEXA wc = {};
    wc.cbSize = sizeof(WNDCLASSEXA);
    wc.lpfnWndProc = DefWindowProc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = "DummyClass";
    RegisterClassExA(&wc);
    return CreateWindowExA(0, "DummyClass", "Dummy", WS_OVERLAPPEDWINDOW, 0, 0, 100, 100, NULL, NULL, wc.hInstance, NULL);
}

DWORD WINAPI MainThread(LPVOID param) {
    if (MH_Initialize() != MH_OK) return 1;
    Sleep(1000);

    HWND dummyWindow = CreateInvisibleWindow();
    IDXGIFactory4* pFactory = nullptr;
    IDXGIAdapter* pAdapter = nullptr;
    ID3D12Device* pDevice = nullptr;
    ID3D12CommandQueue* pCommandQueue = nullptr;
    IDXGISwapChain3* pSwapChain = nullptr;

    CreateDXGIFactory1(IID_PPV_ARGS(&pFactory));
    pFactory->EnumAdapters(0, &pAdapter);
    D3D12CreateDevice(pAdapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&pDevice));

    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    pDevice->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&pCommandQueue));

    DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
    swapChainDesc.BufferCount = 2;
    swapChainDesc.Width = 100;
    swapChainDesc.Height = 100;
    swapChainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swapChainDesc.SampleDesc.Count = 1;

    IDXGISwapChain1* pSwapChain1 = nullptr;
    pFactory->CreateSwapChainForHwnd(pCommandQueue, dummyWindow, &swapChainDesc, nullptr, nullptr, &pSwapChain1);
    pSwapChain1->QueryInterface(IID_PPV_ARGS(&pSwapChain));
    pSwapChain1->Release();

    void** pSwapChainVTable = GetVTable(pSwapChain);
    void** pCommandQueueVTable = GetVTable(pCommandQueue);

    MH_CreateHook(pSwapChainVTable[8], &hkPresent, reinterpret_cast<LPVOID*>(&oPresent));
    MH_EnableHook(pSwapChainVTable[8]);
    MH_CreateHook(pCommandQueueVTable[10], &hkExecuteCommandLists, reinterpret_cast<LPVOID*>(&oExecuteCommandLists));
    MH_EnableHook(pCommandQueueVTable[10]);

    pSwapChain->Release();
    pCommandQueue->Release();
    pDevice->Release();
    pAdapter->Release();
    pFactory->Release();
    DestroyWindow(dummyWindow);

    while (true) {
        if (GetAsyncKeyState(VK_END) & 0x8000) break;
        Sleep(100);
    }

    g_Unload = true;
    Sleep(500);
    if (oWndProc && g_hWnd) SetWindowLongPtr(g_hWnd, GWLP_WNDPROC, (LONG_PTR)oWndProc);
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();

    if (g_Initialized) {
        ImGui_ImplDX12_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
    }

    FreeLibraryAndExitThread((HMODULE)param, 0);
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        CreateThread(nullptr, 0, MainThread, hModule, 0, nullptr);
    }
    return TRUE;
}