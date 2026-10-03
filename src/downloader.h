#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

struct JsonValue {
    enum Type { Null, Bool, Number, String, Array, Object };

    Type type = Null;
    bool boolVal = false;
    double numVal = 0.0;
    std::string strVal;
    std::vector<JsonValue> arrVal;
    std::vector<std::pair<std::string, JsonValue>> objVal;

    JsonValue() = default;
    JsonValue(std::nullptr_t) : type(Null) {}
    JsonValue(bool v) : type(Bool), boolVal(v) {}
    JsonValue(int v) : type(Number), numVal(static_cast<double>(v)) {}
    JsonValue(long v) : type(Number), numVal(static_cast<double>(v)) {}
    JsonValue(long long v) : type(Number), numVal(static_cast<double>(v)) {}
    JsonValue(float v) : type(Number), numVal(static_cast<double>(v)) {}
    JsonValue(double v) : type(Number), numVal(v) {}
    JsonValue(const char* v) : type(String), strVal(v) {}
    JsonValue(const std::string& v) : type(String), strVal(v) {}
    JsonValue(std::string&& v) : type(String), strVal(std::move(v)) {}
    JsonValue(std::vector<JsonValue>&& v) : type(Array), arrVal(std::move(v)) {}

    bool contains(const std::string& key) const {
        if (type != Object) return false;
        for (const auto& [k, v] : objVal) {
            if (k == key) return true;
        }
        return false;
    }

    const JsonValue& operator[](const std::string& key) const {
        static const JsonValue nullVal;
        if (type != Object) return nullVal;
        for (const auto& [k, v] : objVal) {
            if (k == key) return v;
        }
        return nullVal;
    }

    JsonValue& operator[](const std::string& key) {
        if (type != Object) type = Object;
        for (auto& [k, v] : objVal) {
            if (k == key) return v;
        }
        objVal.emplace_back(key, JsonValue());
        return objVal.back().second;
    }

    const JsonValue& operator[](size_t index) const {
        static const JsonValue nullVal;
        if (type != Array || index >= arrVal.size()) return nullVal;
        return arrVal[index];
    }

    std::string string() const { return strVal; }
    double number() const { return numVal; }
    bool boolean() const { return boolVal; }

    size_t size() const {
        if (type == Array) return arrVal.size();
        if (type == Object) return objVal.size();
        return 0;
    }

    std::string stringOr(const std::string& fallback) const {
        return type == String ? strVal : fallback;
    }

    double numberOr(double fallback) const {
        return type == Number ? numVal : fallback;
    }
};

class JsonParser {
public:
    explicit JsonParser(const char* input) : p(input) {}

    JsonValue parse() {
        skipWhitespace();
        return parseValue();
    }

private:
    const char* p;

    void skipWhitespace() {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') ++p;
    }

    char peek() { return *p; }
    char advance() { return *p++; }

    JsonValue parseValue() {
        skipWhitespace();
        char c = peek();
        if (c == '{') return parseObject();
        if (c == '[') return parseArray();
        if (c == '"') return parseString();
        if (c == 't' || c == 'f') return parseBool();
        if (c == 'n') return parseNull();
        return parseNumber();
    }

    JsonValue parseObject() {
        JsonValue val;
        val.type = JsonValue::Object;
        advance(); // '{'
        skipWhitespace();

        if (peek() == '}') {
            advance();
            return val;
        }

        while (true) {
            skipWhitespace();
            auto key = parseString();
            skipWhitespace();
            advance(); // ':'
            skipWhitespace();
            auto value = parseValue();
            val.objVal.emplace_back(std::move(key.strVal), std::move(value));
            skipWhitespace();
            if (peek() == ',') { advance(); continue; }
            break;
        }

        skipWhitespace();
        advance(); // '}'
        return val;
    }

    JsonValue parseArray() {
        JsonValue val;
        val.type = JsonValue::Array;
        advance(); // '['
        skipWhitespace();

        if (peek() == ']') {
            advance();
            return val;
        }

        while (true) {
            skipWhitespace();
            val.arrVal.push_back(parseValue());
            skipWhitespace();
            if (peek() == ',') { advance(); continue; }
            break;
        }

        skipWhitespace();
        advance(); // ']'
        return val;
    }

    JsonValue parseString() {
        JsonValue val;
        val.type = JsonValue::String;
        advance(); // '"'
        std::string result;

        while (*p && *p != '"') {
            if (*p == '\\') {
                ++p;
                switch (*p) {
                    case '"': result += '"'; break;
                    case '\\': result += '\\'; break;
                    case '/': result += '/'; break;
                    case 'b': result += '\b'; break;
                    case 'f': result += '\f'; break;
                    case 'n': result += '\n'; break;
                    case 'r': result += '\r'; break;
                    case 't': result += '\t'; break;
                    case 'u': {
                        ++p;
                        unsigned int cp = 0;
                        for (int i = 0; i < 4 && *p; ++i, ++p) {
                            cp <<= 4;
                            char hex = *p;
                            if (hex >= '0' && hex <= '9') cp += hex - '0';
                            else if (hex >= 'a' && hex <= 'f') cp += hex - 'a' + 10;
                            else if (hex >= 'A' && hex <= 'F') cp += hex - 'A' + 10;
                        }
                        --p;
                        if (cp < 0x80) result += static_cast<char>(cp);
                        else if (cp < 0x800) {
                            result += static_cast<char>(0xC0 | (cp >> 6));
                            result += static_cast<char>(0x80 | (cp & 0x3F));
                        } else {
                            result += static_cast<char>(0xE0 | (cp >> 12));
                            result += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                            result += static_cast<char>(0x80 | (cp & 0x3F));
                        }
                        break;
                    }
                    default: result += *p; break;
                }
            } else {
                result += *p;
            }
            ++p;
        }

        if (*p == '"') ++p;
        val.strVal = std::move(result);
        return val;
    }

    JsonValue parseNumber() {
        JsonValue val;
        val.type = JsonValue::Number;
        const char* start = p;

        if (*p == '-') ++p;
        while (*p >= '0' && *p <= '9') ++p;
        if (*p == '.') { ++p; while (*p >= '0' && *p <= '9') ++p; }
        if (*p == 'e' || *p == 'E') { ++p; if (*p == '+' || *p == '-') ++p; while (*p >= '0' && *p <= '9') ++p; }

        auto result = std::from_chars(start, p, val.numVal);
        if (result.ec != std::errc{}) val.numVal = 0.0;
        return val;
    }

    JsonValue parseBool() {
        JsonValue val;
        val.type = JsonValue::Bool;
        if (*p == 't') { val.boolVal = true; p += 4; }
        else { val.boolVal = false; p += 5; }
        return val;
    }

    JsonValue parseNull() {
        JsonValue val;
        val.type = JsonValue::Null;
        p += 4;
        return val;
    }
};

inline JsonValue parseJson(const std::string& json) {
    return JsonParser(json.c_str()).parse();
}

inline std::wstring utf8ToWide(const std::string& str) {
    if (str.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), static_cast<int>(str.size()), nullptr, 0);
    std::wstring result(len, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), static_cast<int>(str.size()), result.data(), len);
    return result;
}

inline std::string wideToUtf8(const std::wstring& str) {
    if (str.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, str.c_str(), static_cast<int>(str.size()), nullptr, 0, nullptr, nullptr);
    std::string result(len, 0);
    WideCharToMultiByte(CP_UTF8, 0, str.c_str(), static_cast<int>(str.size()), result.data(), len, nullptr, nullptr);
    return result;
}

inline bool parseUrl(const std::string& url, std::wstring& host, std::wstring& path, bool& useHttps) {
    useHttps = false;
    host.clear();
    path.clear();

    std::string work = url;
    if (work.substr(0, 8) == "https://") {
        useHttps = true;
        work = work.substr(8);
    } else if (work.substr(0, 7) == "http://") {
        work = work.substr(7);
    } else {
        return false;
    }

    size_t slashPos = work.find('/');
    if (slashPos == std::string::npos) {
        host = utf8ToWide(work);
        path = L"/";
    } else {
        host = utf8ToWide(work.substr(0, slashPos));
        path = utf8ToWide(work.substr(slashPos));
    }
    return !host.empty();
}

struct DownloadProgress {
    std::function<void(int current, int total)> onProgress;
    std::function<bool()> shouldCancel;
};

inline bool httpGetToFile(
    const std::string& url,
    const std::filesystem::path& destPath,
    const DownloadProgress& progress = {}
) {
    std::wstring host, path;
    bool useHttps;
    if (!parseUrl(url, host, path, useHttps)) return false;

    HINTERNET hSession = WinHttpOpen(
        L"AmethystLauncher/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0
    );
    if (!hSession) return false;

    {
        DWORD resolveTimeout = 5000;
        DWORD connectTimeout = 10000;
        DWORD sendTimeout = 30000;
        DWORD receiveTimeout = 30000;
        WinHttpSetTimeouts(hSession, resolveTimeout, connectTimeout, sendTimeout, receiveTimeout);
    }

    DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
    WinHttpSetOption(hSession, WINHTTP_OPTION_REDIRECT_POLICY, &redirectPolicy, sizeof(redirectPolicy));

    HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(),
        useHttps ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return false; }

    HINTERNET hRequest = WinHttpOpenRequest(
        hConnect, L"GET", path.c_str(),
        nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        useHttps ? WINHTTP_FLAG_SECURE : 0
    );
    if (!hRequest) { WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return false; }

    DWORD secFlags = SECURITY_FLAG_IGNORE_UNKNOWN_CA |
                     SECURITY_FLAG_IGNORE_CERT_DATE_INVALID |
                     SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
                     SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
    WinHttpSetOption(hRequest, WINHTTP_OPTION_SECURITY_FLAGS, &secFlags, sizeof(secFlags));

    if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession);
        return false;
    }

    if (!WinHttpReceiveResponse(hRequest, nullptr)) {
        WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession);
        return false;
    }

    DWORD statusCode = 0;
    DWORD statusSize = sizeof(statusCode);
    WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX);

    if (statusCode != 200 && statusCode != 301 && statusCode != 302) {
        WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession);
        return false;
    }

    std::ofstream outFile(destPath, std::ios::binary);
    if (!outFile) {
        WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession);
        return false;
    }

    DWORD totalSize = 0;
    DWORD totalSizeSize = sizeof(totalSize);
    WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &totalSize, &totalSizeSize, WINHTTP_NO_HEADER_INDEX);

    char buffer[65536];
    DWORD bytesRead = 0;
    DWORD totalRead = 0;

    while (true) {
        if (progress.shouldCancel && progress.shouldCancel()) {
            WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession);
            outFile.close();
            std::filesystem::remove(destPath);
            return false;
        }

        if (!WinHttpReadData(hRequest, buffer, sizeof(buffer), &bytesRead)) break;
        if (bytesRead == 0) break;

        outFile.write(buffer, bytesRead);
        totalRead += bytesRead;

        if (progress.onProgress && totalSize > 0) {
            progress.onProgress(static_cast<int>(totalRead), static_cast<int>(totalSize));
        }
    }

    outFile.close();
    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    return totalRead > 0;
}

inline std::string httpGetString(const std::string& url) {
    std::wstring host, path;
    bool useHttps;
    if (!parseUrl(url, host, path, useHttps)) return {};

    HINTERNET hSession = WinHttpOpen(
        L"AmethystLauncher/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0
    );
    if (!hSession) return {};

    {
        DWORD resolveTimeout = 5000;
        DWORD connectTimeout = 10000;
        DWORD sendTimeout = 30000;
        DWORD receiveTimeout = 30000;
        WinHttpSetTimeouts(hSession, resolveTimeout, connectTimeout, sendTimeout, receiveTimeout);
    }

    DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
    WinHttpSetOption(hSession, WINHTTP_OPTION_REDIRECT_POLICY, &redirectPolicy, sizeof(redirectPolicy));

    HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(),
        useHttps ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return {}; }

    HINTERNET hRequest = WinHttpOpenRequest(
        hConnect, L"GET", path.c_str(),
        nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        useHttps ? WINHTTP_FLAG_SECURE : 0
    );
    if (!hRequest) { WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return {}; }

    DWORD secFlags = SECURITY_FLAG_IGNORE_UNKNOWN_CA |
                     SECURITY_FLAG_IGNORE_CERT_DATE_INVALID |
                     SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
                     SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
    WinHttpSetOption(hRequest, WINHTTP_OPTION_SECURITY_FLAGS, &secFlags, sizeof(secFlags));

    if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession);
        return {};
    }

    if (!WinHttpReceiveResponse(hRequest, nullptr)) {
        WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession);
        return {};
    }

    std::string result;
    char buffer[65536];
    DWORD bytesRead = 0;

    while (WinHttpReadData(hRequest, buffer, sizeof(buffer), &bytesRead) && bytesRead > 0) {
        result.append(buffer, bytesRead);
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    return result;
}

inline bool ensureParentDir(const std::filesystem::path& filePath) {
    auto parent = filePath.parent_path();
    if (!parent.empty() && !std::filesystem::exists(parent)) {
        std::error_code ec;
        std::filesystem::create_directories(parent, ec);
        return !ec;
    }
    return true;
}

inline std::string sha1File(const std::filesystem::path& filePath) {
    HCRYPTPROV hProv = 0;
    HCRYPTHASH hHash = 0;
    std::string result;

    if (!CryptAcquireContext(&hProv, nullptr, nullptr, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT)) return result;
    if (!CryptCreateHash(hProv, CALG_SHA1, 0, 0, &hHash)) {
        CryptReleaseContext(hProv, 0);
        return result;
    }

    std::ifstream file(filePath, std::ios::binary);
    if (!file) {
        CryptDestroyHash(hHash);
        CryptReleaseContext(hProv, 0);
        return result;
    }

    char buffer[65536];
    while (file.read(buffer, sizeof(buffer))) {
        CryptHashData(hHash, reinterpret_cast<BYTE*>(buffer), static_cast<DWORD>(file.gcount()), 0);
    }
    CryptHashData(hHash, reinterpret_cast<BYTE*>(buffer), static_cast<DWORD>(file.gcount()), 0);

    BYTE hash[20];
    DWORD hashLen = sizeof(hash);
    if (CryptGetHashParam(hHash, HP_HASHVAL, hash, &hashLen, 0)) {
        result.resize(40);
        for (int i = 0; i < 20; ++i) {
            sprintf(&result[i * 2], "%02x", hash[i]);
        }
    }

    CryptDestroyHash(hHash);
    CryptReleaseContext(hProv, 0);
    return result;
}

inline bool downloadFile(
    const std::string& url,
    const std::filesystem::path& destPath,
    const std::string& expectedSha1 = "",
    const DownloadProgress& progress = {}
) {
    if (std::filesystem::exists(destPath)) {
        if (expectedSha1.empty()) return true;
        if (sha1File(destPath) == expectedSha1) return true;
    }

    if (!ensureParentDir(destPath)) return false;

    std::filesystem::path tmpPath = destPath;
    tmpPath += L".tmp";

    if (!httpGetToFile(url, tmpPath, progress)) {
        std::error_code ec;
        std::filesystem::remove(tmpPath, ec);
        return false;
    }

    if (!expectedSha1.empty()) {
        std::string actual = sha1File(tmpPath);
        if (actual != expectedSha1) {
            std::error_code ec;
            std::filesystem::remove(tmpPath, ec);
            return false;
        }
    }

    std::error_code ec;
    std::filesystem::rename(tmpPath, destPath, ec);
    if (ec) {
        std::filesystem::remove(tmpPath, ec);
        return false;
    }

    return true;
}
