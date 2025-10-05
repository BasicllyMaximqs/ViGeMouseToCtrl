#define WIN32_LEAN_AND_MEAN
#define DIRECTINPUT_VERSION 0x0800
#define NOMINMAX // took me a hour to find out i need to define this for the min/max macro
#include <windows.h>
#include <dinput.h>
#include <d3d9.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iostream>
#include <thread>
#include <atomic>
#include <mutex>
#include <cstdint>
#include <algorithm>
#include <cmath>
#include <chrono>

// ViGEm
#include "./ViGEm/Client.h"

// ImGui
#include "./imgui/imgui.h"
#include "./imgui/imgui_impl_win32.h"
#include "./imgui/imgui_impl_dx9.h"


#pragma comment(lib, "d3d9.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "ViGEmClient.lib")
#pragma comment(lib, "setupapi.lib")


#define UDP_PORT 12345 //change as u want
const SHORT MAX_AXIS_VALUE = 32767;
const SHORT MIN_AXIS_VALUE = -32768;

static float g_mouse_gain = 30.0f;        //  (Higher = faster turn)
static float g_stick_friction = 0.85f;    // Lower = faster snap back/less smoothing


LPDIRECT3D9 g_pD3D = nullptr;
LPDIRECT3DDEVICE9 g_pd3dDevice = nullptr;
D3DPRESENT_PARAMETERS g_d3dpp = {};

struct MousePayload {
    LONG deltaX;
    LONG deltaY;
    bool mLMB;
    bool mRMB;
};

static std::mutex g_state_mutex;

static LONG g_mouse_delta_x = 0;
static LONG g_mouse_delta_y = 0;

static BYTE g_right_trigger = 0;
static BYTE g_left_trigger = 0;

static bool g_key_w_pressed = false;
static bool g_key_a_pressed = false;
static bool g_key_s_pressed = false;
static bool g_key_d_pressed = false;

static XUSB_REPORT g_report{};
static std::atomic<bool> g_running(true);
static std::atomic<uint64_t> g_udp_packet_count(0);

static uint64_t g_last_packet_count = 0;
static std::chrono::steady_clock::time_point g_last_time;
static float g_packets_per_second = 0.0f;

PVIGEM_CLIENT g_client = nullptr;
PVIGEM_TARGET g_target = nullptr;

static HHOOK g_hKeyboardHook = nullptr;

static float g_stickRX_accum = 0.0f;
static float g_stickRY_accum = 0.0f;

// for some reason, easyanticheat hooks the mouse with DirectInput or whatever
// but not the keyboard for some reason, please @eac fix this lmao
static LRESULT CALLBACK KeyboardProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode >= 0 && (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN ||
        wParam == WM_KEYUP || wParam == WM_SYSKEYUP))
    {
        auto* pKBDLLHookStruct = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);
        DWORD vk = pKBDLLHookStruct->vkCode;
        bool pressed = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);

        std::lock_guard<std::mutex> lock(g_state_mutex);

        switch (vk)
        {
        case 'W': g_key_w_pressed = pressed; return 1;
        case 'S': g_key_s_pressed = pressed; return 1;
        case 'A': g_key_a_pressed = pressed; return 1;
        case 'D': g_key_d_pressed = pressed; return 1;
        }
    }

    return CallNextHookEx(g_hKeyboardHook, nCode, wParam, lParam);
}

bool InstallKeyboardHook()
{
    g_hKeyboardHook = SetWindowsHookEx(WH_KEYBOARD_LL, KeyboardProc, GetModuleHandle(NULL), 0);
    if (!g_hKeyboardHook)
    {
        std::cerr << "ERROR: Failed to init low-level keyboard hook Error: "
            << GetLastError() << "\n";
        return false;
    }
    std::cout << "Low-level keyboard hook init done.\n";
    return true;
}

void UninstallKeyboardHook()
{
    if (g_hKeyboardHook)
    {
        UnhookWindowsHookEx(g_hKeyboardHook);
        g_hKeyboardHook = nullptr;
        std::cout << "Low-level keyboard hook uninstalled.\n";
    }
}

// udp networking bs
/*

    Big disclaimer: don't use C# to try to replicate the client, view udp_mouse_sender.cpp for linux (g++) for more info

    the damn structs wont match and it'll all go down hell

*/
void UdpServerThread()
{
    std::cout << "Starting UDP Server on port " << UDP_PORT << "...\n";

    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::cerr << "ERROR: WSAStartup failed: " << WSAGetLastError() << "\n";
        return;
    }

    SOCKET serverSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (serverSocket == INVALID_SOCKET) {
        std::cerr << "ERROR: Socket creation failed: " << WSAGetLastError() << "\n";
        WSACleanup();
        return;
    }

    sockaddr_in serverAddr{};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(UDP_PORT);
    serverAddr.sin_addr.s_addr = INADDR_ANY;

    DWORD timeout = 100;
    setsockopt(serverSocket, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));

    if (bind(serverSocket, (SOCKADDR*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
        std::cerr << "ERROR: Socket bind failed on port " << UDP_PORT
            << ": " << WSAGetLastError() << "\n";
        closesocket(serverSocket);
        WSACleanup();
        return;
    }

    std::cout << "UDP Server is running.\n";

    sockaddr_in clientAddr{};
    int clientAddrSize = sizeof(clientAddr);
    MousePayload payload;

    while (g_running)
    {
        int bytes = recvfrom(serverSocket, (char*)&payload, sizeof(payload), 0,
            (sockaddr*)&clientAddr, &clientAddrSize);

        if (bytes == sizeof(MousePayload))
        {
            std::lock_guard<std::mutex> lock(g_state_mutex);
            // catch the UDP packets here and send them off
            g_mouse_delta_x += payload.deltaX;
            g_mouse_delta_y += payload.deltaY;
            g_right_trigger = payload.mLMB ? 255 : 0;
            g_left_trigger = payload.mRMB ? 255 : 0;
            g_udp_packet_count++;
        }
    }

    std::cout << "UDP Server shutting down.\n";
    closesocket(serverSocket);
    WSACleanup();
}

// vigem sequence
bool InitViGEm()
{
    std::cout << "Initializing ViGEm Client...\n";
    g_client = vigem_alloc();
    if (!g_client) return false;
    if (!VIGEM_SUCCESS(vigem_connect(g_client))) return false;
    g_target = vigem_target_x360_alloc();
    if (!g_target) return false;
    if (!VIGEM_SUCCESS(vigem_target_add(g_client, g_target))) return false;
    XUSB_REPORT_INIT(&g_report);
    std::cout << "Virtual Xbox 360 Controller plugged in.\n";
    return true;
}

void CleanupViGEm()
{
    if (g_target) {
        vigem_target_remove(g_client, g_target);
        vigem_target_free(g_target);
        g_target = nullptr;
    }
    if (g_client) {
        vigem_disconnect(g_client);
        vigem_free(g_client);
        g_client = nullptr;
    }
}


bool CreateDeviceD3D(HWND hWnd)
{
    if (!(g_pD3D = Direct3DCreate9(D3D_SDK_VERSION))) return false;
    ZeroMemory(&g_d3dpp, sizeof(g_d3dpp));
    g_d3dpp.Windowed = TRUE;
    g_d3dpp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    g_d3dpp.BackBufferFormat = D3DFMT_UNKNOWN;
    g_d3dpp.EnableAutoDepthStencil = TRUE;
    g_d3dpp.AutoDepthStencilFormat = D3DFMT_D16;
    g_d3dpp.PresentationInterval = D3DPRESENT_INTERVAL_DEFAULT;
    return g_pD3D->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hWnd,
        D3DCREATE_HARDWARE_VERTEXPROCESSING, &g_d3dpp, &g_pd3dDevice) >= 0;
}

void CleanupDeviceD3D()
{
    if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = nullptr; }
    if (g_pD3D) { g_pD3D->Release(); g_pD3D = nullptr; }
}

void ResetDevice()
{
    ImGui_ImplDX9_InvalidateDeviceObjects();
    if (g_pd3dDevice->Reset(&g_d3dpp) == D3DERR_INVALIDCALL) IM_ASSERT(0);
    ImGui_ImplDX9_CreateDeviceObjects();
}
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);


LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg)
    {
    case WM_SIZE:
        if (g_pd3dDevice != nullptr && wParam != SIZE_MINIMIZED)
        {
            g_d3dpp.BackBufferWidth = (UINT)LOWORD(lParam);
            g_d3dpp.BackBufferHeight = (UINT)HIWORD(lParam);
            ResetDevice();
        }
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU) return 0;
        break;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}

// dont use WinAPI main, else compiler screamin..
int main(HINSTANCE hInstance, HINSTANCE, LPSTR, int)
{
    if (!InitViGEm()) return -1;

    const wchar_t CLASS_NAME[] = L"randomclassnameayylmao";
    WNDCLASS wc{}; wc.lpfnWndProc = WndProc; wc.hInstance = hInstance; wc.lpszClassName = CLASS_NAME;
    if (!RegisterClass(&wc)) return 1;

    HWND hWnd = CreateWindowEx(0, CLASS_NAME, L"inputmapper",
        WS_OVERLAPPEDWINDOW, 100, 100, 1000, 600,
        nullptr, nullptr, hInstance, nullptr);
    if (!hWnd) return 1;

    if (!CreateDeviceD3D(hWnd)) { CleanupDeviceD3D(); return 1; }

    ShowWindow(hWnd, SW_SHOWDEFAULT);
    UpdateWindow(hWnd);

    // ImGui setup
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui_ImplWin32_Init(hWnd);
    ImGui_ImplDX9_Init(g_pd3dDevice);

    if (!InstallKeyboardHook()) { CleanupViGEm(); return -1; }

    std::thread udpThread(UdpServerThread);

    // Initialize time tracker for pps
    g_last_time = std::chrono::steady_clock::now();

    MSG msg{};
    while (msg.message != WM_QUIT)
    {
        if (PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            continue;
        }

        // pps counter
        {
            auto current_time = std::chrono::steady_clock::now();
            auto elapsed_time = std::chrono::duration_cast<std::chrono::milliseconds>(current_time - g_last_time);
            uint64_t current_packet_count = g_udp_packet_count.load();
            uint64_t delta_packets = current_packet_count - g_last_packet_count;


            if (elapsed_time.count() >= 100)
            {
                float time_in_seconds = elapsed_time.count() / 1000.0f;
                g_packets_per_second = (float)delta_packets / time_in_seconds;

                // Reset counters
                g_last_packet_count = current_packet_count;
                g_last_time = current_time;
            }
        }


        // Gamepad report update (WASD + mouse)
        {
            std::lock_guard<std::mutex> lock(g_state_mutex);


            g_report.sThumbLX = 0;
            g_report.sThumbLY = 0;

            if (g_key_w_pressed && !g_key_s_pressed) g_report.sThumbLY = MAX_AXIS_VALUE;
            else if (g_key_s_pressed && !g_key_w_pressed) g_report.sThumbLY = MIN_AXIS_VALUE;

            if (g_key_d_pressed && !g_key_a_pressed) g_report.sThumbLX = MAX_AXIS_VALUE;
            else if (g_key_a_pressed && !g_key_d_pressed) g_report.sThumbLX = MIN_AXIS_VALUE;


            float impulseRX = static_cast<float>(g_mouse_delta_y) * g_mouse_gain;
            float impulseRY = -static_cast<float>(g_mouse_delta_x) * g_mouse_gain;

            g_stickRX_accum *= g_stick_friction;
            g_stickRY_accum *= g_stick_friction;
            g_stickRX_accum += impulseRX;
            g_stickRY_accum += impulseRY;
            g_stickRX_accum = std::clamp(g_stickRX_accum, static_cast<float>(MIN_AXIS_VALUE), static_cast<float>(MAX_AXIS_VALUE));
            g_stickRY_accum = std::clamp(g_stickRY_accum, static_cast<float>(MIN_AXIS_VALUE), static_cast<float>(MAX_AXIS_VALUE));

            g_report.sThumbRX = static_cast<SHORT>(g_stickRX_accum);
            g_report.sThumbRY = static_cast<SHORT>(g_stickRY_accum);


            g_report.bRightTrigger = g_right_trigger;
            g_report.bLeftTrigger = g_left_trigger;


            g_mouse_delta_x = g_mouse_delta_y = 0;
        }

        vigem_target_x360_update(g_client, g_target, g_report);

        ImGui_ImplDX9_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        ImGui::SetNextWindowPos(ImVec2(10, 10));
        ImGui::SetNextWindowSize(ImVec2(450, 360));
        ImGui::Begin("Controller Status & Settings", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove);
        ImGui::Text("ViGEm Input Mapper");
        ImGui::Separator();

        ImGui::SliderFloat("Mouse Gain (Sensitivity)", &g_mouse_gain, 0.1f, 100.0f, "%.1f");
        ImGui::TextDisabled("Higher value = faster turning for a given mouse movement.");

        ImGui::SliderFloat("Stick Friction (Damping)", &g_stick_friction, 0.01f, 0.99f, "%.2f");
        ImGui::TextDisabled("Lower value (e.g., 0.60) = faster snap-back to center/less smoothing (more responsive).");
        ImGui::TextDisabled("Higher value (e.g., 0.95) = slower snap-back/more smoothing (reduces jitter).");
        ImGui::Separator();

        ImGui::Text("UDP Traffic:");
        ImGui::Text("Total packets: %llu", g_udp_packet_count.load());
        ImGui::Text("Packets/Sec (PPS): %.2f", g_packets_per_second);
        ImGui::Separator();

        ImGui::Text("Input Deltas: X:%d / Y:%d", g_mouse_delta_x, g_mouse_delta_y);
        ImGui::Text("Current Gain: %.2f / Friction: %.2f", g_mouse_gain, g_stick_friction);
        ImGui::Text("LStick (WASD): %d / %d", g_report.sThumbLX, g_report.sThumbLY);
        ImGui::Text("RStick (Mouse): %d / %d", g_report.sThumbRX, g_report.sThumbRY);
        ImGui::Text("Triggers: LT:%d / RT:%d", g_report.bLeftTrigger, g_report.bRightTrigger);
        ImGui::End();

        ImGui::EndFrame();

        g_pd3dDevice->SetRenderState(D3DRS_ZENABLE, FALSE);
        g_pd3dDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        g_pd3dDevice->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
        g_pd3dDevice->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,
            D3DCOLOR_RGBA(0, 0, 0, 255), 1.0f, 0);
        if (g_pd3dDevice->BeginScene() >= 0)
        {
            ImGui::Render();
            ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
            g_pd3dDevice->EndScene();
        }

        if (g_pd3dDevice->Present(nullptr, nullptr, nullptr, nullptr) == D3DERR_DEVICELOST &&
            g_pd3dDevice->TestCooperativeLevel() == D3DERR_DEVICENOTRESET)
            ResetDevice();
    }

    g_running = false;
    if (udpThread.joinable()) udpThread.join();

    UninstallKeyboardHook();
    ImGui_ImplDX9_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupDeviceD3D();
    CleanupViGEm();
    UnregisterClass(CLASS_NAME, hInstance);

    return 0;
}
