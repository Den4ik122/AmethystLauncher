#pragma once

#include "downloader.h"
#include <windows.h>

#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

inline std::wstring findJavaExe() {
    auto fileExists = [](const std::wstring& path) -> bool {
        DWORD attr = GetFileAttributesW(path.c_str());
        return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
    };

    auto getEnv = [](const wchar_t* name) -> std::wstring {
        DWORD len = GetEnvironmentVariableW(name, nullptr, 0);
        if (len == 0) return {};
        std::wstring buf(len, 0);
        GetEnvironmentVariableW(name, buf.data(), len);
        if (!buf.empty() && buf.back() == 0) buf.pop_back();
        return buf;
    };

    std::wstring javaHome = getEnv(L"JAVA_HOME");
    if (!javaHome.empty()) {
        std::wstring candidate = javaHome + L"\\bin\\javaw.exe";
        if (fileExists(candidate)) return candidate;
        candidate = javaHome + L"\\bin\\java.exe";
        if (fileExists(candidate)) return candidate;
    }

    HKEY hKey;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
            L"SOFTWARE\\JavaSoft\\Java Runtime Environment",
            0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        DWORD index = 0;
        wchar_t subkeyName[256];
        DWORD subkeySize = 256;
        std::wstring bestVersion;
        std::wstring bestJavaHome;

        while (RegEnumKeyExW(hKey, index, subkeyName, &subkeySize,
                             nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS) {
            HKEY hSub;
            if (RegOpenKeyExW(hKey, subkeyName, 0, KEY_READ, &hSub) == ERROR_SUCCESS) {
                DWORD type, size;
                wchar_t homeBuf[MAX_PATH]{};
                size = sizeof(homeBuf);
                if (RegQueryValueExW(hSub, L"JavaHome", nullptr, &type,
                                     reinterpret_cast<BYTE*>(homeBuf), &size) == ERROR_SUCCESS) {
                    std::wstring ver(subkeyName);
                    if (bestVersion.empty() || ver > bestVersion) {
                        bestVersion = ver;
                        bestJavaHome = homeBuf;
                    }
                }
                RegCloseKey(hSub);
            }
            subkeySize = 256;
            ++index;
        }
        RegCloseKey(hKey);

        if (!bestJavaHome.empty()) {
            std::wstring candidate = bestJavaHome + L"\\bin\\javaw.exe";
            if (fileExists(candidate)) return candidate;
            candidate = bestJavaHome + L"\\bin\\java.exe";
            if (fileExists(candidate)) return candidate;
        }
    }

    DWORD pathLen = GetEnvironmentVariableW(L"PATH", nullptr, 0);
    if (pathLen > 0) {
        std::wstring pathEnv(pathLen, 0);
        GetEnvironmentVariableW(L"PATH", pathEnv.data(), pathLen);
        if (!pathEnv.empty() && pathEnv.back() == 0) pathEnv.pop_back();

        std::wistringstream ss(pathEnv);
        std::wstring segment;
        while (std::getline(ss, segment, L';')) {
            if (segment.empty()) continue;
            std::wstring candidate = segment + L"\\javaw.exe";
            if (fileExists(candidate)) return candidate;
            candidate = segment + L"\\java.exe";
            if (fileExists(candidate)) return candidate;
        }
    }

    return {};
}

inline std::string resolvePlaceholder(const std::string& name,
                                      const std::function<std::string(const std::string&)>& resolver) {
    return resolver(name);
}

struct LaunchConfig {
    std::filesystem::path gameDir;
    std::string mcVersion;
    std::string username;
    std::string uuid;
    std::string accessToken;   // OAuth2 access_token (scope minecraft_server_session)
    bool useFabric = true;
    std::filesystem::path modsDir;
    std::string serverAddress;
    std::string serverName = "Lemon Server";
    int maxMemoryMB = 2048;
    int minWidth = 854;
    int minHeight = 480;
};

struct LaunchResult {
    bool success = false;
    std::string error;
    DWORD processId = 0;
};

inline std::string stripQuotes(const std::string& s) {
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"') {
        return s.substr(1, s.size() - 2);
    }
    return s;
}

inline size_t syncMods(const std::filesystem::path& sourceDir, const std::filesystem::path& gameDir) {
    std::filesystem::path modsSourceDir = sourceDir;
    std::filesystem::path modsDestDir = gameDir / "mods";

    std::error_code ec;
    std::filesystem::create_directories(modsSourceDir, ec);
    std::filesystem::create_directories(modsDestDir, ec);

    auto isJar = [](const std::filesystem::path& p) {
        std::filesystem::path ext = p.extension();
        return ext == ".jar" || ext == ".JAR";
    };

    for (const auto& entry : std::filesystem::directory_iterator(modsDestDir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file()) continue;
        if (!isJar(entry.path())) continue;
        std::filesystem::path srcCandidate = modsSourceDir / entry.path().filename();
        if (!std::filesystem::exists(srcCandidate)) {
            std::filesystem::remove(entry.path(), ec);
            ec.clear();
        }
    }

    size_t copied = 0;
    if (!std::filesystem::exists(modsSourceDir)) return copied;
    for (const auto& entry : std::filesystem::directory_iterator(modsSourceDir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file()) continue;
        if (!isJar(entry.path())) continue;
        try {
            std::filesystem::copy_file(
                entry.path(),
                modsDestDir / entry.path().filename(),
                std::filesystem::copy_options::overwrite_existing);
            ++copied;
        } catch (...) {}
    }
    return copied;
}

inline std::vector<unsigned char> buildServersNbt(const std::string& name, const std::string& ip) {
    std::vector<unsigned char> out;
    auto writeU8 = [&](unsigned char v) { out.push_back(v); };
    auto writeString = [&](const std::string& s) {
        writeU8(static_cast<unsigned char>((s.size() >> 8) & 0xFF));
        writeU8(static_cast<unsigned char>(s.size() & 0xFF));
        out.insert(out.end(), s.begin(), s.end());
    };
    auto writeS32 = [&](int32_t v) {
        uint32_t uv = static_cast<uint32_t>(v);
        writeU8(static_cast<unsigned char>((uv >> 24) & 0xFF));
        writeU8(static_cast<unsigned char>((uv >> 16) & 0xFF));
        writeU8(static_cast<unsigned char>((uv >> 8) & 0xFF));
        writeU8(static_cast<unsigned char>(uv & 0xFF));
    };

    writeU8(0x0A);
    writeString("");
    writeU8(0x09);
    writeString("servers");
    writeU8(0x0A);
    writeS32(1);
    writeU8(0x08);
    writeString("name");
    writeString(name);
    writeU8(0x08);
    writeString("ip");
    writeString(ip);
    writeU8(0x03);
    writeString("acceptTextures");
    writeS32(1);
    writeU8(0x00);
    writeU8(0x00);
    return out;
}

inline uint32_t crc32(const std::vector<unsigned char>& data) {
    static uint32_t table[256];
    static bool tableReady = false;
    if (!tableReady) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1) ? (0xEDB88320 ^ (c >> 1)) : (c >> 1);
            }
            table[i] = c;
        }
        tableReady = true;
    }
    uint32_t crc = 0xFFFFFFFF;
    for (unsigned char b : data) {
        crc = table[(crc ^ b) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFF;
}

inline std::vector<unsigned char> gzipStored(const std::vector<unsigned char>& data) {
    std::vector<unsigned char> out;
    out.push_back(0x1F); out.push_back(0x8B);
    out.push_back(0x08);
    out.push_back(0x00);
    out.push_back(0x00); out.push_back(0x00);
    out.push_back(0x00); out.push_back(0x00);
    out.push_back(0x00);
    out.push_back(0x03);

    size_t offset = 0;
    while (offset < data.size()) {
        size_t chunk = std::min<size_t>(65535, data.size() - offset);
        bool finalBlock = (offset + chunk == data.size());
        out.push_back(finalBlock ? 0x01 : 0x00);
        uint16_t len = static_cast<uint16_t>(chunk);
        uint16_t nlen = static_cast<uint16_t>(~len);
        out.push_back(static_cast<unsigned char>(len & 0xFF));
        out.push_back(static_cast<unsigned char>((len >> 8) & 0xFF));
        out.push_back(static_cast<unsigned char>(nlen & 0xFF));
        out.push_back(static_cast<unsigned char>((nlen >> 8) & 0xFF));
        out.insert(out.end(), data.begin() + offset, data.begin() + offset + chunk);
        offset += chunk;
    }

    auto appendLE = [&](uint32_t v) {
        out.push_back(static_cast<unsigned char>(v & 0xFF));
        out.push_back(static_cast<unsigned char>((v >> 8) & 0xFF));
        out.push_back(static_cast<unsigned char>((v >> 16) & 0xFF));
        out.push_back(static_cast<unsigned char>((v >> 24) & 0xFF));
    };
    appendLE(crc32(data));
    appendLE(static_cast<uint32_t>(data.size()));
    return out;
}

inline bool writeServerList(
    const std::filesystem::path& gameDir,
    const std::string& serverName,
    const std::string& ip
) {
    std::vector<unsigned char> nbt = buildServersNbt(serverName, ip);
    std::vector<unsigned char> gz = gzipStored(nbt);

    std::error_code ec;
    std::filesystem::create_directories(gameDir, ec);

    std::filesystem::path dest = gameDir / "servers.dat";
    std::ofstream file(dest, std::ios::binary | std::ios::trunc);
    if (!file) return false;
    file.write(reinterpret_cast<const char*>(gz.data()), static_cast<std::streamsize>(gz.size()));
    file.close();
    return true;
}

inline LaunchResult launchGame(const LaunchConfig& config) {
    std::filesystem::path gameDir = config.gameDir;
    std::filesystem::path librariesDir = gameDir / "libraries";
    std::filesystem::path versionsDir = gameDir / "versions";
    std::filesystem::path assetsDir = gameDir / "assets";

    size_t syncedMods = 0;
    if (config.useFabric) {
        std::filesystem::path modsSource = config.modsDir.empty()
            ? gameDir.parent_path() / "mods"
            : config.modsDir;
        syncedMods = syncMods(modsSource, gameDir);
    }

    bool serverListWritten = false;
    if (!config.serverAddress.empty()) {
        serverListWritten = writeServerList(gameDir, config.serverName, config.serverAddress);
    }

    std::wstring javaPath = findJavaExe();
    if (javaPath.empty()) {
        return {false, "Java не найдена. Установите Java 17+ или задайте JAVA_HOME", 0};
    }

    auto parseArgsArray = [](const JsonValue& arr, std::vector<std::string>& out) {
        if (arr.type != JsonValue::Array) return;
        for (size_t i = 0; i < arr.size(); ++i) {
            const auto& entry = arr[i];
            if (entry.type == JsonValue::String) {
                out.push_back(entry.string());
            } else if (entry.type == JsonValue::Object) {
                if (entry.contains("rules")) {
                    bool allow = false;
                    const auto& rules = entry["rules"];
                    if (rules.type == JsonValue::Array) {
                        for (size_t r = 0; r < rules.size(); ++r) {
                            const auto& rule = rules[r];
                            bool actionAllow = rule.contains("action") &&
                                rule["action"].string() == "allow";
                            bool osMatch = true;
                            if (rule.contains("os")) {
                                const auto& os = rule["os"];
                                osMatch = false;
                                if (os.contains("name") && os["name"].string() == "windows")
                                    osMatch = true;
                            }
                            if (r == rules.size() - 1) {
                                allow = actionAllow && osMatch;
                            } else if (actionAllow && osMatch) {
                                allow = true;
                            } else if (!actionAllow && osMatch) {
                                allow = false;
                            }
                        }
                    }
                    if (allow && entry.contains("value")) {
                        if (entry["value"].type == JsonValue::Array) {
                            for (size_t v = 0; v < entry["value"].size(); ++v) {
                                out.push_back(entry["value"][v].string());
                            }
                        } else if (entry["value"].type == JsonValue::String) {
                            out.push_back(entry["value"].string());
                        }
                    }
                } else if (entry.contains("value")) {
                    if (entry["value"].type == JsonValue::Array) {
                        for (size_t v = 0; v < entry["value"].size(); ++v) {
                            out.push_back(entry["value"][v].string());
                        }
                    } else if (entry["value"].type == JsonValue::String) {
                        out.push_back(entry["value"].string());
                    }
                }
            }
        }
    };

    std::vector<std::pair<std::string, std::string>> fabricLibs;
    std::vector<std::pair<std::string, std::string>> mcLibs;
    std::vector<std::string> jvmArgs;
    std::vector<std::string> gameArgs;
    std::string mainClass;
    std::string clientJarPath;
    std::string assetIndexId;
    std::string versionId;

    if (!std::filesystem::exists(versionsDir)) {
        return {false, "Папка versions не найдена", 0};
    }

    if (config.useFabric) {
        std::string fabricVersionId;
        for (const auto& entry : std::filesystem::directory_iterator(versionsDir)) {
            if (entry.is_directory()) {
                std::wstring dirName = entry.path().filename().wstring();
                if (dirName.find(L"fabric-loader-") == 0) {
                    fabricVersionId = wideToUtf8(dirName);
                    break;
                }
            }
        }

        if (fabricVersionId.empty()) {
            return {false, "Fabric версия не найдена. Установите Fabric сначала.", 0};
        }

        std::filesystem::path fabricJsonPath = versionsDir /
            std::filesystem::path(utf8ToWide(fabricVersionId)) /
            std::filesystem::path(utf8ToWide(fabricVersionId + ".json"));

        if (!std::filesystem::exists(fabricJsonPath)) {
            return {false, "Файл " + fabricVersionId + ".json не найден", 0};
        }

        std::ifstream fabricFile(fabricJsonPath);
        std::string fabricJson(
            (std::istreambuf_iterator<char>(fabricFile)),
            std::istreambuf_iterator<char>());
        fabricFile.close();

        JsonValue fabricData = parseJson(fabricJson);

        std::string inheritsFrom = fabricData.contains("inheritsFrom")
            ? fabricData["inheritsFrom"].string() : "";

        mainClass = fabricData.contains("mainClass")
            ? fabricData["mainClass"].string()
            : "net.fabricmc.loader.impl.launch.knot.KnotClient";

        versionId = fabricVersionId;

        if (fabricData.contains("libraries") && fabricData["libraries"].type == JsonValue::Array) {
            const auto& libs = fabricData["libraries"];
            for (size_t i = 0; i < libs.size(); ++i) {
                std::string name = libs[i].contains("name") ? libs[i]["name"].string() : "";
                if (!name.empty()) {
                    std::string path = mavenNameToPath(name);
                    fabricLibs.push_back({name, path});
                }
            }
        }

        if (!inheritsFrom.empty()) {
            std::filesystem::path baseJsonPath = versionsDir /
                std::filesystem::path(utf8ToWide(inheritsFrom)) /
                std::filesystem::path(utf8ToWide(inheritsFrom + ".json"));

            if (std::filesystem::exists(baseJsonPath)) {
                std::ifstream baseFile(baseJsonPath);
                std::string baseJson(
                    (std::istreambuf_iterator<char>(baseFile)),
                    std::istreambuf_iterator<char>());
                baseFile.close();

                JsonValue baseData = parseJson(baseJson);

                if (baseData.contains("libraries") && baseData["libraries"].type == JsonValue::Array) {
                    const auto& libs = baseData["libraries"];
                    for (size_t i = 0; i < libs.size(); ++i) {
                        std::string name = libs[i].contains("name") ? libs[i]["name"].string() : "";
                        if (!name.empty()) {
                            std::string path = mavenNameToPath(name);
                            mcLibs.push_back({name, path});
                        }
                    }
                }

                if (baseData.contains("arguments")) {
                    const auto& args = baseData["arguments"];
                    if (args.contains("jvm")) parseArgsArray(args["jvm"], jvmArgs);
                    if (args.contains("game")) parseArgsArray(args["game"], gameArgs);
                }

                if (baseData.contains("downloads") && baseData["downloads"].contains("client")) {
                    const auto& client = baseData["downloads"]["client"];
                    clientJarPath = client.contains("path") ? client["path"].string() : "";
                }

                if (baseData.contains("assetIndex") && baseData["assetIndex"].type == JsonValue::Object) {
                    assetIndexId = baseData["assetIndex"].contains("id")
                        ? baseData["assetIndex"]["id"].string() : "";
                }

                if (mainClass.empty() && baseData.contains("mainClass")) {
                    mainClass = baseData["mainClass"].string();
                }
            }
        }
    } else {
        versionId = config.mcVersion;

        std::filesystem::path jsonPath = versionsDir /
            std::filesystem::path(utf8ToWide(config.mcVersion)) /
            std::filesystem::path(utf8ToWide(config.mcVersion + ".json"));

        if (!std::filesystem::exists(jsonPath)) {
            return {false, "Файл " + config.mcVersion + ".json не найден", 0};
        }

        std::ifstream jsonFile(jsonPath);
        std::string versionJson(
            (std::istreambuf_iterator<char>(jsonFile)),
            std::istreambuf_iterator<char>());
        jsonFile.close();

        JsonValue versionData = parseJson(versionJson);

        mainClass = versionData.contains("mainClass")
            ? versionData["mainClass"].string()
            : "net.minecraft.client.main.Minecraft";

        if (versionData.contains("libraries") && versionData["libraries"].type == JsonValue::Array) {
            const auto& libs = versionData["libraries"];
            for (size_t i = 0; i < libs.size(); ++i) {
                std::string name = libs[i].contains("name") ? libs[i]["name"].string() : "";
                if (!name.empty()) {
                    std::string path = mavenNameToPath(name);
                    mcLibs.push_back({name, path});
                }
            }
        }

        if (versionData.contains("arguments")) {
            const auto& args = versionData["arguments"];
            if (args.contains("jvm")) parseArgsArray(args["jvm"], jvmArgs);
            if (args.contains("game")) parseArgsArray(args["game"], gameArgs);
        } else if (versionData.contains("minecraftArguments")) {
            std::string gameArgStr = versionData["minecraftArguments"].string();
            std::istringstream iss(gameArgStr);
            std::string token;
            while (iss >> token) gameArgs.push_back(token);
        }

        if (versionData.contains("downloads") && versionData["downloads"].contains("client")) {
            const auto& client = versionData["downloads"]["client"];
            clientJarPath = client.contains("path") ? client["path"].string() : "";
        }

        if (versionData.contains("assetIndex") && versionData["assetIndex"].type == JsonValue::Object) {
            assetIndexId = versionData["assetIndex"].contains("id")
                ? versionData["assetIndex"]["id"].string() : "";
        }
    }

    std::string classpath;
    auto addLibToClasspath = [&](const std::string& mavenPath) {
        if (mavenPath.empty()) return;
        std::filesystem::path jarPath = librariesDir / std::filesystem::path(utf8ToWide(mavenPath));
        if (std::filesystem::exists(jarPath)) {
            if (!classpath.empty()) classpath += ";";
            classpath += wideToUtf8(jarPath.wstring());
        }
    };

    for (const auto& [name, path] : fabricLibs) {
        addLibToClasspath(path);
    }
    for (const auto& [name, path] : mcLibs) {
        addLibToClasspath(path);
    }

    if (clientJarPath.empty()) {
        std::filesystem::path defaultClientJar = versionsDir /
            std::filesystem::path(utf8ToWide(config.mcVersion)) /
            std::filesystem::path(utf8ToWide(config.mcVersion + ".jar"));
        if (std::filesystem::exists(defaultClientJar)) {
            clientJarPath = wideToUtf8(defaultClientJar.wstring());
        }
    } else {
        std::filesystem::path clientJar = gameDir / std::filesystem::path(utf8ToWide(clientJarPath));
        clientJarPath = wideToUtf8(clientJar.wstring());
    }

    if (!clientJarPath.empty()) {
        if (!classpath.empty()) classpath += ";";
        classpath += clientJarPath;
    }

    if (classpath.empty()) {
        return {false, "Classpath пуст - библиотеки не найдены", 0};
    }

    std::wstring gameDirW = gameDir.wstring();
    std::wstring versionsDirW = versionsDir.wstring();
    std::wstring assetsDirW = assetsDir.wstring();

    std::string assetIndexName = assetIndexId.empty() ? config.mcVersion : assetIndexId;

    std::filesystem::path authlibInjectorPath = gameDir / "authlib-injector-1.2.8.jar";
    if (!config.accessToken.empty() && !std::filesystem::exists(authlibInjectorPath)) {
        constexpr const char* authlibUrl =
            "https://github.com/yushijinhun/authlib-injector/releases/download/v1.2.8/authlib-injector-1.2.8.jar";

        bool downloaded = httpGetToFile(authlibUrl, authlibInjectorPath);

        if (!downloaded) {
            // Fallback: curl.exe
            std::error_code ec;
            std::filesystem::create_directories(authlibInjectorPath.parent_path(), ec);
            const std::filesystem::path partialPath = authlibInjectorPath.string() + ".part";
            std::filesystem::remove(partialPath, ec);

            std::string command = "curl.exe --fail --location --silent --show-error --retry 3 --connect-timeout 15 --output \""
                + wideToUtf8(partialPath.wstring()) + "\" \"" + authlibUrl + "\"";
            int rc = std::system(command.c_str());

            if (rc == 0 && std::filesystem::exists(partialPath) &&
                std::filesystem::file_size(partialPath, ec) > 0) {
                std::filesystem::remove(authlibInjectorPath, ec);
                std::filesystem::rename(partialPath, authlibInjectorPath, ec);
                downloaded = !ec;
            } else {
                std::filesystem::remove(partialPath, ec);
            }
        }

        if (!downloaded) {
            return {false,
                "Не удалось скачать authlib-injector. Проверьте подключение к интернету. "
                "Если GitHub недоступен, положите authlib-injector-1.2.8.jar вручную в папку minecraft.",
                0};
        }
    }

    std::array<unsigned char, 16> uuidBytes{};
    if (!config.uuid.empty()) {
        std::string cleanUuid;
        for (char c : config.uuid) if (c != '-') cleanUuid += c;
        if (cleanUuid.size() == 32) {
            for (size_t i = 0; i < 16; ++i) {
                uuidBytes[i] = static_cast<unsigned char>(std::strtoul(cleanUuid.substr(i * 2, 2).c_str(), nullptr, 16));
            }
        }
    }
    if (config.uuid.empty()) {
        std::string uuidSrc = "OfflinePlayer:" + config.username;
        for (size_t i = 0; i < uuidSrc.size(); ++i) {
            uuidBytes[i % 16] ^= static_cast<unsigned char>(uuidSrc[i]);
        }
        uuidBytes[6] = (uuidBytes[6] & 0x0f) | 0x30;
        uuidBytes[8] = (uuidBytes[8] & 0x3f) | 0x80;
    }
    char uuidStr[37];
    snprintf(uuidStr, sizeof(uuidStr),
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        uuidBytes[0], uuidBytes[1], uuidBytes[2], uuidBytes[3],
        uuidBytes[4], uuidBytes[5],
        uuidBytes[6], uuidBytes[7],
        uuidBytes[8], uuidBytes[9],
        uuidBytes[10], uuidBytes[11], uuidBytes[12], uuidBytes[13], uuidBytes[14], uuidBytes[15]);
    std::string authUuid = uuidStr;

    std::string classpathDir = wideToUtf8(librariesDir.wstring());
    std::string nativesDir = wideToUtf8(versionsDirW);
    std::string assetsRoot = wideToUtf8(assetsDirW);
    std::string gameDirStr = wideToUtf8(gameDirW);

    auto resolveJvmArg = [&](const std::string& arg) -> std::string {
        std::string result = arg;
        auto replace = [&](const std::string& key, const std::string& val) {
            size_t pos;
            while ((pos = result.find(key)) != std::string::npos) {
                result.replace(pos, key.size(), val);
            }
        };
        replace("${natives_directory}", nativesDir);
        replace("${launcher_name}", "AmethystLauncher");
        replace("${launcher_version}", "1.0");
        replace("${library_directory}", classpathDir);
        replace("${classpath_separator}", ";");
        replace("${classpath_path}", classpath);
        replace("${classpath}", classpath);
        return result;
    };

    auto resolveGameArg = [&](const std::string& arg) -> std::string {
        std::string result = arg;
        auto replace = [&](const std::string& key, const std::string& val) {
            size_t pos;
            while ((pos = result.find(key)) != std::string::npos) {
                result.replace(pos, key.size(), val);
            }
        };
        replace("${auth_player_name}", config.username);
        replace("${auth_session}", config.accessToken.empty() ? "0" : config.accessToken);
        replace("${auth_access_token}", config.accessToken.empty() ? "0" : config.accessToken);
        replace("${auth_uuid}", authUuid);
        replace("${auth_xuid}", "");
        replace("${xuid}", "");
        replace("${user_type}", config.accessToken.empty() ? "legacy" : "mojang");
        replace("${user_properties}", "{}");
        replace("${game_directory}", gameDirStr);
        replace("${assets_root}", assetsRoot);
        replace("${assets_index_name}", assetIndexName);
        replace("${version_name}", versionId);
        replace("${version_type}", "release");
        replace("${resolution_width}", "854");
        replace("${resolution_height}", "480");
        replace("${quickPlayPath}", "");
        replace("${quickPlaySingleplayer}", "");
        replace("${quickPlayMultiplayer}", config.serverAddress);
        replace("${quickPlayRealms}", "");
        return result;
    };

    std::vector<std::wstring> cmdLine;

    bool hasXmx = false;
    bool hasXms = false;
    bool hasDsunJava2d = false;
    for (const auto& arg : jvmArgs) {
        if (arg.find("-Xmx") != std::string::npos) hasXmx = true;
        if (arg.find("-Xms") != std::string::npos) hasXms = true;
        if (arg.find("-Dsun.java2d") != std::string::npos) hasDsunJava2d = true;
    }

    cmdLine.push_back(javaPath);

    if (!config.accessToken.empty()) {
        cmdLine.push_back(
            L"-javaagent:" + authlibInjectorPath.wstring() +
            L"=https://authserver.ely.by/api/authlib-injector"
        );
    }

    for (const auto& arg : jvmArgs) {
        std::string resolved = resolveJvmArg(arg);
        if (!resolved.empty()) {
            cmdLine.push_back(utf8ToWide(resolved));
        }
    }

    if (!hasXmx) {
        cmdLine.push_back(L"-Xmx" + std::to_wstring(config.maxMemoryMB) + L"m");
    }
    if (!hasXms) {
        cmdLine.push_back(L"-Xms512m");
    }
    if (!hasDsunJava2d) {
        cmdLine.push_back(L"-Dsun.java2d.uiScale=1");
    }

    cmdLine.push_back(utf8ToWide(mainClass));

    bool hasUserType = false;
    for (const auto& arg : gameArgs) {
        std::string resolved = resolveGameArg(arg);
        if (!resolved.empty()) {
            if (resolved == "--userType") hasUserType = true;
        }
    }

    for (size_t i = 0; i < gameArgs.size(); ++i) {
        std::string resolved = resolveGameArg(gameArgs[i]);
        if (resolved.empty()) continue;

        if (resolved == "--demo") continue;

        if (resolved.find("${") != std::string::npos) {
            if (i + 1 < gameArgs.size()) i++;
            continue;
        }

        if (resolved.size() >= 2 && resolved[0] == '-' && resolved[1] == '-') {
            if (i + 1 < gameArgs.size()) {
                std::string nextResolved = resolveGameArg(gameArgs[i + 1]);
                if (nextResolved.empty()) {
                    i++;
                    continue;
                }
            }
        }

        cmdLine.push_back(utf8ToWide(resolved));
    }

    if (!hasUserType) {
        cmdLine.push_back(L"--userType");
        cmdLine.push_back(config.accessToken.empty() ? L"legacy" : L"mojang");
    }

    std::wstring cmdLineStr;
    for (size_t i = 0; i < cmdLine.size(); ++i) {
        if (i > 0) cmdLineStr += L' ';
        const std::wstring& arg = cmdLine[i];
        bool needsQuote = arg.find(L' ') != std::wstring::npos ||
                          arg.find(L'\t') != std::wstring::npos ||
                          arg.empty();
        if (needsQuote) {
            cmdLineStr += L'"';
            for (wchar_t c : arg) {
                if (c == L'"') cmdLineStr += L"\\\"";
                else cmdLineStr += c;
            }
            cmdLineStr += L'"';
        } else {
            cmdLineStr += arg;
        }
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    std::vector<wchar_t> cmdBuf(cmdLineStr.begin(), cmdLineStr.end());
    cmdBuf.push_back(0);

    BOOL result = CreateProcessW(
        nullptr,
        cmdBuf.data(),
        nullptr,
        nullptr,
        FALSE,
        CREATE_NEW_CONSOLE,
        nullptr,
        gameDirW.c_str(),
        &si,
        &pi
    );

    std::filesystem::path logPath = gameDir / "launch_log.txt";
    {
        std::ofstream logFile(logPath);
        logFile << "=== Amethyst Launcher Launch Log ===\n\n";
        logFile << "Java: " << wideToUtf8(javaPath) << "\n";
        logFile << "Main class: " << mainClass << "\n";
        logFile << "Version: " << versionId << "\n";
        logFile << "Fabric: " << (config.useFabric ? "yes" : "no") << "\n";
        logFile << "Game dir: " << gameDirStr << "\n";
        logFile << "Assets: " << assetsRoot << "\n";
        logFile << "Asset index: " << assetIndexName << "\n";
        logFile << "Classpath entries: " << fabricLibs.size() + mcLibs.size() << "\n";
        logFile << "Mods synced: " << syncedMods << "\n";
        logFile << "Server: " << (config.serverAddress.empty() ? "-" : config.serverAddress) << "\n";
        logFile << "Server list written: " << (serverListWritten ? "yes" : "no") << "\n";
        logFile << "Ely auth: " << (config.accessToken.empty() ? "no" : "yes") << "\n";
        logFile << "Ely uuid: " << (config.uuid.empty() ? "-" : config.uuid) << "\n\n";

        logFile << "--- JVM Arguments (" << jvmArgs.size() << ") ---\n";
        for (const auto& a : jvmArgs) logFile << "  " << resolveJvmArg(a) << "\n";
        logFile << "\n--- Game Arguments (" << gameArgs.size() << ") ---\n";
        for (const auto& a : gameArgs) logFile << "  " << resolveGameArg(a) << "\n";
        logFile << "\n--- Full Command ---\n";
        logFile << wideToUtf8(cmdLineStr) << "\n";
    }

    if (!result) {
        DWORD err = GetLastError();
        return {false, "CreateProcess ошибка: " + std::to_string(static_cast<int>(err)), 0};
    }

    CloseHandle(pi.hThread);

    if (!config.serverAddress.empty()) {
        struct ServerListRestoreTask {
            HANDLE process;
            std::filesystem::path gameDir;
            std::string name;
            std::string address;
        };

        auto restoreFunc = [](LPVOID param) -> DWORD {
            auto* task = static_cast<ServerListRestoreTask*>(param);
            WaitForSingleObject(task->process, INFINITE);
            writeServerList(task->gameDir, task->name, task->address);
            CloseHandle(task->process);
            delete task;
            return 0;
        };

        HANDLE restoreThread = CreateThread(
            nullptr,
            0,
            restoreFunc,
            new ServerListRestoreTask{pi.hProcess, gameDir, config.serverName, config.serverAddress},
            0,
            nullptr
        );
        if (restoreThread) CloseHandle(restoreThread);
        if (!restoreThread) CloseHandle(pi.hProcess);
    } else {
        CloseHandle(pi.hProcess);
    }

    return {true, "", pi.dwProcessId};
}