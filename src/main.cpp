#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <winsock2.h>
#include <windows.h>
#include <shellapi.h>
#include <gdiplus.h>
#include <dwmapi.h>
#include <shlwapi.h>

#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>

#include "installer.h"
#include "launcher.h"
#include "ely_auth.h"
#include "emoji_icons.h"
#include <discord_rpc.h>

#include <cmath>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <algorithm>
#include <filesystem>
#include <string>
#include <fstream>
#include <vector>

constexpr int WIDTH = 980;
constexpr int HEIGHT = 640;

unsigned int gIconProfileTex = 0;
unsigned int gIconBuildsTex = 0;
unsigned int gIconSettingsTex = 0;
unsigned int gIconInfoTex = 0;
unsigned int gBlurBackgroundTex = 0;

// Текст последней ошибки входа, отображаемый в тосте
std::string g_LastLoginErrorText = "Введите игровой ник";

const char* const kDiscordAppId = "1546780287189651566";
bool gDiscordReady = false;
bool gDiscordInGame = false;
bool gDiscordRpcEnabled = true;

void UpdateDiscordPresence(const char* nick) {
    if (!gDiscordReady || !gDiscordRpcEnabled) return;

    DiscordRichPresence presence{};
    memset(&presence, 0, sizeof(presence));

    if (gDiscordInGame) {
        presence.state = "Играет на Amethyst";
        presence.details = nick && nick[0] ? nick : "AmethystLauncher";
    } else {
        presence.state = "В лаунчере";
        presence.details = nick && nick[0] ? nick : "AmethystLauncher";
        presence.startTimestamp = static_cast<int64_t>(time(nullptr));
        presence.largeImageKey = "amethyst_icon";
        presence.largeImageText = "Amethyst Launcher";
    }

    Discord_UpdatePresence(&presence);
}

void InitDiscordRPC(const char* nick) {
    if (gDiscordReady) return;

    DiscordEventHandlers handlers{};
    memset(&handlers, 0, sizeof(handlers));

    Discord_Initialize(kDiscordAppId, &handlers, 1, nullptr);
    gDiscordReady = true;
    UpdateDiscordPresence(nick);
}

struct InfoImage {
    unsigned int tex = 0;
    int width = 0;
    int height = 0;
};

InfoImage gInfoImages[4];

std::wstring FindExecutableDirectory() {
    wchar_t executablePath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, executablePath, MAX_PATH);

    std::wstring directory(executablePath);
    const size_t slash = directory.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : directory.substr(0, slash);
}

std::wstring FindBackgroundDirectory() {
    std::wstring directory = FindExecutableDirectory();

    const std::wstring dirs[] = {
        directory + L"\\..\\assets",
        directory + L"\\..\\..\\assets",
        directory + L""
    };

    for (const std::wstring& dir : dirs) {
        if (GetFileAttributesW((dir + L"\\minecraft.jpg").c_str()) != INVALID_FILE_ATTRIBUTES) {
            return dir;
        }
    }

    return dirs[0];
}

std::wstring FindBackgroundPath() {
    return FindBackgroundDirectory() + L"\\minecraft.jpg";
}

std::wstring FindInfoImagePath(const wchar_t* name) {
    return FindBackgroundDirectory() + L"\\" + name;
}

std::string GetInstallStatusText(InstallState& state) {
    std::lock_guard lock(state.mtx);
    if (state.phase == InstallPhase::Done) return "installed";
    if (state.phase == InstallPhase::Error) return "error: " + state.error;
    if (state.phase == InstallPhase::Idle) return "";
    return "installing";
}

bool IsGameInstalledOnDisk(const std::filesystem::path& gameDir) {
    std::filesystem::path statusPath = gameDir / ".fabric_install_status";
    std::error_code ec;
    if (std::filesystem::exists(statusPath, ec)) {
        std::ifstream statusFile(statusPath);
        std::string content(
            (std::istreambuf_iterator<char>(statusFile)),
            std::istreambuf_iterator<char>());
        if (!content.empty()) return true;
    }
    ec.clear();
    std::filesystem::path versionsDir = gameDir / "versions";
    if (std::filesystem::exists(versionsDir, ec)) {
        for (const auto& entry : std::filesystem::directory_iterator(versionsDir, ec)) {
            if (ec) break;
            if (entry.is_directory() &&
                entry.path().filename().wstring().find(L"fabric-loader-") == 0) {
                return true;
            }
        }
    }
    return false;
}

bool HasVisibleText(const char* text) {
    if (!text) return false;

    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(text); *p; ++p) {
        if (!std::isspace(*p)) return true;
    }

    return false;
}

int CountProjectMods() {
    std::wstring modsPath = FindExecutableDirectory() + L"\\..\\mods";
    if (GetFileAttributesW(modsPath.c_str()) == INVALID_FILE_ATTRIBUTES) return 0;

    int count = 0;
    try {
        for (const auto& entry : std::filesystem::directory_iterator(modsPath)) {
            if (!entry.is_regular_file()) continue;
            std::wstring ext = entry.path().extension().wstring();
            if (ext == L".jar" || ext == L".JAR") ++count;
        }
    } catch (...) {}
    return count;
}

void ApplyAppStyle() {
    ImGuiStyle& style = ImGui::GetStyle();
    ImVec4* colors = style.Colors;

    colors[ImGuiCol_Text] = ImVec4(0.94f, 0.95f, 0.97f, 1.0f);
    colors[ImGuiCol_TextDisabled] = ImVec4(0.57f, 0.61f, 0.68f, 1.0f);
    colors[ImGuiCol_WindowBg] = ImVec4(0, 0, 0, 0);
    colors[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);

    colors[ImGuiCol_Button] = ImVec4(0.14f, 0.15f, 0.18f, 1.0f);
    colors[ImGuiCol_ButtonHovered] = ImVec4(0.22f, 0.23f, 0.27f, 1.0f);
    colors[ImGuiCol_ButtonActive] = ImVec4(0.32f, 0.28f, 0.11f, 1.0f);

    colors[ImGuiCol_FrameBg] = ImVec4(0.08f, 0.09f, 0.12f, 0.94f);
    colors[ImGuiCol_FrameBgHovered] = ImVec4(0.12f, 0.13f, 0.17f, 1.0f);
    colors[ImGuiCol_FrameBgActive] = ImVec4(0.16f, 0.17f, 0.22f, 1.0f);

    style.WindowPadding = ImVec2(0, 0);
    style.FramePadding = ImVec2(11, 8);
    style.ItemSpacing = ImVec2(8, 8);
    style.FrameRounding = 8.0f;
    style.WindowRounding = 0.0f;
    style.ChildRounding = 8.0f;
}

void ApplyBlur(HWND hwnd) {
    struct ACCENTPOLICY {
        int AccentState;
        int AccentFlags;
        int GradientColor;
        int AnimationId;
    };

    struct WINCOMPATTRDATA {
        int Attribute;
        PVOID Data;
        ULONG SizeOfData;
    };

    HMODULE user32 = GetModuleHandleA("user32.dll");
    if (!user32) return;

    using Fn = BOOL(WINAPI*)(HWND, WINCOMPATTRDATA*);
    const Fn setWindowCompositionAttribute =
        reinterpret_cast<Fn>(GetProcAddress(user32, "SetWindowCompositionAttribute"));

    if (!setWindowCompositionAttribute) return;

    ACCENTPOLICY policy = {3, 0, 0, 0};
    WINCOMPATTRDATA data = {19, &policy, sizeof(policy)};
    setWindowCompositionAttribute(hwnd, &data);
}

unsigned int UploadBitmapToGL(Gdiplus::Bitmap& bitmap, int& outWidth, int& outHeight) {
    const UINT width = bitmap.GetWidth();
    const UINT height = bitmap.GetHeight();
    outWidth = 0;
    outHeight = 0;

    if (!width || !height) {
        return 0;
    }

    Gdiplus::Rect rect(0, 0, static_cast<INT>(width), static_cast<INT>(height));
    Gdiplus::BitmapData data{};

    if (bitmap.LockBits(
            &rect,
            Gdiplus::ImageLockModeRead,
            PixelFormat32bppARGB,
            &data
        ) != Gdiplus::Ok) {
        return 0;
    }

    std::vector<unsigned char> pixels(static_cast<size_t>(width) * height * 4);

    for (UINT y = 0; y < height; ++y) {
        const unsigned char* source =
            static_cast<const unsigned char*>(data.Scan0) + y * data.Stride;

        unsigned char* destination =
            pixels.data() + static_cast<size_t>(y) * width * 4;

        for (UINT x = 0; x < width; ++x) {
            destination[x * 4 + 0] = source[x * 4 + 2];
            destination[x * 4 + 1] = source[x * 4 + 1];
            destination[x * 4 + 2] = source[x * 4 + 0];
            destination[x * 4 + 3] = source[x * 4 + 3];
        }
    }

    bitmap.UnlockBits(&data);

    unsigned int texture = 0;

    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    glTexImage2D(
        GL_TEXTURE_2D,
        0,
        GL_RGBA,
        static_cast<GLsizei>(width),
        static_cast<GLsizei>(height),
        0,
        GL_RGBA,
        GL_UNSIGNED_BYTE,
        pixels.data()
    );

    glBindTexture(GL_TEXTURE_2D, 0);

    outWidth = static_cast<int>(width);
    outHeight = static_cast<int>(height);
    return texture;
}

unsigned int LoadLocalTextureSized(const wchar_t* file, int& outWidth, int& outHeight) {
    outWidth = 0;
    outHeight = 0;

    Gdiplus::Bitmap bitmap(file);

    if (bitmap.GetLastStatus() != Gdiplus::Ok) {
        return 0;
    }

    return UploadBitmapToGL(bitmap, outWidth, outHeight);
}

unsigned int LoadBlurredBackgroundTexture(const wchar_t* file, int& outWidth, int& outHeight) {
    Gdiplus::Bitmap source(file);
    outWidth = 0;
    outHeight = 0;

    if (source.GetLastStatus() != Gdiplus::Ok) {
        return 0;
    }

    const int targetW = WIDTH;
    const int targetH = HEIGHT;

    const int smallW = std::max(1, targetW / 32);
    const int smallH = std::max(1, targetH / 32);
    const int midW = std::max(1, targetW / 6);
    const int midH = std::max(1, targetH / 6);

    Gdiplus::Bitmap small(smallW, smallH, PixelFormat32bppARGB);
    {
        Gdiplus::Graphics g(&small);
        g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        g.DrawImage(&source, Gdiplus::Rect(0, 0, smallW, smallH));
    }

    Gdiplus::Bitmap mid(midW, midH, PixelFormat32bppARGB);
    {
        Gdiplus::Graphics g(&mid);
        g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        g.DrawImage(&small, Gdiplus::Rect(0, 0, midW, midH));
    }

    Gdiplus::Bitmap result(targetW, targetH, PixelFormat32bppARGB);
    {
        Gdiplus::Graphics g(&result);
        g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        g.DrawImage(&mid, Gdiplus::Rect(0, 0, targetW, targetH));
    }

    return UploadBitmapToGL(result, outWidth, outHeight);
}

unsigned int LoadLocalTexture(const wchar_t* file) {
    int width = 0;
    int height = 0;
    return LoadLocalTextureSized(file, width, height);
}

unsigned int LoadTextureFromMemory(const unsigned char* data, size_t len) {
    IStream* stream = SHCreateMemStream(
        static_cast<const BYTE*>(data),
        static_cast<UINT>(len)
    );
    if (!stream) {
        return 0;
    }

    Gdiplus::Bitmap bitmap(stream);
    stream->Release();

    if (bitmap.GetLastStatus() != Gdiplus::Ok) {
        return 0;
    }

    const UINT width = bitmap.GetWidth();
    const UINT height = bitmap.GetHeight();

    if (!width || !height) {
        return 0;
    }

    Gdiplus::Rect rect(0, 0, static_cast<INT>(width), static_cast<INT>(height));
    Gdiplus::BitmapData dataBuf{};

    if (bitmap.LockBits(
            &rect,
            Gdiplus::ImageLockModeRead,
            PixelFormat32bppARGB,
            &dataBuf
        ) != Gdiplus::Ok) {
        return 0;
    }

    std::vector<unsigned char> pixels(static_cast<size_t>(width) * height * 4);

    for (UINT y = 0; y < height; ++y) {
        const unsigned char* source =
            static_cast<const unsigned char*>(dataBuf.Scan0) + y * dataBuf.Stride;

        unsigned char* destination =
            pixels.data() + static_cast<size_t>(y) * width * 4;

        for (UINT x = 0; x < width; ++x) {
            destination[x * 4 + 0] = source[x * 4 + 2];
            destination[x * 4 + 1] = source[x * 4 + 1];
            destination[x * 4 + 2] = source[x * 4 + 0];
            destination[x * 4 + 3] = source[x * 4 + 3];
        }
    }

    bitmap.UnlockBits(&dataBuf);

    unsigned int texture = 0;

    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    glTexImage2D(
        GL_TEXTURE_2D,
        0,
        GL_RGBA,
        static_cast<GLsizei>(width),
        static_cast<GLsizei>(height),
        0,
        GL_RGBA,
        GL_UNSIGNED_BYTE,
        pixels.data()
    );

    glBindTexture(GL_TEXTURE_2D, 0);
    return texture;
}

void BeginRootWindow(const char* id) {
    ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(WIDTH, HEIGHT), ImGuiCond_Always);

    ImGui::Begin(
        id,
        nullptr,
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoDecoration |
        ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoScrollWithMouse |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoBackground
    );
}

void DrawWindowControls(HWND hwnd) {
    ImGui::PushID("window-controls");

    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1, 1, 1, 0.12f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1, 1, 1, 0.20f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.88f, 0.89f, 0.92f, 1.0f));

    ImGui::SetCursorPos(ImVec2(WIDTH - 76.0f, 10.0f));
    if (ImGui::Button("-", ImVec2(28, 26)) && hwnd) {
        ShowWindow(hwnd, SW_MINIMIZE);
    }

    ImGui::SetCursorPos(ImVec2(WIDTH - 40.0f, 10.0f));
    if (ImGui::Button("x", ImVec2(28, 26)) && hwnd) {
        PostMessage(hwnd, WM_CLOSE, 0, 0);
    }

    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar();
    ImGui::PopID();
}

static WNDPROC g_originalWndProc = nullptr;

static LRESULT CALLBACK LauncherWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_NCHITTEST) {
        const POINT pt = {
            static_cast<LONG>(static_cast<short>(LOWORD(lParam))),
            static_cast<LONG>(static_cast<short>(HIWORD(lParam)))
        };

        RECT rc;
        GetWindowRect(hwnd, &rc);

        const int localX = static_cast<int>(pt.x - rc.left);
        const int localY = static_cast<int>(pt.y - rc.top);

        if (localY <= 84 && !(localX >= WIDTH - 80 && localY <= 42)) {
            return HTCAPTION;
        }
        return HTCLIENT;
    }
    return CallWindowProc(g_originalWndProc, hwnd, msg, wParam, lParam);
}

void DrawSplashLogo(ImDrawList* draw, ImVec2 center, float scale) {
    const ImU32 white = IM_COL32(242, 242, 240, 255);
    const ImU32 lavender = IM_COL32(188, 132, 255, 255);

    const float r = 34.0f * scale;
    const float arm = 15.0f * scale;
    const float line = 4.0f * scale;

    draw->AddLine({center.x - r, center.y - r}, {center.x - r + arm, center.y - r}, white, line);
    draw->AddLine({center.x - r, center.y - r}, {center.x - r, center.y - r + arm}, white, line);
    draw->AddLine({center.x + r, center.y - r}, {center.x + r - arm, center.y - r}, white, line);
    draw->AddLine({center.x + r, center.y - r}, {center.x + r, center.y - r + arm}, white, line);
    draw->AddLine({center.x - r, center.y + r}, {center.x - r + arm, center.y + r}, white, line);
    draw->AddLine({center.x - r, center.y + r}, {center.x - r, center.y + r - arm}, white, line);
    draw->AddLine({center.x + r, center.y + r}, {center.x + r - arm, center.y + r}, white, line);
    draw->AddLine({center.x + r, center.y + r}, {center.x + r, center.y + r - arm}, white, line);

    draw->AddCircleFilled(center, 13.0f * scale, lavender);
    draw->AddCircle(center, 23.0f * scale, lavender, 32, 3.0f * scale);
}

void DrawLoginToast(ImDrawList* draw, ImVec2 rootPos, bool loginError, double loginErrorAt, const char* errorText) {
    if (!loginError || loginErrorAt < 0.0) return;

    const float age = static_cast<float>(glfwGetTime() - loginErrorAt);
    if (age >= 2.8f) return;

    const float show = fminf(age / 0.20f, 1.0f);
    const float hide = fminf((2.8f - age) / 0.28f, 1.0f);
    const float alpha = fminf(show, hide);

    const float toastW = 620.0f;
    const float toastH = 86.0f;

    const ImVec2 min(
        rootPos.x + WIDTH - toastW - 22.0f + (1.0f - alpha) * 20.0f,
        rootPos.y + HEIGHT - toastH - 22.0f
    );

    const ImVec2 max(min.x + toastW, min.y + toastH);
    const int a = static_cast<int>(255.0f * alpha);

    draw->AddRectFilled(
        {min.x - 7.0f, min.y - 7.0f},
        {max.x + 7.0f, max.y + 7.0f},
        IM_COL32(183, 35, 56, static_cast<int>(alpha * 45.0f)),
        16.0f
    );

    draw->AddRectFilled(min, max, IM_COL32(29, 13, 19, static_cast<int>(a * 0.94f)), 12.0f);
    draw->AddRect(min, max, IM_COL32(205, 61, 82, static_cast<int>(a * 0.88f)), 12.0f);
    draw->AddRectFilled(min, {min.x + 4.0f, max.y}, IM_COL32(224, 65, 86, a), 4.0f);

    draw->AddCircleFilled({min.x + 24.0f, min.y + 24.0f}, 9.0f, IM_COL32(224, 65, 86, a));
    draw->AddLine(
        {min.x + 24.0f, min.y + 17.0f},
        {min.x + 24.0f, min.y + 26.0f},
        IM_COL32(255, 240, 242, a),
        3.0f
    );
    draw->AddCircleFilled({min.x + 24.0f, min.y + 31.0f}, 1.8f, IM_COL32(255, 240, 242, a));

    draw->AddText({min.x + 44.0f, min.y + 13.0f}, IM_COL32(255, 239, 241, a), "Ошибка входа");

    std::string line = errorText ? errorText : "";
    if (line.size() > 220) line = line.substr(0, 217) + "...";
    draw->AddText(
        ImGui::GetFont(),
        13.0f,
        {min.x + 44.0f, min.y + 40.0f},
        IM_COL32(227, 177, 185, a),
        line.c_str()
    );
}

void DrawLoadingScreen(HWND hwnd, unsigned int texture) {
    BeginRootWindow("##loading");

    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetWindowPos();
    const ImVec2 end(p.x + WIDTH, p.y + HEIGHT);

    draw->AddRectFilledMultiColor(
        p, end,
        IM_COL32(8, 9, 14, 255),
        IM_COL32(19, 21, 42, 255),
        IM_COL32(5, 15, 21, 255),
        IM_COL32(8, 9, 14, 255)
    );

    if (texture) {
        draw->AddImage(
            (ImTextureID)(intptr_t)texture,
            p, end,
            {0, 0}, {1, 1},
            IM_COL32(255, 255, 255, 90)
        );
        draw->AddRectFilled(p, end, IM_COL32(7, 9, 14, 150));
    }

    DrawWindowControls(hwnd);

    DrawSplashLogo(draw, {p.x + WIDTH * 0.5f, p.y + HEIGHT * 0.42f}, 1.15f);

    draw->AddText(
        ImGui::GetFont(),
        30.0f,
        {p.x + WIDTH * 0.5f - 100.0f, p.y + HEIGHT * 0.61f},
        IM_COL32(245, 246, 248, 255),
        "Amethyst Launcher"
    );

    draw->AddText(
        {p.x + WIDTH * 0.5f - 71.0f, p.y + HEIGHT * 0.67f},
        IM_COL32(172, 178, 190, 255),
        "Загрузка лаунчера..."
    );

    const float barW = 210.0f;
    const float barX = p.x + (WIDTH - barW) * 0.5f;
    const float barY = p.y + HEIGHT * 0.76f;
    const float segment = 62.0f;
    const float offset = fmodf(static_cast<float>(glfwGetTime()) * 150.0f, barW + segment) - segment;

    draw->AddRectFilled({barX, barY}, {barX + barW, barY + 3.0f}, IM_COL32(255, 255, 255, 35), 2.0f);
    draw->AddRectFilled({barX + offset, barY}, {barX + offset + segment, barY + 3.0f}, IM_COL32(188, 132, 255, 235), 2.0f);

    ImGui::End();
}

std::filesystem::path LoginFilePath() {
    return std::filesystem::path(FindExecutableDirectory()) / L"account.txt";
}

void SaveLoginName(const char* name) {
    std::ofstream file(LoginFilePath(), std::ios::binary | std::ios::trunc);
    file << name;
}

std::string LoadLoginName() {
    std::ifstream file(LoginFilePath(), std::ios::binary);
    if (!file.is_open()) return {};
    std::string s((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    return s;
}

void DrawGlassPanel(
    ImDrawList* draw,
    const ImVec2& winPos,
    const ImVec2& min,
    const ImVec2& max,
    float rounding,
    ImU32 tint
);

bool GlassInputText(
    ImDrawList* draw,
    const ImVec2& winPos,
    const char* id,
    const char* hint,
    char* buf,
    size_t bufSize,
    float width,
    const ImVec2& pos
);

bool GlassButton(
    ImDrawList* draw,
    const ImVec2& winPos,
    const char* id,
    const char* text,
    const ImVec2& size,
    const ImVec2& pos,
    ImU32 tint,
    ImU32 textColor
);

void DrawLoginScreen(
    HWND hwnd,
    unsigned int texture,
    char* loginName,
    size_t loginNameSize,
    bool& loggedIn,
    bool& loginError,
    double& loginErrorAt,
    bool& rememberMe,
    ElyAuthResult& elyResult,
    std::mutex& elyMutex,
    std::atomic<bool>& elyRunning
) {
    BeginRootWindow("##login");

    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetWindowPos();
    const ImVec2 end(p.x + WIDTH, p.y + HEIGHT);

    draw->AddRectFilledMultiColor(
        p, end,
        IM_COL32(9, 10, 16, 255),
        IM_COL32(18, 19, 37, 255),
        IM_COL32(7, 14, 19, 255),
        IM_COL32(8, 9, 14, 255)
    );

    if (texture) {
        draw->AddImage(
            (ImTextureID)(intptr_t)texture,
            p, end,
            {0, 0}, {1, 1},
            IM_COL32(255, 255, 255, 205)
        );

        draw->AddRectFilledMultiColor(
            p, end,
            IM_COL32(8, 9, 15, 92),
            IM_COL32(8, 9, 15, 120),
            IM_COL32(8, 9, 15, 195),
            IM_COL32(8, 9, 15, 215)
        );
    }

    DrawWindowControls(hwnd);

    draw->AddText(
        ImGui::GetFont(),
        34.0f,
        {p.x + 42.0f, p.y + 300.0f},
        IM_COL32(255, 255, 255, 255),
        "Amethyst Launcher"
    );

    draw->AddText(
        {p.x + 44.0f, p.y + 346.0f},
        IM_COL32(182, 186, 196, 255),
        "Amethyst Launcher — лаунчер Minecraft"
    );

    const float cardW = 386.0f;
    const float cardH = 378.0f;
    const float cardX = WIDTH - cardW - 60.0f;
    const float cardY = 155.0f;

    const ImVec2 cardMin(p.x + cardX, p.y + cardY);
    const ImVec2 cardMax(cardMin.x + cardW, cardMin.y + cardH);

    DrawGlassPanel(draw, p, cardMin, cardMax, 22.0f, IM_COL32(10, 12, 19, 175));
    draw->AddRect(cardMin, cardMax, IM_COL32(110, 100, 140, 190), 22.0f);

    draw->AddCircleFilled({cardMin.x + 49.0f, cardMin.y + 50.0f}, 17.0f, IM_COL32(188, 132, 255, 255));
    draw->AddText({cardMin.x + 44.0f, cardMin.y + 42.0f}, IM_COL32(30, 21, 50, 255), "A");

    ImGui::SetCursorPos({cardX + 32.0f, cardY + 31.0f});
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));

    ImGui::BeginChild(
        "##login-form",
        ImVec2(cardW - 64.0f, cardH - 58.0f),
        false,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse
    );

    ImGui::SetCursorPosX(52.0f);
    ImGui::TextColored(ImVec4(0.96f, 0.97f, 0.98f, 1.0f), "Amethyst");

    ImGui::SetCursorPosX(52.0f);
    ImGui::TextDisabled("Войдите в аккаунт проекта");

    ImGui::Dummy({0, 22});
    ImGui::TextDisabled("ИМЯ ПОЛЬЗОВАТЕЛЯ");

    {
        const ImVec2 inputPos = ImGui::GetCursorScreenPos();
        const float inputW = ImGui::GetContentRegionAvail().x;
        GlassInputText(draw, p, "##login-name", "Ваш игровой ник", loginName, loginNameSize, inputW, inputPos);
    }

    ImGui::Dummy({0, 10});

    {
        const ImVec2 btnPos = ImGui::GetCursorScreenPos();
        const float btnW = ImGui::GetContentRegionAvail().x;
        if (GlassButton(
                draw, p, "##login-enter", "ВОЙТИ",
                {btnW, 40.0f},
                btnPos,
                IM_COL32(75, 105, 160, 130),
                IM_COL32(240, 246, 255, 255)
            )) {
            if (HasVisibleText(loginName)) {
                if (rememberMe) SaveLoginName(loginName);
                loginError = false;
                loginErrorAt = -1.0;
                loggedIn = true;
            } else {
                g_LastLoginErrorText = "Введите игровой ник";
                loginError = true;
                loginErrorAt = glfwGetTime();
            }
        }
    }

    ImGui::Dummy({0, 8});
    ImGui::SetCursorPosX(52.0f);
    ImGui::TextDisabled("или");
    ImGui::Dummy({0, 8});

    ImGui::TextDisabled("ВОЙТИ ЧЕРЕЗ ELY.BY");
    ImGui::Dummy({0, 5});

    {
        const ImVec2 btnPos = ImGui::GetCursorScreenPos();
        const float btnW = ImGui::GetContentRegionAvail().x;
        if (GlassButton(
                draw, p, "##login-ely", elyRunning ? "ОЖИДАНИЕ ELY.BY" : "ВОЙТИ ЧЕРЕЗ ELY.BY",
                {btnW, 40.0f},
                btnPos,
                IM_COL32(160, 110, 240, 130),
                IM_COL32(246, 240, 255, 255)
            )) {
            if (!elyRunning) {
                {
                    std::lock_guard lock(elyMutex);
                    elyResult = {};
                }
                g_LastLoginErrorText = "Ожидание Ely.by...";
                loginError = false;
                loginErrorAt = -1.0;
                elyRunning = true;
                std::thread(runElyAuth, std::ref(elyResult), std::ref(elyMutex), std::ref(elyRunning)).detach();
            }
        }
    }

    ImGui::Dummy({0, 8});
    {
        const ImVec2 cbPos = ImGui::GetCursorScreenPos();
        const float cbH = ImGui::GetFrameHeight();
        const float cbW = ImGui::GetContentRegionAvail().x + 8.0f;
        DrawGlassPanel(draw, p, cbPos, {cbPos.x + cbW, cbPos.y + cbH}, 6.0f, IM_COL32(12, 16, 24, 110));

        ImGui::PushStyleColor(ImGuiCol_CheckMark, ImVec4(0.74f, 0.52f, 1.0f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(1.0f, 1.0f, 1.0f, 0.05f));
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(1.0f, 1.0f, 1.0f, 0.08f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.88f, 0.89f, 0.92f, 1.0f));
        ImGui::Checkbox("  Запомнить меня", &rememberMe);
        ImGui::PopStyleColor(5);
    }

    ImGui::Dummy({0, 4});
    if (elyResult.ready) {
        if (elyResult.success) {
            strncpy_s(loginName, loginNameSize, elyResult.username.c_str(), _TRUNCATE);
            if (rememberMe) SaveLoginName(loginName);
            loginError = false;
            loginErrorAt = -1.0;
            loggedIn = true;
        } else {
            g_LastLoginErrorText = elyResult.error.empty()
                ? "Не удалось войти через Ely.by"
                : elyResult.error;
            loginError = true;
            loginErrorAt = glfwGetTime();
        }
        elyResult.ready = false;
    }

    ImGui::TextDisabled("Нет аккаунта?");
    ImGui::SameLine();

    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.84f, 0.66f, 1.0f, 1.0f));
    if (ImGui::SmallButton("Зарегистрироваться")) {
        ShellExecuteA(nullptr, "open", "https://t.me/lemonserver_bot", nullptr, nullptr, SW_SHOWNORMAL);
    }
    ImGui::PopStyleColor();

    ImGui::EndChild();
    ImGui::PopStyleColor();

    DrawLoginToast(draw, p, loginError, loginErrorAt, g_LastLoginErrorText.c_str());

    ImGui::End();
}

bool launcherIsOwner(const std::string& name) {
    return name == "Kira_lf";
}

bool launcherIsAdmin(const std::string& name) {
    static const char* adminNames[] = {
        "Kira_lf", "CreeperTron123", "DenShaRp", "CreeperTron1014", "popa123"
    };
    for (const char* admin : adminNames) {
        if (name == admin) return true;
    }
    return false;
}

const char* launcherPrefix(const std::string& name) {
    if (launcherIsOwner(name)) return "Владелец сервера";
    if (launcherIsAdmin(name)) return "Админ сервера";
    return "Игрок";
}

void DrawGlassPanel(ImDrawList* draw, const ImVec2& winPos, const ImVec2& min, const ImVec2& max, float rounding, ImU32 tint) {
    if (gBlurBackgroundTex) {
        draw->AddImageRounded(
            (ImTextureID)(intptr_t)gBlurBackgroundTex,
            min, max,
            {(min.x - winPos.x) / WIDTH, (min.y - winPos.y) / HEIGHT},
            {(max.x - winPos.x) / WIDTH, (max.y - winPos.y) / HEIGHT},
            IM_COL32(255, 255, 255, 255),
            rounding
        );
    }
    draw->AddRectFilled(min, max, tint, rounding);
    draw->AddRect(min, max, IM_COL32(255, 255, 255, 22), rounding);
}

bool DrawToggleSwitch(ImDrawList* draw, const ImVec2& winPos, const char* id, bool* value, const ImVec2& pos) {
    const float w = 46.0f;
    const float h = 26.0f;

    const bool on = *value;

    const ImU32 tint = on
        ? IM_COL32(150, 104, 230, 115)
        : IM_COL32(22, 28, 40, 135);

    DrawGlassPanel(draw, winPos, pos, {pos.x + w, pos.y + h}, h * 0.5f, tint);

    ImGui::SetCursorScreenPos(pos);
    ImGui::InvisibleButton(id, ImVec2(w, h));

    const bool clicked = ImGui::IsItemClicked();
    if (clicked) {
        *value = !*value;
    }
    if (ImGui::IsItemHovered()) {
        draw->AddRectFilled(pos, {pos.x + w, pos.y + h}, IM_COL32(255, 255, 255, 14), h * 0.5f);
    }

    const float pad = 3.0f;
    const float knobD = h - pad * 2.0f;
    const float knobX = on ? pos.x + w - pad - knobD : pos.x + pad;
    draw->AddCircleFilled(
        {knobX + knobD * 0.5f, pos.y + h * 0.5f},
        knobD * 0.5f,
        on ? IM_COL32(240, 228, 255, 255) : IM_COL32(208, 214, 222, 255),
        32
    );

    return clicked;
}

bool GlassSliderInt(
    ImDrawList* draw,
    const ImVec2& winPos,
    const char* id,
    int* value,
    int vmin,
    int vmax,
    float width,
    const ImVec2& pos
) {
    const float h = 30.0f;
    const ImVec2 min(pos.x, pos.y);
    const ImVec2 max(pos.x + width, pos.y + h);

    DrawGlassPanel(draw, winPos, min, max, 8.0f, IM_COL32(12, 16, 24, 125));

    ImGui::SetCursorScreenPos(min);
    ImGui::SetNextItemWidth(width);

    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(1.0f, 1.0f, 1.0f, 0.06f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(1.0f, 1.0f, 1.0f, 0.10f));
    ImGui::PushStyleColor(ImGuiCol_SliderGrab, ImVec4(0.74f, 0.52f, 1.0f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, ImVec4(0.84f, 0.66f, 1.0f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.90f, 0.92f, 0.96f, 1.0f));

    ImGui::SliderInt(id, value, vmin, vmax, "");

    ImGui::PopStyleColor(6);
    ImGui::PopStyleVar();

    return ImGui::IsItemDeactivatedAfterEdit();
}

bool GlassInputText(
    ImDrawList* draw,
    const ImVec2& winPos,
    const char* id,
    const char* hint,
    char* buf,
    size_t bufSize,
    float width,
    const ImVec2& pos
) {
    const float h = 32.0f;
    const ImVec2 min(pos.x, pos.y);
    const ImVec2 max(pos.x + width, pos.y + h);

    DrawGlassPanel(draw, winPos, min, max, 8.0f, IM_COL32(12, 16, 24, 125));

    ImGui::SetCursorScreenPos(min);
    ImGui::SetNextItemWidth(width);

    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(1.0f, 1.0f, 1.0f, 0.06f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(1.0f, 1.0f, 1.0f, 0.06f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.86f, 0.89f, 0.94f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_TextSelectedBg, ImVec4(0.74f, 0.52f, 1.0f, 0.35f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1.0f, 1.0f, 1.0f, 0.10f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);

    ImGui::InputTextWithHint(id, hint, buf, bufSize);

    ImGui::PopStyleVar();
    ImGui::PopStyleColor(6);
    ImGui::PopStyleVar();

    return ImGui::IsItemDeactivatedAfterEdit();
}

bool GlassButton(
    ImDrawList* draw,
    const ImVec2& winPos,
    const char* id,
    const char* text,
    const ImVec2& size,
    const ImVec2& pos,
    ImU32 tint,
    ImU32 textColor
) {
    const float fontSize = 15.0f;
    const float rounding = 10.0f;

    DrawGlassPanel(draw, winPos, pos, {pos.x + size.x, pos.y + size.y}, rounding, tint);

    ImGui::SetCursorScreenPos(pos);
    ImGui::InvisibleButton(id, size);

    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();
    const bool clicked = ImGui::IsItemClicked();

    if (active || hovered) {
        draw->AddRectFilled(
            pos,
            {pos.x + size.x, pos.y + size.y},
            IM_COL32(255, 255, 255, active ? 28 : 15),
            rounding
        );
    }

    const ImVec2 tSize = ImGui::GetFont()->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, text);
    draw->AddText(
        ImGui::GetFont(),
        fontSize,
        {pos.x + (size.x - tSize.x) * 0.5f, pos.y + (size.y - tSize.y) * 0.5f},
        textColor,
        text
    );

    return clicked;
}

void DrawLauncherScreen(
    HWND hwnd,
    unsigned int texture,
    const char* loginName,
    int& launcherPage,
    InstallState& installState,
    std::string& launchError,
    double& launchErrorAt,
    bool useMods,
    bool& autoJoinServer,
    char* serverAddress,
    size_t serverAddressSize,
    int& maxMemoryMB,
    bool& discordRpcEnabled,
    ElyAuthResult& elyResult,
    std::mutex& elyMutex
) {
    BeginRootWindow("##launcher");

    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetWindowPos();
    const ImVec2 end(p.x + WIDTH, p.y + HEIGHT);

    constexpr float sidebarWidth = 96.0f;

    if (texture) {
        draw->AddImage(
            (ImTextureID)(intptr_t)texture,
            p, end,
            {0, 0}, {1, 1},
            IM_COL32(255, 255, 255, 255)
        );
    } else {
        draw->AddRectFilled(p, end, IM_COL32(16, 22, 30, 255));
    }

    draw->AddRectFilled(p, end, IM_COL32(4, 8, 13, 135));

    const ImVec2 sidebarEnd(p.x + sidebarWidth, p.y + HEIGHT);

    if (gBlurBackgroundTex) {
        draw->AddImage(
            (ImTextureID)(intptr_t)gBlurBackgroundTex,
            p, sidebarEnd,
            {0.0f, 0.0f},
            {(sidebarEnd.x - p.x) / WIDTH, (sidebarEnd.y - p.y) / HEIGHT},
            IM_COL32(255, 255, 255, 255)
        );
    }

    draw->AddRectFilledMultiColor(
        p, sidebarEnd,
        IM_COL32(9, 13, 20, 185),
        IM_COL32(10, 14, 22, 178),
        IM_COL32(7, 10, 17, 182),
        IM_COL32(7, 10, 17, 185)
    );

    draw->AddLine(
        {p.x + sidebarWidth, p.y},
        {p.x + sidebarWidth, p.y + HEIGHT},
        IM_COL32(188, 132, 255, 105),
        1.0f
    );

    DrawSplashLogo(draw, {p.x + sidebarWidth * 0.5f, p.y + 39.0f}, 0.65f);

    draw->AddLine(
        {p.x + 20.0f, p.y + 78.0f},
        {p.x + sidebarWidth - 20.0f, p.y + 78.0f},
        IM_COL32(255, 255, 255, 25),
        1.0f
    );

    DrawWindowControls(hwnd);

    const float navBtnD = 44.0f;
    const float navSpacing = 12.0f;
    float navY = (HEIGHT - (navBtnD * 4.0f + navSpacing * 3.0f)) / 2.0f;
    const float navX = (sidebarWidth - navBtnD) * 0.5f;

    auto navButton = [&](unsigned int texture, const char* title, int page) {
        const bool active = launcherPage == page;

        const ImVec2 buttonPos = ImVec2(navX, navY);
        const ImVec2 buttonSize = ImVec2(navBtnD, navBtnD);
        const ImVec2 buttonCenter = ImVec2(buttonPos.x + navBtnD * 0.5f, buttonPos.y + navBtnD * 0.5f);

        ImGui::SetCursorPos(buttonPos);
        ImGui::PushID(page);

        ImGui::InvisibleButton(title, buttonSize);

        const bool hovered = ImGui::IsItemHovered();
        const bool pressed = ImGui::IsItemActive();

        const ImU32 glassTintBg = IM_COL32(24, 30, 42, 135);
        const float glassRound = navBtnD * 0.5f;
        const ImVec2 buttonEnd(buttonPos.x + navBtnD, buttonPos.y + navBtnD);

        if (active) {
            DrawGlassPanel(draw, p, buttonPos, buttonEnd, glassRound, IM_COL32(170, 115, 245, 65));
            draw->AddCircle(buttonCenter, navBtnD * 0.5f, IM_COL32(188, 132, 255, 235), 48, 2.0f);
        } else if (pressed) {
            DrawGlassPanel(draw, p, buttonPos, buttonEnd, glassRound, glassTintBg);
            draw->AddCircleFilled(buttonCenter, navBtnD * 0.5f, IM_COL32(255, 255, 255, 20), 48);
        } else {
            DrawGlassPanel(draw, p, buttonPos, buttonEnd, glassRound, glassTintBg);
            if (hovered) {
                draw->AddCircleFilled(buttonCenter, navBtnD * 0.5f, IM_COL32(255, 255, 255, 12), 48);
            }
        }

        if (texture) {
            const float iconSize = 22.0f;
            draw->AddImage(
                (ImTextureID)(intptr_t)texture,
                ImVec2(buttonCenter.x - iconSize * 0.5f, buttonCenter.y - iconSize * 0.5f),
                ImVec2(buttonCenter.x + iconSize * 0.5f, buttonCenter.y + iconSize * 0.5f)
            );
        }

        if (ImGui::IsItemClicked()) {
            launcherPage = page;
        }

        if (hovered) {
            ImGui::SetTooltip("%s", title);
        }

        ImGui::PopID();

        navY += navBtnD + navSpacing;
    };

    navButton(gIconProfileTex, "Профиль", 0);
    navButton(gIconBuildsTex, "Сборки", 1);
    navButton(gIconSettingsTex, "Настройки", 2);
    navButton(gIconInfoTex, "Информация", 3);

    const char* pageTitles[] = {
        "ПРОФИЛЬ",
        "СБОРКИ",
        "НАСТРОЙКИ",
        "ИНФОРМАЦИЯ"
    };

    const char* pageDescriptions[] = {
        "Ваш профиль и аккаунт",
        "Выберите сборку перед запуском игры",
        "Настройки лаунчера",
        "Новости и информация о лаунчере"
    };

    const ImVec2 contentMin(p.x + sidebarWidth + 42.0f, p.y + 42.0f);
    const ImVec2 contentMax(p.x + WIDTH - 36.0f, p.y + HEIGHT - 48.0f);

    if (gBlurBackgroundTex) {
        draw->AddImageRounded(
            (ImTextureID)(intptr_t)gBlurBackgroundTex,
            contentMin, contentMax,
            {(contentMin.x - p.x) / WIDTH, (contentMin.y - p.y) / HEIGHT},
            {(contentMax.x - p.x) / WIDTH, (contentMax.y - p.y) / HEIGHT},
            IM_COL32(255, 255, 255, 255),
            18.0f
        );
    }
    draw->AddRectFilled(contentMin, contentMax, IM_COL32(8, 12, 18, 132), 18.0f);
    draw->AddRect(contentMin, contentMax, IM_COL32(255, 255, 255, 30), 18.0f);

    draw->AddText(
        ImGui::GetFont(),
        26.0f,
        {contentMin.x + 28.0f, contentMin.y + 26.0f},
        IM_COL32(255, 255, 255, 255),
        pageTitles[launcherPage]
    );

    draw->AddText(
        {contentMin.x + 29.0f, contentMin.y + 62.0f},
        IM_COL32(185, 193, 205, 245),
        pageDescriptions[launcherPage]
    );

    draw->AddLine(
        {contentMin.x + 28.0f, contentMin.y + 96.0f},
        {contentMax.x - 28.0f, contentMin.y + 96.0f},
        IM_COL32(255, 255, 255, 28),
        1.0f
    );

    if (launcherPage == 1) {
        const std::wstring gameDirectory = FindExecutableDirectory() + L"\\..\\minecraft";
        const std::string installStatus = GetInstallStatusText(installState);
        const bool installed = installStatus == "installed";
        const bool installing = installStatus == "installing";

        InstallPhase phase;
        std::string detail;
        int total = 0;
        int current = 0;
        std::string phaseTextCopy;
        std::string errorCopy;
        {
            std::lock_guard lock(installState.mtx);
            phase = installState.phase;
            detail = installState.phaseDetail;
            total = installState.total;
            current = installState.current;
            phaseTextCopy = installState.phaseText;
            errorCopy = installState.error;
        }

        const char* statusText;
        ImU32 statusColor;

        if (installed) {
            statusText = "Установлено";
            statusColor = IM_COL32(112, 212, 133, 255);
        } else if (phase == InstallPhase::Error) {
            statusText = errorCopy.c_str();
            statusColor = IM_COL32(230, 91, 91, 255);
        } else if (installing) {
            statusText = phaseTextCopy.c_str();
            statusColor = IM_COL32(190, 164, 224, 255);
        } else {
            statusText = "Не установлено";
            statusColor = IM_COL32(176, 150, 218, 255);
        }

        const float cx = contentMin.x + 28.0f;
        const float cy = contentMin.y + 124.0f;
        const float rightX = contentMax.x - 28.0f;
        const float cw = rightX - cx;

        draw->AddText(
            ImGui::GetFont(),
            24.0f,
            {cx, cy},
            IM_COL32(250, 250, 253, 255),
            "Fabric 1.21.11"
        );

        const ImVec2 stsz = ImGui::GetFont()->CalcTextSizeA(15.0f, FLT_MAX, 0.0f, statusText);
        const float stx = rightX - stsz.x;
        draw->AddCircleFilled({stx - 13.0f, cy + 15.0f}, 4.5f, statusColor);
        draw->AddText({stx, cy + 7.0f}, statusColor, statusText);

        draw->AddText(
            {cx, cy + 42.0f},
            IM_COL32(184, 193, 205, 225),
            "Ванила+ сборка для Amethyst сервера — серая версия Minecraft 1.21.11 с модами"
        );
        draw->AddText(
            {cx, cy + 66.0f},
            IM_COL32(160, 168, 180, 190),
            "Майнкрафт и Fabric будут установлены в папку minecraft рядом с лаунчером"
        );

        draw->AddLine(
            {cx, cy + 102.0f},
            {rightX, cy + 102.0f},
            IM_COL32(255, 255, 255, 28),
            1.0f
        );

        const float controlY = cy + 124.0f;
        const float uiX = sidebarWidth + 70.0f;

        if (installing) {
            float pct = (total > 0) ? (static_cast<float>(current) / static_cast<float>(total)) : 0.0f;
            char pctBuf[32];
            snprintf(pctBuf, sizeof(pctBuf), "%.0f%%", pct * 100.0f);
            draw->AddText(
                ImGui::GetFont(),
                18.0f,
                {rightX - 60.0f, controlY},
                IM_COL32(188, 132, 255, 255),
                pctBuf
            );

            draw->AddRectFilled(
                {cx, controlY + 28.0f},
                {rightX, controlY + 34.0f},
                IM_COL32(255, 255, 255, 26),
                3.0f
            );
            draw->AddRectFilled(
                {cx, controlY + 28.0f},
                {cx + cw * pct, controlY + 34.0f},
                IM_COL32(188, 132, 255, 240),
                3.0f
            );

            if (!detail.empty()) {
                draw->AddText(
                    {cx, controlY + 46.0f},
                    IM_COL32(160, 168, 180, 230),
                    detail.c_str()
                );
            }
            if (total > 0) {
                char cntBuf[48];
                snprintf(cntBuf, sizeof(cntBuf), "%d / %d", current, total);
                draw->AddText(
                    {cx, controlY + 70.0f},
                    IM_COL32(150, 156, 168, 200),
                    cntBuf
                );
            }

            if (GlassButton(
                    draw, p, "##cancel-install", "ОТМЕНИТЬ",
                    {254.0f, 44.0f},
                    {uiX, controlY + 102.0f},
                    IM_COL32(200, 60, 60, 120),
                    IM_COL32(255, 232, 234, 255)
                )) {
                installState.cancel = true;
            }
        } else {
            const int projectMods = CountProjectMods();
            char modsInfo[96];
            snprintf(modsInfo, sizeof(modsInfo), "Модов в папке mods: %d", projectMods);
            ImGui::SetCursorPos({uiX, controlY});
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.60f, 0.66f, 0.75f, 1.0f));
            ImGui::Text("%s", modsInfo);
            if (projectMods == 0) {
                ImGui::SetCursorPos({uiX, controlY + 22.0f});
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.50f, 0.55f, 0.63f, 1.0f));
                ImGui::TextDisabled("Положите .jar моды рядом с лаунчером в папку mods");
                ImGui::PopStyleColor();
            }
            ImGui::PopStyleColor();

            if (installed) {
                if (GlassButton(
                        draw, p, "##btn-launch", "ЗАПУСТИТЬ ИГРУ",
                        {254.0f, 48.0f},
                        {uiX, controlY + 58.0f},
                        IM_COL32(55, 175, 85, 115),
                        IM_COL32(236, 255, 241, 255)
                    )) {
                    std::filesystem::path gd = std::filesystem::path(FindExecutableDirectory()) / ".." / "minecraft";
                    LaunchConfig lc;
                    lc.gameDir = gd;
                    lc.mcVersion = "1.21.11";
                    lc.username = HasVisibleText(loginName) ? loginName : "Игрок";
                    {
                        std::lock_guard lock(elyMutex);
                        lc.uuid        = elyResult.uuid;
                        lc.accessToken = elyResult.accessToken;
                        if (!elyResult.username.empty()) {
                            lc.username = elyResult.username;
                        }
                    }
                    lc.useFabric = useMods;
                    lc.modsDir = std::filesystem::path(FindExecutableDirectory()) / ".." / "mods";
                    lc.serverAddress = autoJoinServer ? std::string(serverAddress) : "";
                    lc.maxMemoryMB = maxMemoryMB;
                    LaunchResult lr = launchGame(lc);
                    if (!lr.success) {
                        launchError = lr.error;
                        launchErrorAt = glfwGetTime();
                    } else {
                        launchError.clear();
                        launchErrorAt = -1.0;
                        gDiscordInGame = true;
                        UpdateDiscordPresence(lc.username.c_str());
                    }
                }

                if (GlassButton(
                        draw, p, "##btn-reinstall", "ПЕРЕУСТАНОВИТЬ",
                        {254.0f, 42.0f},
                        {uiX, controlY + 114.0f},
                        IM_COL32(35, 45, 60, 120),
                        IM_COL32(210, 220, 235, 255)
                    )) {
                    std::filesystem::path gd = std::filesystem::path(FindExecutableDirectory()) / ".." / "minecraft";
                    startInstallThread(gd, "1.21.11", installState);
                }
            } else {
                if (GlassButton(
                        draw, p, "##btn-install", "УСТАНОВИТЬ FABRIC",
                        {254.0f, 48.0f},
                        {uiX, controlY + 58.0f},
                        IM_COL32(160, 110, 240, 125),
                        IM_COL32(246, 240, 255, 255)
                    )) {
                    std::filesystem::path gd = std::filesystem::path(FindExecutableDirectory()) / ".." / "minecraft";
                    startInstallThread(gd, "1.21.11", installState);
                }
            }
        }

        if (!launchError.empty() && launchErrorAt >= 0.0) {
            float age = static_cast<float>(glfwGetTime() - launchErrorAt);
            if (age < 5.0f) {
                float alpha = 1.0f;
                if (age < 0.2f) alpha = age / 0.2f;
                else if (age > 4.2f) alpha = (5.0f - age) / 0.8f;

                float toastW = 340.0f;
                float toastH = 72.0f;
                float toastX = contentMin.x + 20.0f;
                float toastY = contentMax.y - toastH - 20.0f;

                draw->AddRectFilled(
                    {toastX - 6.0f, toastY - 6.0f},
                    {toastX + toastW + 6.0f, toastY + toastH + 6.0f},
                    IM_COL32(183, 35, 56, static_cast<int>(alpha * 45.0f)),
                    14.0f
                );
                draw->AddRectFilled(
                    {toastX, toastY},
                    {toastX + toastW, toastY + toastH},
                    IM_COL32(29, 13, 19, static_cast<int>(alpha * 240.0f)),
                    10.0f
                );
                draw->AddRect(
                    {toastX, toastY},
                    {toastX + toastW, toastY + toastH},
                    IM_COL32(205, 61, 82, static_cast<int>(alpha * 220.0f)),
                    10.0f
                );
                draw->AddRectFilled(
                    {toastX, toastY},
                    {toastX + 4.0f, toastY + toastH},
                    IM_COL32(224, 65, 86, static_cast<int>(alpha * 255.0f)),
                    4.0f
                );

                int a255 = static_cast<int>(alpha * 255.0f);
                draw->AddCircleFilled({toastX + 22.0f, toastY + 22.0f}, 9.0f, IM_COL32(224, 65, 86, a255));
                draw->AddLine(
                    {toastX + 22.0f, toastY + 15.0f},
                    {toastX + 22.0f, toastY + 24.0f},
                    IM_COL32(255, 240, 242, a255), 3.0f
                );
                draw->AddCircleFilled({toastX + 22.0f, toastY + 29.0f}, 1.8f, IM_COL32(255, 240, 242, a255));

                draw->AddText(
                    {toastX + 42.0f, toastY + 12.0f},
                    IM_COL32(255, 239, 241, a255),
                    "Ошибка запуска"
                );
                draw->AddText(
                    {toastX + 42.0f, toastY + 36.0f},
                    IM_COL32(227, 177, 185, a255),
                    launchError.c_str()
                );
            } else {
                launchError.clear();
                launchErrorAt = -1.0;
            }
        }
    } else if (launcherPage == 2) {
        const float sx = contentMin.x + 29.0f;
        const float ex = contentMax.x - 29.0f;
        const float sy = contentMin.y + 124.0f;

        auto settingLabel = [&](const char* text, float y) {
            draw->AddText(
                ImGui::GetFont(),
                15.0f,
                {sx, y},
                IM_COL32(150, 158, 172, 255),
                text
            );
        };

        auto settingDesc = [&](const char* text, float y) {
            draw->AddText(
                {sx, y},
                IM_COL32(130, 138, 152, 255),
                text
            );
        };

        settingLabel("ОПЕРАТИВНАЯ ПАМЯТЬ", sy);

        char memValue[32];
        snprintf(memValue, sizeof(memValue), "%d МБ", maxMemoryMB);
        const ImVec2 memSize = ImGui::GetFont()->CalcTextSizeA(15.0f, FLT_MAX, 0.0f, memValue);
        draw->AddText({ex - memSize.x, sy}, IM_COL32(222, 226, 233, 255), memValue);

        GlassSliderInt(
            draw,
            p,
            "##memory-slider",
            &maxMemoryMB,
            1024,
            16384,
            400.0f,
            {contentMin.x + 29.0f, contentMin.y + 154.0f}
        );

        draw->AddLine({sx, sy + 82.0f}, {ex, sy + 82.0f}, IM_COL32(255, 255, 255, 28), 1.0f);

        settingLabel("АВТОВХОД НА СЕРВЕР", sy + 104.0f);
        settingDesc("Автоматически заходить на сервер при запуске игры", sy + 130.0f);
        DrawToggleSwitch(draw, p, "##autojoin-toggle", &autoJoinServer, {ex - 46.0f, sy + 100.0f});

        if (autoJoinServer) {
            GlassInputText(
                draw,
                p,
                "##server-ip",
                "IP сервера (напр. lemon.mineserver.xyz)",
                serverAddress,
                serverAddressSize,
                400.0f,
                {contentMin.x + 29.0f, contentMin.y + 282.0f}
            );
        }

        draw->AddLine({sx, sy + 208.0f}, {ex, sy + 208.0f}, IM_COL32(255, 255, 255, 28), 1.0f);

        settingLabel("DISCORD RICH PRESENCE", sy + 230.0f);
        settingDesc("Показывать статус лаунчера в Discord", sy + 256.0f);
        if (DrawToggleSwitch(draw, p, "##rpc-toggle", &discordRpcEnabled, {ex - 46.0f, sy + 226.0f})) {
            if (gDiscordReady) {
                if (discordRpcEnabled) {
                    gDiscordInGame = false;
                    UpdateDiscordPresence(loginName);
                } else {
                    gDiscordInGame = false;
                    Discord_ClearPresence();
                }
            }
        }
    } else if (launcherPage == 3) {
        const float imgX = contentMin.x + 28.0f;
        const float imgY = contentMin.y + 116.0f;
        const float imgW = (contentMax.x - contentMin.x) - 56.0f;
        const float imgH = contentMax.y - contentMin.y - 116.0f;
        const float gap = 14.0f;

        const float thumbGap = 2.0f;
        const float thumbH = 90.0f;
        const float thumbW = 120.0f;
        const float bottom = imgY + imgH;

        const float fourBoxW = 500.0f;
        const float fourBoxH = 200.0f;
        const float fourY = imgY - 30.0f;
        const float fourX = imgX + (imgW - fourBoxW) * 0.5f;

        struct ImageSlot {
            int index;
            ImVec2 tl;
            ImVec2 br;
        };

        const ImageSlot slots[] = {
            {3, {fourX, fourY}, {fourX + fourBoxW, fourY + fourBoxH}},
            {0, {imgX, bottom - thumbH}, {imgX + thumbW, bottom}},
            {1, {imgX + thumbW + thumbGap, bottom - thumbH}, {imgX + 2.0f * thumbW + thumbGap, bottom}},
            {2, {imgX, bottom - 2.0f * thumbH - gap}, {imgX + thumbW, bottom - thumbH - gap}}
        };

        for (const ImageSlot& slot : slots) {
            const InfoImage& info = gInfoImages[slot.index];
            if (!info.tex || info.width <= 0 || info.height <= 0) continue;

            const float boxW = slot.br.x - slot.tl.x;
            const float boxH = slot.br.y - slot.tl.y;
            const float scale = std::min(boxW / static_cast<float>(info.width), boxH / static_cast<float>(info.height));
            const float drawW = static_cast<float>(info.width) * scale;
            const float drawH = static_cast<float>(info.height) * scale;
            const ImVec2 drawTL(slot.tl.x + (boxW - drawW) * 0.5f, slot.tl.y + (boxH - drawH) * 0.5f);
            draw->AddImage((ImTextureID)(intptr_t)info.tex, drawTL, {drawTL.x + drawW, drawTL.y + drawH});
        }

        const float panelCenterX = contentMin.x + (contentMax.x - contentMin.x) * 0.5f;
        const float textCenterY = imgY + 178.0f;
        ImFont* font = ImGui::GetFont();

        const char* infoTitle = "Amethyst server";
        const float infoTitleSize = 23.0f;
        const ImVec2 infoTitleSizeV = font->CalcTextSizeA(infoTitleSize, FLT_MAX, -1.0f, infoTitle);
        draw->AddText(
            font,
            infoTitleSize,
            {panelCenterX - infoTitleSizeV.x * 0.5f, textCenterY - 28.0f},
            IM_COL32(188, 132, 255, 255),
            infoTitle
        );

        const char* infoLines[] = {
            "Сервер двух флудов в одной атмосфере. Ванила+.",
            "Будут играть: тех, Админы и ВЛД",
            "Доступны: войс-чат, эмоции, пат-пат и поглаживание"
        };
        float lineY = textCenterY - 2.0f;
        for (const char* line : infoLines) {
            const ImVec2 lineSize = font->CalcTextSizeA(16.0f, FLT_MAX, -1.0f, line);
            draw->AddText(
                {panelCenterX - lineSize.x * 0.5f, lineY},
                IM_COL32(205, 214, 226, 255),
                line
            );
            lineY += lineSize.y + 3.0f;
        }
    } else {
        const char* nickText = HasVisibleText(loginName) ? loginName : "Игрок";
        const char* prefixText = launcherPrefix(nickText);

        ImU32 prefixColor;
        if (launcherIsOwner(nickText)) {
            prefixColor = IM_COL32(176, 132, 255, 255);
        } else if (launcherIsAdmin(nickText)) {
            prefixColor = IM_COL32(238, 130, 85, 255);
        } else {
            prefixColor = IM_COL32(147, 156, 170, 255);
        }

        ImFont* font = ImGui::GetFont();

        const float nameStartX = contentMin.x + 28.0f;
        draw->AddText(
            font,
            32.0f,
            {nameStartX, contentMin.y + 150.0f},
            IM_COL32(248, 250, 253, 255),
            nickText
        );

        bool isElyAuthenticated = false;
        {
            std::lock_guard lock(elyMutex);
            isElyAuthenticated = !elyResult.accessToken.empty();
        }
        if (isElyAuthenticated) {
            const ImVec2 nickSize = font->CalcTextSizeA(32.0f, FLT_MAX, 0.0f, nickText);
            const ImVec2 badgeTextSize = font->CalcTextSizeA(13.0f, FLT_MAX, 0.0f, "ely.by");
            const float badgePadX = 9.0f;
            const float badgePadY = 5.0f;
            const ImVec2 badgeTL(nameStartX + nickSize.x + 12.0f, contentMin.y + 153.0f);
            const ImVec2 badgeBR(
                badgeTL.x + badgeTextSize.x + badgePadX * 2.0f,
                badgeTL.y + badgeTextSize.y + badgePadY * 2.0f
            );
            draw->AddRectFilled(badgeTL, badgeBR, IM_COL32(38, 150, 82, 220), 7.0f);
            draw->AddRect(badgeTL, badgeBR, IM_COL32(95, 218, 132, 230), 7.0f);
            draw->AddText(
                font,
                13.0f,
                {badgeTL.x + badgePadX, badgeTL.y + badgePadY},
                IM_COL32(225, 255, 233, 255),
                "ely.by"
            );
        }

        char welcomeBuf[160];
        if (launcherIsOwner(nickText)) {
            snprintf(welcomeBuf, sizeof(welcomeBuf), "Добро пожаловать домой, %s!", nickText);
        } else if (launcherIsAdmin(nickText)) {
            snprintf(welcomeBuf, sizeof(welcomeBuf), "Рады видеть вас, %s!", nickText);
        } else {
            snprintf(welcomeBuf, sizeof(welcomeBuf), "Приятной игры, %s!", nickText);
        }
        draw->AddText(
            {nameStartX + 3.0f, contentMin.y + 196.0f},
            IM_COL32(163, 171, 185, 255),
            welcomeBuf
        );

        const ImVec2 chipTextSize = font->CalcTextSizeA(15.0f, FLT_MAX, 0.0f, prefixText);
        const float chipPadX = 14.0f;
        const float chipPadY = 6.0f;
        const ImVec2 chipTL(nameStartX + 1.0f, contentMin.y + 232.0f);
        const ImVec2 chipBR(chipTL.x + chipTextSize.x + chipPadX * 2.0f, chipTL.y + chipTextSize.y + chipPadY * 2.0f);
        DrawGlassPanel(
            draw,
            p,
            chipTL,
            chipBR,
            9.0f,
            (prefixColor & 0x00FFFFFF) | (70U << 24)
        );
        draw->AddRect(chipTL, chipBR, (prefixColor & 0x00FFFFFF) | (120U << 24), 9.0f);
        draw->AddText({chipTL.x + chipPadX, chipTL.y + chipPadY}, prefixColor, prefixText);
    }

    ImGui::End();
}

int main(int, char**) {
    ShowWindow(GetConsoleWindow(), SW_HIDE);

    glfwSetErrorCallback([](int, const char* description) {
        OutputDebugStringA(description);
        OutputDebugStringA("\n");
    });

    if (!glfwInit()) {
        return -1;
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);
    glfwWindowHint(GLFW_TRANSPARENT_FRAMEBUFFER, GLFW_TRUE);
    glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);

    GLFWmonitor* monitor = glfwGetPrimaryMonitor();
    if (monitor) {
        const GLFWvidmode* mode = glfwGetVideoMode(monitor);

        if (mode) {
            glfwWindowHint(GLFW_POSITION_X, (mode->width - WIDTH) / 2);
            glfwWindowHint(GLFW_POSITION_Y, (mode->height - HEIGHT) / 2);
        }
    }

    GLFWwindow* window = glfwCreateWindow(WIDTH, HEIGHT, "Amethyst Launcher", nullptr, nullptr);

    if (!window) {
        glfwTerminate();
        return -1;
    }

    HWND hwnd = glfwGetWin32Window(window);

    if (hwnd) {
        g_originalWndProc = reinterpret_cast<WNDPROC>(
            SetWindowLongPtr(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&LauncherWndProc))
        );

        SetWindowLongPtrA(
            hwnd,
            GWL_EXSTYLE,
            GetWindowLongPtrA(hwnd, GWL_EXSTYLE) | WS_EX_LAYERED
        );

        SetLayeredWindowAttributes(hwnd, RGB(0, 0, 0), 255, LWA_ALPHA);

        MARGINS margins = {-1};
        DwmExtendFrameIntoClientArea(hwnd, &margins);
        ApplyBlur(hwnd);
    }

    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    Gdiplus::GdiplusStartupInput gdiplusInput;
    ULONG_PTR gdiplusToken = 0;
    Gdiplus::GdiplusStartup(&gdiplusToken, &gdiplusInput, nullptr);

    const std::wstring backgroundPath = FindBackgroundPath();
    const unsigned int backgroundTexture = LoadLocalTexture(backgroundPath.c_str());

    {
        int blurW = 0;
        int blurH = 0;
        gBlurBackgroundTex = LoadBlurredBackgroundTexture(backgroundPath.c_str(), blurW, blurH);
    }

    const wchar_t* infoImageNames[] = {L"1.png", L"2.png", L"3.png", L"4.png"};
    for (int i = 0; i < 4; ++i) {
        gInfoImages[i].tex = LoadLocalTextureSized(
            FindInfoImagePath(infoImageNames[i]).c_str(),
            gInfoImages[i].width,
            gInfoImages[i].height
        );
    }

    gIconProfileTex = LoadTextureFromMemory(kIconProfilePng, kIconProfilePng_len);
    gIconBuildsTex = LoadTextureFromMemory(kIconBuildsPng, kIconBuildsPng_len);
    gIconSettingsTex = LoadTextureFromMemory(kIconSettingsPng, kIconSettingsPng_len);
    gIconInfoTex = LoadTextureFromMemory(kIconInfoPng, kIconInfoPng_len);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ImFontConfig fontConfig;
    fontConfig.OversampleH = 2;
    fontConfig.OversampleV = 2;

    ImFont* segoeFont = io.Fonts->AddFontFromFileTTF(
        "C:/Windows/Fonts/segoeui.ttf",
        16.0f,
        &fontConfig,
        io.Fonts->GetGlyphRangesCyrillic()
    );

    if (segoeFont) {
        io.FontDefault = segoeFont;
    }

    if (!ImGui_ImplGlfw_InitForOpenGL(window, true) ||
        !ImGui_ImplOpenGL3_Init("#version 330")) {
        ImGui::DestroyContext();

        if (backgroundTexture) {
            glDeleteTextures(1, &backgroundTexture);
        }
        if (gBlurBackgroundTex) {
            glDeleteTextures(1, &gBlurBackgroundTex);
        }

        for (int i = 0; i < 4; ++i) {
            if (gInfoImages[i].tex) glDeleteTextures(1, &gInfoImages[i].tex);
        }

        if (gdiplusToken) {
            Gdiplus::GdiplusShutdown(gdiplusToken);
        }

        glfwDestroyWindow(window);
        glfwTerminate();
        return -1;
    }

    ApplyAppStyle();

    bool startupFinished = false;
    const double splashStartedAt = glfwGetTime();

    bool loggedIn = false;
    bool loginError = false;
    double loginErrorAt = -1.0;
    bool rememberMe = false;
    ElyAuthResult elyResult;
    std::mutex elyMutex;
    std::atomic<bool> elyRunning = false;

    char loginName[64] = "";
    {
        std::string savedName = LoadLoginName();
        if (!savedName.empty() && savedName.size() < sizeof(loginName)) {
            memcpy(loginName, savedName.c_str(), savedName.size());
            loginName[savedName.size()] = 0;
        }
    }
    int launcherPage = 0;
    InstallState installState;
    {
        const std::filesystem::path gameDir =
            std::filesystem::path(FindExecutableDirectory()) / ".." / "minecraft";
        if (IsGameInstalledOnDisk(gameDir)) {
            installState.phase = InstallPhase::Done;
        }
    }
    std::string launchError;
    double launchErrorAt = -1.0;
    bool useMods = true;
    bool autoJoinServer = true;
    char serverAddress[64] = "lemon.mineserver.xyz";
    int maxMemoryMB = 4096;

    InitDiscordRPC(loginName);

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        if (gDiscordReady && gDiscordRpcEnabled) Discord_RunCallbacks();

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        if (!startupFinished && glfwGetTime() - splashStartedAt >= 2.8) {
            startupFinished = true;
        }

        if (!startupFinished) {
            DrawLoadingScreen(hwnd, backgroundTexture);
        } else if (!loggedIn) {
            DrawLoginScreen(
                hwnd,
                backgroundTexture,
                loginName,
                sizeof(loginName),
                loggedIn,
                loginError,
                loginErrorAt,
                rememberMe,
                elyResult,
                elyMutex,
                elyRunning
            );
        } else {
            DrawLauncherScreen(
                hwnd,
                backgroundTexture,
                loginName,
                launcherPage,
                installState,
                launchError,
                launchErrorAt,
                useMods,
                autoJoinServer,
                serverAddress,
                sizeof(serverAddress),
                maxMemoryMB,
                gDiscordRpcEnabled,
                elyResult,
                elyMutex
            );
        }

        ImGui::Render();

        int displayWidth = 0;
        int displayHeight = 0;
        glfwGetFramebufferSize(window, &displayWidth, &displayHeight);

        glViewport(0, 0, displayWidth, displayHeight);
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    if (backgroundTexture) {
        glDeleteTextures(1, &backgroundTexture);
    }
    if (gBlurBackgroundTex) {
        glDeleteTextures(1, &gBlurBackgroundTex);
    }

    if (gdiplusToken) {
        Gdiplus::GdiplusShutdown(gdiplusToken);
    }

    if (gDiscordReady) {
        Discord_ClearPresence();
        Discord_Shutdown();
        gDiscordReady = false;
    }

    glfwDestroyWindow(window);
    glfwTerminate();

    return 0;
}