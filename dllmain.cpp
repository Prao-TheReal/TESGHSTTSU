#include "pch.h"
#include <Windows.h>
#include <cstdio>
#include <cmath>
#include <vector>
#include <d3d12.h>
#include <dxgi1_4.h>
#include "MinHook.h"
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx12.h"

// Player position - POINTER CHAIN
uintptr_t g_GameBase = 0;

struct Vector3 { float x, y, z; };
struct Vector2 { float x, y; };

// Structure to hold item data including memory address
struct ItemData {
    Vector3 position;
    uintptr_t address;
};

Vector3 GetPlayerPosition() {
    Vector3 pos = { 0, 0, 0 };

    // Get game base address dynamically
    if (g_GameBase == 0) {
        g_GameBase = (uintptr_t)GetModuleHandleA("GhostOfTsushima.exe");
        printf("[GoTCheat] Game base address: 0x%llX\n", g_GameBase);
    }

    __try {
        // Follow pointer chain: [base+0x017F5F60]+0x50 = player coords (UPDATED for patch)
        uintptr_t ptrAddress = g_GameBase + 0x017F5F60;
        uintptr_t ptr = *(uintptr_t*)(ptrAddress);

        if (ptr) {
            uintptr_t playerAddr = ptr + 0x50;
            pos.x = *(float*)(playerAddr);
            pos.y = *(float*)(playerAddr + 0x4);
            pos.z = *(float*)(playerAddr + 0x8);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        // Error accessing memory
    }
    return pos;
}

// Calculate distance between two points
float Distance3D(Vector3 a, Vector3 b) {
    float dx = a.x - b.x;
    float dy = a.y - b.y;
    float dz = a.z - b.z;
    return sqrtf(dx * dx + dy * dy + dz * dz);
}

// Forward declarations for globals used in WorldToScreen
extern float g_FOVMultiplier;
extern float g_DetectionRange;
extern bool g_ShowW2SDebug;

// Real WorldToScreen using view matrix!
bool WorldToScreen(Vector3 worldPos, Vector3 playerPos, Vector2& screenPos, float screenWidth, float screenHeight) {
    // View matrix is at a static offset!
    static uintptr_t viewMatrixAddr = 0;
    if (viewMatrixAddr == 0) {
        viewMatrixAddr = g_GameBase + 0x3487540;  // NEW matrix address!
    }

    // Read the 4x4 view matrix
    float matrix[16] = { 0 };

    __try {
        for (int i = 0; i < 16; i++) {
            matrix[i] = *(float*)(viewMatrixAddr + (i * 4));
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }

    // Extract camera position from matrix (standard: last row)
    Vector3 camPos = { matrix[12], matrix[13], matrix[14] };

    // Extract view vectors (try swapping forward/right)
    Vector3 forward = { matrix[0], matrix[1], matrix[2] };  // Try row 0 as forward
    Vector3 right = { matrix[4], matrix[5], matrix[6] };     // Try row 1 as right  
    Vector3 up = { matrix[8], matrix[9], matrix[10] };       // Try row 2 as up

    // Calculate relative position
    float dx = worldPos.x - camPos.x;
    float dy = worldPos.y - camPos.y;
    float dz = worldPos.z - camPos.z;

    // Project onto camera axes (try negating both)
    float dotForward = dx * forward.x + dy * forward.y + dz * forward.z;
    float dotRight = -(dx * right.x + dy * right.y + dz * right.z);  // Negated
    float dotUp = -(dx * up.x + dy * up.y + dz * up.z);  // Negated

    // Behind camera check
    if (dotForward <= 0.1f) return false;

    // Perspective projection using adjustable FOV
    float baseFOV = 60.0f; // Base FOV in degrees
    float fov = baseFOV * g_FOVMultiplier; // Use slider value
    float aspectRatio = screenWidth / screenHeight;
    float fovRadians = fov * 3.14159f / 180.0f;
    float fovScale = tanf(fovRadians / 2.0f);

    // Project to screen space
    screenPos.x = (screenWidth / 2.0f) + (dotRight / (dotForward * fovScale * aspectRatio)) * (screenWidth / 2.0f);
    screenPos.y = (screenHeight / 2.0f) + (dotUp / (dotForward * fovScale)) * (screenHeight / 2.0f);  // Inverted Y

    // Temporary offset adjustment
    screenPos.y -= 100.0f;  // Pull dots up by 100px

    // Check if on screen (with generous margins for ultrawide)
    return (screenPos.x >= -200 && screenPos.x <= screenWidth + 200 &&
        screenPos.y >= -200 && screenPos.y <= screenHeight + 200);
}

// Global state
bool g_ShowMenu = false;
bool g_Initialized = false;
HWND g_hWnd = nullptr;

// Menu options
bool g_ShowPlayerInfo = true;
bool g_ShowRadar = true;
bool g_ShowSupplyESP = false;  // Supply Bundle ESP toggle
float g_FOVMultiplier = 1.0f;  // FOV adjustment slider
float g_DetectionRange = 1000.0f;  // Detection range in meters
bool g_ShowW2SDebug = false;   // WorldToScreen debug output

// Dummy bools for disabled "Coming Soon" checkboxes
bool g_DummyEnemyESP = false;
bool g_DummyLootESP = false;
bool g_DummyShrineESP = false;

// DirectX 12 objects
ID3D12Device* g_pd3dDevice = nullptr;
ID3D12DescriptorHeap* g_pd3dSrvDescHeap = nullptr;
ID3D12DescriptorHeap* g_pd3dRtvDescHeap = nullptr;
ID3D12CommandQueue* g_pd3dCommandQueue = nullptr;
ID3D12GraphicsCommandList* g_pd3dCommandList = nullptr;
ID3D12CommandAllocator* g_commandAllocators[3] = {};
ID3D12Resource* g_mainRenderTargetResource[3] = {};
D3D12_CPU_DESCRIPTOR_HANDLE g_mainRenderTargetDescriptor[3] = {};
UINT g_buffersCounts = -1;

extern LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
typedef LRESULT(CALLBACK* WNDPROC)(HWND, UINT, WPARAM, LPARAM);
WNDPROC oWndProc = nullptr;

LRESULT CALLBACK hkWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    if (uMsg == WM_KEYDOWN && wParam == VK_INSERT) {
        g_ShowMenu = !g_ShowMenu;
        printf("[GoTCheat] Menu toggled: %s\n", g_ShowMenu ? "ON" : "OFF");
    }

    if (g_ShowMenu) {
        ImGui_ImplWin32_WndProcHandler(hWnd, uMsg, wParam, lParam);
        return true;
    }

    return CallWindowProc(oWndProc, hWnd, uMsg, wParam, lParam);
}

void RenderMenu() {
    if (!g_ShowMenu) return;

    ImGui::SetNextWindowSize(ImVec2(450, 400), ImGuiCond_FirstUseEver);
    ImGui::Begin("Ghost of Tsushima - Scan ESP", &g_ShowMenu);

    ImGui::TextColored(ImVec4(0, 1, 0, 1), "Press INSERT to toggle menu");
    ImGui::Separator();

    if (ImGui::CollapsingHeader("Player Info", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Checkbox("Show Player Info Overlay", &g_ShowPlayerInfo);

        Vector3 pos = GetPlayerPosition();
        ImGui::Spacing();
        ImGui::Text("Current Position:");
        ImGui::BulletText("X: %.2f", pos.x);
        ImGui::BulletText("Y: %.2f", pos.y);
        ImGui::BulletText("Z: %.2f", pos.z);
    }

    if (ImGui::CollapsingHeader("ESP Options")) {
        ImGui::Checkbox("Show Radar", &g_ShowRadar);
        ImGui::Checkbox("Supply Bundle ESP (Scan-Based)", &g_ShowSupplyESP);

        ImGui::Spacing();
        ImGui::Text("FOV Multiplier:");
        ImGui::SliderFloat("##FOV", &g_FOVMultiplier, 0.5f, 2.0f, "%.2f");

        ImGui::Spacing();
        ImGui::Text("Detection Range (meters):");
        ImGui::SliderFloat("##Range", &g_DetectionRange, 100.0f, 10000.0f, "%.0fm");

        ImGui::Spacing();
        ImGui::Checkbox("Show W2S Debug", &g_ShowW2SDebug);

        ImGui::Spacing();
        ImGui::TextColored(ImVec4(1, 1, 0, 1), "Scans memory every 1 second");
        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1), "Shows items within 300m");

        ImGui::Spacing();
        ImGui::BeginDisabled();
        ImGui::Checkbox("Enemy ESP (Coming Soon)", &g_DummyEnemyESP);
        ImGui::Checkbox("Loot ESP (Coming Soon)", &g_DummyLootESP);
        ImGui::Checkbox("Shrine ESP (Coming Soon)", &g_DummyShrineESP);
        ImGui::EndDisabled();
    }

    ImGui::End();
}

void RenderESP() {
    ImGuiIO& io = ImGui::GetIO();

    if (g_ShowPlayerInfo) {
        ImGui::SetNextWindowPos(ImVec2(10, 10));
        ImGui::SetNextWindowBgAlpha(0.8f);
        ImGui::Begin("##PlayerInfo", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoSavedSettings);

        Vector3 pos = GetPlayerPosition();
        ImGui::TextColored(ImVec4(0, 1, 0, 1), "=== PLAYER ===");
        ImGui::Text("X: %.2f", pos.x);
        ImGui::Text("Y: %.2f", pos.y);
        ImGui::Text("Z: %.2f", pos.z);

        // Show view matrix when debug is enabled
        if (g_ShowW2SDebug) {
            ImGui::Separator();
            ImGui::TextColored(ImVec4(1, 1, 0, 1), "=== VIEW MATRIX ===");

            // Read the matrix
            static uintptr_t viewMatrixAddr = g_GameBase + 0x3487540;
            float matrix[16] = { 0 };
            __try {
                for (int i = 0; i < 16; i++) {
                    matrix[i] = *(float*)(viewMatrixAddr + (i * 4));
                }

                ImGui::Text("[0-3]:   [%.2f, %.2f, %.2f, %.2f]", matrix[0], matrix[1], matrix[2], matrix[3]);
                ImGui::Text("[4-7]:   [%.2f, %.2f, %.2f, %.2f]", matrix[4], matrix[5], matrix[6], matrix[7]);
                ImGui::Text("[8-11]:  [%.2f, %.2f, %.2f, %.2f]", matrix[8], matrix[9], matrix[10], matrix[11]);
                ImGui::Text("[12-15]: [%.2f, %.2f, %.2f, %.2f]", matrix[12], matrix[13], matrix[14], matrix[15]);
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {
                ImGui::TextColored(ImVec4(1, 0, 0, 1), "Failed to read matrix!");
            }
        }

        ImGui::End();
    }

    if (g_ShowRadar) {
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 170, io.DisplaySize.y - 170));
        ImGui::SetNextWindowSize(ImVec2(160, 160));
        ImGui::SetNextWindowBgAlpha(0.8f);
        ImGui::Begin("##Radar", nullptr,
            ImGuiWindowFlags_NoTitleBar |
            ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoSavedSettings);

        ImDrawList* draw = ImGui::GetWindowDrawList();
        ImVec2 p = ImGui::GetCursorScreenPos();
        float radius = 60.0f;
        ImVec2 center(p.x + 80, p.y + 80);

        // Radar circle
        draw->AddCircle(center, radius, IM_COL32(0, 255, 0, 255), 32, 2.0f);

        // Crosshair
        draw->AddLine(ImVec2(center.x - 10, center.y), ImVec2(center.x + 10, center.y), IM_COL32(0, 255, 0, 255));
        draw->AddLine(ImVec2(center.x, center.y - 10), ImVec2(center.x, center.y + 10), IM_COL32(0, 255, 0, 255));

        // Player dot
        draw->AddCircleFilled(center, 4.0f, IM_COL32(255, 0, 0, 255));

        ImGui::SetCursorPos(ImVec2(10, 10));
        ImGui::TextColored(ImVec4(1, 1, 1, 1), "RADAR");

        ImGui::End();
    }

    // ===== SCAN-BASED SUPPLY ESP =====
    if (g_ShowSupplyESP) {
        Vector3 playerPos = GetPlayerPosition();
        ImDrawList* draw = ImGui::GetBackgroundDrawList();

        static std::vector<ItemData> scannedItems;
        static DWORD lastScanTime = 0;
        static bool debugPrinted = false;
        DWORD currentTime = GetTickCount();

        // Scan every 1 second
        if (currentTime - lastScanTime > 1000) {
            lastScanTime = currentTime;
            scannedItems.clear();

            // Debug output (first scan only)
            if (!debugPrinted) {
                printf("\n[SCAN DEBUG] Starting item scan...\n");
                printf("[SCAN DEBUG] Player position: [%.2f, %.2f, %.2f]\n",
                    playerPos.x, playerPos.y, playerPos.z);
            }

            // USER'S FINAL DISCOVERY - Multi-level pointer chain to loot array!
            __try {
                // Follow the complete pointer chain (REVERSED ORDER!):
                // Base → +718 → +38 → +B8 → +A0 → Items with 0x180 stride

                uintptr_t ptr1 = *(uintptr_t*)(g_GameBase + 0x033B4E18);
                if (!ptr1 || ptr1 < 0x10000) {
                    if (!debugPrinted) {
                        printf("\n[ITEM ARRAY] ERROR: First pointer is NULL!\n");
                        debugPrinted = true;
                    }
                }
                else {
                    uintptr_t ptr2 = *(uintptr_t*)(ptr1 + 0x718);
                    if (!ptr2 || ptr2 < 0x10000) {
                        if (!debugPrinted) {
                            printf("\n[ITEM ARRAY] ERROR: Second pointer is NULL!\n");
                            debugPrinted = true;
                        }
                    }
                    else {
                        uintptr_t ptr3 = *(uintptr_t*)(ptr2 + 0x38);
                        if (!ptr3 || ptr3 < 0x10000) {
                            if (!debugPrinted) {
                                printf("\n[ITEM ARRAY] ERROR: Third pointer is NULL!\n");
                                debugPrinted = true;
                            }
                        }
                        else {
                            uintptr_t ptr4 = *(uintptr_t*)(ptr3 + 0xB8);
                            if (!ptr4 || ptr4 < 0x10000) {
                                if (!debugPrinted) {
                                    printf("\n[ITEM ARRAY] ERROR: Fourth pointer is NULL!\n");
                                    debugPrinted = true;
                                }
                            }
                            else {
                                uintptr_t firstItemAddr = ptr4 + 0xA0;  // A0 gives X, Y, Z (not A8 which gives Z first!)

                                if (!debugPrinted) {
                                    printf("\n[ITEM ARRAY] Following 4-level pointer chain!\n");
                                    printf("[ITEM ARRAY] Base: 0x%llX\n", g_GameBase + 0x033B4E18);
                                    printf("[ITEM ARRAY] Ptr1: 0x%llX (base)\n", ptr1);
                                    printf("[ITEM ARRAY] Ptr2: 0x%llX (ptr1 + 718)\n", ptr2);
                                    printf("[ITEM ARRAY] Ptr3: 0x%llX (ptr2 + 38)\n", ptr3);
                                    printf("[ITEM ARRAY] Ptr4: 0x%llX (ptr3 + B8)\n", ptr4);
                                    printf("[ITEM ARRAY] Items: 0x%llX (ptr4 + A0)\n", firstItemAddr);
                                }

                                int validPointers = 0;
                                int itemsAdded = 0;

                                // Scan up to 500 items with 0x180 stride
                                for (int i = 0; i < 500; i++) {
                                    uintptr_t itemAddr = firstItemAddr + (i * 0x180);

                                    Vector3 pos = { 0, 0, 0 };

                                    __try {
                                        // Coordinates at: item+0 (X), item+4 (Y), item+8 (Z)
                                        pos.x = *(float*)(itemAddr);
                                        pos.y = *(float*)(itemAddr + 4);
                                        pos.z = *(float*)(itemAddr + 8);
                                    }
                                    __except (EXCEPTION_EXECUTE_HANDLER) {
                                        continue; // Can't read, skip
                                    }

                                    // Debug first 5 items
                                    if (!debugPrinted && i < 5) {
                                        printf("  [Item %d] at 0x%llX: [%.2f, %.2f, %.2f]\n",
                                            i, itemAddr, pos.x, pos.y, pos.z);
                                    }

                                    validPointers++;

                                    // Validate coordinates
                                    if (pos.x == 0 && pos.y == 0 && pos.z == 0) continue;
                                    if (isnan(pos.x) || isnan(pos.y) || isnan(pos.z)) continue;
                                    if (fabs(pos.x) > 500000 || fabs(pos.y) > 500000 || fabs(pos.z) > 500000) continue;

                                    float distance = Distance3D(playerPos, pos);
                                    if (distance < 5.0f) continue;  // Too close (probably player)
                                    if (distance > g_DetectionRange) continue;  // Use slider value

                                    // Check for duplicates
                                    bool isDuplicate = false;
                                    for (const ItemData& existing : scannedItems) {
                                        if (Distance3D(pos, existing.position) < 1.0f) {
                                            isDuplicate = true;
                                            break;
                                        }
                                    }

                                    if (!isDuplicate) {
                                        ItemData item;
                                        item.position = pos;
                                        item.address = itemAddr;
                                        scannedItems.push_back(item);
                                        itemsAdded++;

                                        if (!debugPrinted && itemsAdded <= 10) {
                                            printf("  [LOOT FOUND] #%d: [%.0f, %.0f, %.0f] dist=%.0fm\n",
                                                itemsAdded, pos.x, pos.y, pos.z, distance);
                                        }
                                    }
                                }

                                if (!debugPrinted) {
                                    printf("\n[ITEM ARRAY] Scan complete:\n");
                                    printf("  Items scanned: %d / 500\n", validPointers);
                                    printf("  Items in range (%.0fm): %d\n", g_DetectionRange, itemsAdded);
                                    printf("  Final item count: %d\n", (int)scannedItems.size());
                                    debugPrinted = true;
                                }
                            }
                        }
                    }
                }

                // SECOND POINTER CHAIN - Additional items
                // Base: +033B4E30 → +0 → +38 → +B8 → +A0 (reading CE bottom to top)
                __try {
                    uintptr_t ptr1_2 = *(uintptr_t*)(g_GameBase + 0x033B4E30);
                    if (ptr1_2 && ptr1_2 >= 0x10000) {
                        uintptr_t ptr2_2 = *(uintptr_t*)(ptr1_2 + 0x0);  // Read pointer at +0
                        if (ptr2_2 && ptr2_2 >= 0x10000) {
                            uintptr_t ptr3_2 = *(uintptr_t*)(ptr2_2 + 0x38);  // Read pointer at +38
                            if (ptr3_2 && ptr3_2 >= 0x10000) {
                                uintptr_t ptr4_2 = *(uintptr_t*)(ptr3_2 + 0xB8);  // Read pointer at +B8
                                if (ptr4_2 && ptr4_2 >= 0x10000) {
                                    uintptr_t firstItemAddr2 = ptr4_2 + 0xA0;  // Final offset +A0 (X coords)

                                    // Scan second array
                                    for (int i = 0; i < 500; i++) {
                                        uintptr_t itemAddr = firstItemAddr2 + (i * 0x180);

                                        Vector3 pos = { 0, 0, 0 };
                                        __try {
                                            pos.x = *(float*)(itemAddr);
                                            pos.y = *(float*)(itemAddr + 4);
                                            pos.z = *(float*)(itemAddr + 8);
                                        }
                                        __except (EXCEPTION_EXECUTE_HANDLER) {
                                            continue;
                                        }

                                        // Validate
                                        if (pos.x == 0 && pos.y == 0 && pos.z == 0) continue;
                                        if (isnan(pos.x) || isnan(pos.y) || isnan(pos.z)) continue;
                                        if (fabs(pos.x) > 500000 || fabs(pos.y) > 500000 || fabs(pos.z) > 500000) continue;

                                        float distance = Distance3D(playerPos, pos);
                                        if (distance < 5.0f) continue;
                                        if (distance > g_DetectionRange) continue;

                                        // Check duplicates
                                        bool isDuplicate = false;
                                        for (const ItemData& existing : scannedItems) {
                                            if (Distance3D(pos, existing.position) < 1.0f) {
                                                isDuplicate = true;
                                                break;
                                            }
                                        }

                                        if (!isDuplicate) {
                                            ItemData item;
                                            item.position = pos;
                                            item.address = itemAddr;
                                            scannedItems.push_back(item);
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
                __except (EXCEPTION_EXECUTE_HANDLER) {
                    // Silent fail for second chain
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {
                if (!debugPrinted) {
                    printf("[ITEM ARRAY] EXCEPTION during scan!\n");
                    debugPrinted = true;
                }
            }
        }

        // Display scanned items
        int itemCount = 0;
        for (const ItemData& item : scannedItems) {
            float distance = Distance3D(playerPos, item.position);

            // List items in text (max 12 on screen)
            if (itemCount < 12) {
                char itemText[256];
                sprintf_s(itemText, "Item %d: %.0fm [%.0f, %.0f, %.0f] @ 0x%llX",
                    itemCount + 1, distance, item.position.x, item.position.y, item.position.z, item.address);
                draw->AddText(ImVec2(10, 200 + itemCount * 15), IM_COL32(0, 255, 0, 255), itemText);
            }

            // Draw ESP markers for items within detection range
            if (distance <= g_DetectionRange) {
                Vector2 screenPos;
                bool onScreen = WorldToScreen(item.position, playerPos, screenPos, io.DisplaySize.x, io.DisplaySize.y);

                if (onScreen) {
                    // Yellow circle
                    draw->AddCircleFilled(ImVec2(screenPos.x, screenPos.y), 8.0f, IM_COL32(255, 255, 0, 200));
                    draw->AddCircle(ImVec2(screenPos.x, screenPos.y), 8.0f, IM_COL32(0, 0, 0, 255), 12, 2.0f);

                    // Distance text
                    char text[64];
                    sprintf_s(text, "%.0fm", distance);
                    draw->AddText(ImVec2(screenPos.x + 12, screenPos.y - 8), IM_COL32(255, 255, 255, 255), text);
                }
            }

            itemCount++;
        }

        // Show status at top
        char statusText[256];
        sprintf_s(statusText, "Scan ESP: %d items found | Scan range: %.0fm | Player: [%.0f, %.0f, %.0f]",
            (int)scannedItems.size(), g_DetectionRange, playerPos.x, playerPos.y, playerPos.z);
        draw->AddText(ImVec2(10, 180), IM_COL32(0, 255, 255, 255), statusText);

        // If no items found, show help message
        if (scannedItems.size() == 0) {
            draw->AddText(ImVec2(10, 360), IM_COL32(255, 255, 0, 255),
                "No items detected. Try moving closer to supply bundles!");
        }
    }
}

// ===== COMMAND QUEUE HOOK =====
typedef void(__stdcall* ExecuteCommandLists)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
ExecuteCommandLists oExecuteCommandLists = nullptr;

void __stdcall hkExecuteCommandLists(ID3D12CommandQueue* queue, UINT NumCommandLists, ID3D12CommandList* const* ppCommandLists) {
    if (!g_pd3dCommandQueue) {
        g_pd3dCommandQueue = queue;
    }
    oExecuteCommandLists(queue, NumCommandLists, ppCommandLists);
}

// ===== PRESENT HOOK =====
typedef HRESULT(__stdcall* D3D12Present)(IDXGISwapChain3*, UINT, UINT);
D3D12Present oPresent = nullptr;

void** GetVTable(void* obj) {
    return *reinterpret_cast<void***>(obj);
}

HRESULT __stdcall hkPresent(IDXGISwapChain3* pSwapChain, UINT SyncInterval, UINT Flags) {
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
            ImGui_ImplDX12_Init(g_pd3dDevice, g_buffersCounts,
                DXGI_FORMAT_R8G8B8A8_UNORM, g_pd3dSrvDescHeap,
                g_pd3dSrvDescHeap->GetCPUDescriptorHandleForHeapStart(),
                g_pd3dSrvDescHeap->GetGPUDescriptorHandleForHeapStart());

            oWndProc = (WNDPROC)SetWindowLongPtr(g_hWnd, GWLP_WNDPROC, (LONG_PTR)hkWndProc);

            g_Initialized = true;
            printf("[GoTCheat] ===== SCAN ESP INITIALIZED! =====\n");
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

HWND CreateInvisibleWindow() {
    WNDCLASSEXA wc = {};
    wc.cbSize = sizeof(WNDCLASSEXA);
    wc.lpfnWndProc = DefWindowProc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = "DummyClass";
    RegisterClassExA(&wc);
    return CreateWindowExA(0, "DummyClass", "Dummy", WS_OVERLAPPEDWINDOW,
        0, 0, 100, 100, NULL, NULL, wc.hInstance, NULL);
}

DWORD WINAPI MainThread(LPVOID param) {
    AllocConsole();
    FILE* f;
    freopen_s(&f, "CONOUT$", "w", stdout);

    printf("========================================\n");
    printf("  Ghost of Tsushima - SCAN-BASED ESP\n");
    printf("========================================\n");

    if (MH_Initialize() != MH_OK) {
        printf("[GoTCheat] MinHook init failed!\n");
        return 1;
    }

    Sleep(5000);

    printf("[GoTCheat] Creating dummy device...\n");

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

    void* pPresentAddr = pSwapChainVTable[8];
    void* pExecuteCommandListsAddr = pCommandQueueVTable[10];

    printf("[GoTCheat] Present: 0x%p\n", pPresentAddr);
    printf("[GoTCheat] ExecuteCommandLists: 0x%p\n", pExecuteCommandListsAddr);

    MH_CreateHook(pPresentAddr, &hkPresent, reinterpret_cast<LPVOID*>(&oPresent));
    MH_EnableHook(pPresentAddr);

    MH_CreateHook(pExecuteCommandListsAddr, &hkExecuteCommandLists, reinterpret_cast<LPVOID*>(&oExecuteCommandLists));
    MH_EnableHook(pExecuteCommandListsAddr);

    printf("[GoTCheat] ===== HOOKS ENABLED! =====\n");
    printf("[GoTCheat] Using scan-based item detection\n");
    printf("[GoTCheat] Array base: GhostOfTsushima.exe+1EBD440\n");
    printf("[GoTCheat] Press INSERT in-game to toggle menu\n");

    pSwapChain->Release();
    pCommandQueue->Release();
    pDevice->Release();
    pAdapter->Release();
    pFactory->Release();
    DestroyWindow(dummyWindow);

    while (true) Sleep(1000);
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        CreateThread(nullptr, 0, MainThread, hModule, 0, nullptr);
    }
    else if (reason == DLL_PROCESS_DETACH) {
        MH_DisableHook(MH_ALL_HOOKS);
        MH_Uninitialize();
        if (oWndProc) SetWindowLongPtr(g_hWnd, GWLP_WNDPROC, (LONG_PTR)oWndProc);
        if (g_Initialized) {
            ImGui_ImplDX12_Shutdown();
            ImGui_ImplWin32_Shutdown();
            ImGui::DestroyContext();
        }
    }
    return TRUE;
}