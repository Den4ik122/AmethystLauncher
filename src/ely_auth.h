#pragma once

#include "downloader.h"

#include <atomic>
#include <cstdlib>
#include <mutex>
#include <random>
#include <thread>
#include <ws2tcpip.h>

struct ElyAuthResult {
    bool ready = false;
    bool success = false;
    std::string error;
    std::string username;
    std::string uuid;          // UUID аккаунта Ely.by (совпадает с игровым)
    std::string accessToken;   // OAuth2 access_token со scope minecraft_server_session
    std::string refreshToken;  // refresh_token (если запрошен offline_access)
};

inline constexpr char kElyClientId[] = "amethyst-launcher1";
inline constexpr char kElyClientSecret[] = "Y3VZ9rph95sY5CcWSBUvcQULKDOaamWyw04NZMkNDnvNiMj-8A9-6um1z5xqRlmc";

inline constexpr char kElyAccountApi[]     = "https://account.ely.by/api/account/v1/info";
inline constexpr char kElyOauthToken[]     = "https://account.ely.by/api/oauth2/v1/token";
inline constexpr char kElyOauthAuthorize[] = "https://account.ely.by/oauth2/v1";
inline constexpr char kElyAuthlibMeta[]    = "https://authserver.ely.by/api/authlib-injector";

inline std::string elyUrlEncode(const std::string& value) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string result;
    for (unsigned char c : value) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') result += static_cast<char>(c);
        else { result += '%'; result += hex[c >> 4]; result += hex[c & 15]; }
    }
    return result;
}

inline std::string elyQueryValue(const std::string& query, const std::string& key) {
    const std::string prefix = key + "=";
    size_t start = 0;
    while (start < query.size()) {
        size_t end = query.find('&', start);
        if (end == std::string::npos) end = query.size();
        if (query.compare(start, prefix.size(), prefix) == 0)
            return query.substr(start + prefix.size(), end - start - prefix.size());
        start = end + 1;
    }
    return {};
}

inline std::string elyRandomState() {
    static constexpr char chars[] = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<size_t> dist(0, sizeof(chars) - 2);
    std::string result;
    for (int i = 0; i < 32; ++i) result += chars[dist(gen)];
    return result;
}

// POST application/x-www-form-urlencoded
inline bool elyPostForm(const std::string& url, const std::string& body, std::string& response) {
    std::wstring host, path;
    bool https = false;
    if (!parseUrl(url, host, path, https) || !https) return false;

    HINTERNET session = WinHttpOpen(L"AmethystLauncher/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return false;
    WinHttpSetTimeouts(session, 5000, 10000, 10000, 30000);

    DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
    WinHttpSetOption(session, WINHTTP_OPTION_REDIRECT_POLICY, &redirectPolicy, sizeof(redirectPolicy));

    HINTERNET connection = WinHttpConnect(session, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET request = connection ? WinHttpOpenRequest(connection, L"POST", path.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;

    const wchar_t* headers = L"Content-Type: application/x-www-form-urlencoded\r\n";

    bool ok = request && WinHttpSendRequest(request, headers, static_cast<DWORD>(-1L),
        const_cast<char*>(body.data()), static_cast<DWORD>(body.size()),
        static_cast<DWORD>(body.size()), 0) &&
        WinHttpReceiveResponse(request, nullptr);

    DWORD status = 0, statusSize = sizeof(status);
    if (ok) WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX);

    char buffer[4096]; DWORD read = 0;
    while (ok && WinHttpReadData(request, buffer, sizeof(buffer), &read) && read)
        response.append(buffer, read);

    if (request) WinHttpCloseHandle(request);
    if (connection) WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);

    return ok && status >= 200 && status < 300;
}

// GET с Authorization: Bearer
inline bool elyGetProfile(const std::string& token, std::string& response) {
    std::wstring host, path;
    bool https = false;
    if (!parseUrl(kElyAccountApi, host, path, https)) return false;
    HINTERNET session = WinHttpOpen(L"AmethystLauncher/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return false;
    WinHttpSetTimeouts(session, 5000, 10000, 10000, 30000);
    HINTERNET connection = WinHttpConnect(session, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET request = connection ? WinHttpOpenRequest(connection, L"GET", path.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;
    std::wstring auth = L"Authorization: Bearer " + utf8ToWide(token) + L"\r\n";
    bool ok = request && WinHttpAddRequestHeaders(request, auth.c_str(), static_cast<DWORD>(-1L), WINHTTP_ADDREQ_FLAG_ADD) &&
        WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(request, nullptr);
    DWORD status = 0, statusSize = sizeof(status);
    if (ok) WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX);
    char buffer[4096]; DWORD read = 0;
    while (ok && WinHttpReadData(request, buffer, sizeof(buffer), &read) && read) response.append(buffer, read);
    if (request) WinHttpCloseHandle(request);
    if (connection) WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    return ok && status >= 200 && status < 300;
}

inline void runElyAuth(ElyAuthResult& result, std::mutex& resultMutex, std::atomic<bool>& running) {
    auto finish = [&](const std::string& error) {
        std::lock_guard lock(resultMutex);
        result.error = error;
        result.ready = true;
        running = false;
    };

    const char* clientId = kElyClientId;
    const char* clientSecret = kElyClientSecret;
    if (std::strcmp(clientId, "PASTE_CLIENT_ID_HERE") == 0) {
        clientId = std::getenv("AMETHYST_ELY_CLIENT_ID");
    }
    if (std::strcmp(clientSecret, "PASTE_CLIENT_SECRET_HERE") == 0) {
        clientSecret = std::getenv("AMETHYST_ELY_CLIENT_SECRET");
    }
    if (!clientId || !clientSecret || !clientId[0] || !clientSecret[0]) {
        finish("Задайте AMETHYST_ELY_CLIENT_ID и AMETHYST_ELY_CLIENT_SECRET");
        return;
    }

    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) { finish("Не удалось запустить сетевой модуль"); return; }

    constexpr unsigned short port = 17843;
    const std::string redirectUri = "http://127.0.0.1:17843/ely/callback";
    const std::string state = elyRandomState();

    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);

    if (listener == INVALID_SOCKET ||
        bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR ||
        listen(listener, 1) == SOCKET_ERROR) {
        if (listener != INVALID_SOCKET) closesocket(listener);
        WSACleanup();
        finish("Порт авторизации занят");
        return;
    }

    // scope: account_info + minecraft_server_session + offline_access (для refresh_token)
    const std::string authUrl = std::string(kElyOauthAuthorize) +
        "?client_id=" + elyUrlEncode(clientId) +
        "&redirect_uri=" + elyUrlEncode(redirectUri) +
        "&response_type=code" +
        "&scope=account_info%20minecraft_server_session%20offline_access" +
        "&state=" + state;

    ShellExecuteA(nullptr, "open", authUrl.c_str(), nullptr, nullptr, SW_SHOWNORMAL);

    fd_set readSet; FD_ZERO(&readSet); FD_SET(listener, &readSet);
    timeval timeout{180, 0};
    SOCKET client = select(0, &readSet, nullptr, nullptr, &timeout) > 0
        ? accept(listener, nullptr, nullptr)
        : INVALID_SOCKET;

    std::string request;
    if (client != INVALID_SOCKET) {
        char buffer[4096]; int received = recv(client, buffer, sizeof(buffer) - 1, 0);
        if (received > 0) request.assign(buffer, received);
        const char reply[] = "HTTP/1.1 200 OK\r\nContent-Type: text/plain; charset=utf-8\r\nConnection: close\r\n\r\n"
                             "Авторизация завершена. Это окно можно закрыть.";
        send(client, reply, static_cast<int>(sizeof(reply) - 1), 0);
        closesocket(client);
    }
    closesocket(listener); WSACleanup();

    size_t queryStart = request.find('?');
    size_t queryEnd = request.find(' ', queryStart);
    std::string query = queryStart == std::string::npos
        ? ""
        : request.substr(queryStart + 1, queryEnd - queryStart - 1);

    if (elyQueryValue(query, "state") != state) { finish("Неверный state OAuth"); return; }
    std::string code = elyQueryValue(query, "code");
    if (code.empty()) { finish("Авторизация отменена"); return; }

    std::string tokenJson;
    const std::string body =
        "client_id=" + elyUrlEncode(clientId) +
        "&client_secret=" + elyUrlEncode(clientSecret) +
        "&redirect_uri=" + elyUrlEncode(redirectUri) +
        "&grant_type=authorization_code&code=" + elyUrlEncode(code);

    if (!elyPostForm(kElyOauthToken, body, tokenJson)) {
        finish("Не удалось получить токен Ely.by");
        return;
    }

    JsonValue tokenData = parseJson(tokenJson);
    std::string oauthAccessToken  = tokenData["access_token"].string();
    std::string oauthRefreshToken = tokenData.contains("refresh_token") ? tokenData["refresh_token"].string() : "";
    if (oauthAccessToken.empty()) { finish("Ely.by не вернул access_token"); return; }

    // Профиль: username + uuid
    std::string profileJson;
    std::string accountUsername;
    std::string profileUuid;
    if (elyGetProfile(oauthAccessToken, profileJson)) {
        JsonValue p = parseJson(profileJson);
        accountUsername = p["username"].string();
        profileUuid     = p["uuid"].string();
    }

    if (accountUsername.empty() || profileUuid.empty()) {
        finish("Не удалось получить профиль Ely.by");
        return;
    }

    // По документации Ely.by: OAuth2 access_token со scope minecraft_server_session
    // МОЖНО использовать напрямую как игровую сессию Minecraft. Никакого
    // /auth/authenticate не требуется — authlib-injector сам всё сделает.
    std::lock_guard lock(resultMutex);
    result.username     = accountUsername;
    result.uuid         = profileUuid;
    result.accessToken  = oauthAccessToken;
    result.refreshToken = oauthRefreshToken;
    result.success = !result.username.empty() && !result.uuid.empty() && !result.accessToken.empty();
    result.error = result.success ? "" : "Профиль Ely.by поврежден";
    result.ready = true;
    running = false;
}