#pragma once

#include "downloader.h"

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

constexpr const char* VERSION_MANIFEST_URL = "https://launchermeta.mojang.com/mc/game/version_manifest_v2.json";
constexpr const char* LIBRARIES_BASE = "https://libraries.minecraft.net/";
constexpr const char* RESOURCES_BASE = "https://resources.download.minecraft.net/";
constexpr const char* FABRIC_META_BASE = "https://meta.fabricmc.net/v2/versions/loader/";
constexpr const char* MODPACK_URL = "https://www.dropbox.com/scl/fi/r2uyocoh0fhsouq65a5f8/.zip?rlkey=cohzdezhjtwda62vvwf2hxneo&st=gwvtzk7u&dl=1";

enum class InstallPhase {
    Idle,
    Manifest,
    VersionJson,
    Libraries,
    ClientJar,
    Assets,
    Fabric,
    Mods,
    Done,
    Error
};

struct InstallState {
    std::mutex mtx;
    InstallPhase phase = InstallPhase::Idle;
    std::string phaseText;
    std::string phaseDetail;
    int total = 0;
    int current = 0;
    std::string error;
    bool cancel = false;
    bool running = false;

    void set(InstallPhase p, const std::string& text, const std::string& detail = "", int t = 0, int c = 0) {
        std::lock_guard lock(mtx);
        phase = p;
        phaseText = text;
        phaseDetail = detail;
        total = t;
        current = c;
    }

    void setError(const std::string& msg) {
        std::lock_guard lock(mtx);
        phase = InstallPhase::Error;
        error = msg;
        running = false;
    }

    void finish() {
        std::lock_guard lock(mtx);
        phase = InstallPhase::Done;
        running = false;
    }

    bool shouldCancel() {
        std::lock_guard lock(mtx);
        return cancel;
    }
};

struct MinecraftLibrary {
    std::string name;
    std::string url;
    std::string sha1;
    int64_t size = 0;
    std::string classifier;
    bool isNative = false;
};

inline std::string mavenNameToPath(const std::string& name, const std::string& classifier = "") {
    size_t firstColon = name.find(':');
    size_t secondColon = name.find(':', firstColon + 1);
    if (firstColon == std::string::npos || secondColon == std::string::npos) return {};

    std::string group = name.substr(0, firstColon);
    std::string artifact = name.substr(firstColon + 1, secondColon - firstColon - 1);
    std::string version;
    std::string nameClassifier;

    size_t thirdColon = name.find(':', secondColon + 1);
    if (thirdColon != std::string::npos) {
        version = name.substr(secondColon + 1, thirdColon - secondColon - 1);
        size_t fourthColon = name.find(':', thirdColon + 1);
        if (fourthColon != std::string::npos) {
            nameClassifier = name.substr(thirdColon + 1, fourthColon - thirdColon - 1);
        } else {
            nameClassifier = name.substr(thirdColon + 1);
        }
    } else {
        version = name.substr(secondColon + 1);
    }

    std::string effectiveClassifier = classifier.empty() ? nameClassifier : classifier;

    std::string groupPath = group;
    for (char& c : groupPath) {
        if (c == '.') c = '/';
    }

    std::string jarName = artifact + "-" + version;
    if (!effectiveClassifier.empty()) jarName += "-" + effectiveClassifier;
    jarName += ".jar";

    return groupPath + "/" + artifact + "/" + version + "/" + jarName;
}

inline bool shouldIncludeLibrary(const JsonValue& lib) {
    if (!lib.contains("rules")) return true;

    const auto& rules = lib["rules"];
    if (rules.type != JsonValue::Array) return true;

    bool allowed = false;
    for (size_t i = 0; i < rules.size(); ++i) {
        const auto& rule = rules[i];
        bool actionAllow = rule.contains("action") && rule["action"].string() == "allow";
        bool osMatch = true;

        if (rule.contains("os")) {
            const auto& os = rule["os"];
            osMatch = false;
            if (os.contains("name") && os["name"].string() == "windows") osMatch = true;
        }

        if (i == rules.size() - 1) {
            allowed = actionAllow && osMatch;
        } else if (actionAllow && osMatch) {
            allowed = true;
        } else if (!actionAllow && osMatch) {
            allowed = false;
        }
    }
    return allowed;
}

inline std::string getFabricLoaderVersion(const std::string& mcVersion) {
    std::string url = std::string(FABRIC_META_BASE) + mcVersion;
    std::string resp = httpGetString(url);
    if (resp.empty()) return {};

    JsonValue versions = parseJson(resp);
    if (versions.type == JsonValue::Array && versions.size() > 0) {
        for (size_t i = 0; i < versions.size(); ++i) {
            const auto& entry = versions[i];
            if (entry.type == JsonValue::Object && entry.contains("loader")) {
                const auto& loader = entry["loader"];
                if (loader.contains("version")) return loader["version"].string();
            }
        }
        const auto& first = versions.arrVal[0];
        if (first.type == JsonValue::Object) {
            if (first.contains("version")) return first["version"].string();
        }
    }
    return {};
}

struct InstallResult {
    bool success = false;
    std::string error;
};

inline std::string powerShellLiteral(const std::filesystem::path& path) {
    std::string value = wideToUtf8(path.wstring());
    std::string escaped;
    escaped.reserve(value.size() + 8);
    for (char c : value) {
        if (c == '\'') escaped += "''";
        else escaped += c;
    }
    return "'" + escaped + "'";
}

inline bool extractModpack(const std::filesystem::path& archive, const std::filesystem::path& modsDir) {
    const std::filesystem::path extractDir = archive.parent_path() / ".lemon-modpack-extracted";
    std::error_code ec;
    std::filesystem::remove_all(extractDir, ec);
    std::filesystem::create_directories(extractDir, ec);
    std::filesystem::create_directories(modsDir, ec);

    const std::string command = "powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command \"$ErrorActionPreference='Stop'; Expand-Archive -LiteralPath "
        + powerShellLiteral(archive) + " -DestinationPath " + powerShellLiteral(extractDir) + " -Force\"";
    if (std::system(command.c_str()) != 0) {
        std::filesystem::remove_all(extractDir, ec);
        return false;
    }

    bool success = true;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(extractDir, ec)) {
        if (ec) { success = false; break; }
        if (!entry.is_regular_file()) continue;
        const auto extension = entry.path().extension().wstring();
        if (extension != L".jar" && extension != L".JAR") continue;
        try {
            std::filesystem::copy_file(
                entry.path(), modsDir / entry.path().filename(),
                std::filesystem::copy_options::overwrite_existing);
        } catch (...) {
            success = false;
            break;
        }
    }
    std::filesystem::remove_all(extractDir, ec);
    return success;
}

inline bool downloadModpack(const std::string& url, const std::filesystem::path& destPath) {
    std::error_code ec;
    std::filesystem::create_directories(destPath.parent_path(), ec);
    const std::filesystem::path partialPath = destPath.string() + ".part";
    std::filesystem::remove(partialPath, ec);

    const std::string command = "curl.exe --fail --location --silent --show-error --retry 3 --connect-timeout 15 --output \""
        + wideToUtf8(partialPath.wstring()) + "\" \"" + url + "\"";
    if (std::system(command.c_str()) != 0 || !std::filesystem::exists(partialPath) ||
        std::filesystem::file_size(partialPath, ec) == 0) {
        std::filesystem::remove(partialPath, ec);
        return false;
    }

    std::filesystem::remove(destPath, ec);
    std::filesystem::rename(partialPath, destPath, ec);
    if (ec) {
        std::filesystem::remove(partialPath, ec);
        return false;
    }
    return true;
}

inline InstallResult installMinecraftAndFabric(
    const std::filesystem::path& gameDir,
    const std::string& mcVersion,
    InstallState& state
) {
    auto cancelCheck = [&]() -> bool { return state.shouldCancel(); };
    DownloadProgress dp{nullptr, cancelCheck};

    std::filesystem::create_directories(gameDir);

    // 1. Download version manifest
    state.set(InstallPhase::Manifest, "Получение манифеста версий...");
    std::string manifestJson = httpGetString(VERSION_MANIFEST_URL);
    if (manifestJson.empty()) {
        state.setError("Не удалось скачать манифест версий");
        return {false, "manifest"};
    }

    JsonValue manifest = parseJson(manifestJson);

    // Find version
    std::string versionUrl;
    if (manifest.contains("versions") && manifest["versions"].type == JsonValue::Array) {
        const auto& versions = manifest["versions"];
        for (size_t i = 0; i < versions.size(); ++i) {
            if (versions[i].contains("id") && versions[i]["id"].string() == mcVersion) {
                versionUrl = versions[i]["url"].string();
                break;
            }
        }
    }

    if (versionUrl.empty()) {
        state.setError(std::string("Версия ") + mcVersion + " не найдена в манифесте");
        return {false, "version not found"};
    }

    // 2. Download version JSON
    state.set(InstallPhase::VersionJson, std::string("Скачивание JSON версии ") + mcVersion + "...");
    std::string versionJson = httpGetString(versionUrl);
    if (versionJson.empty()) {
        state.setError("Не удалось скачать JSON версии");
        return {false, "version json"};
    }

    JsonValue versionData = parseJson(versionJson);

    // Parse libraries
    std::vector<MinecraftLibrary> libraries;
    if (versionData.contains("libraries") && versionData["libraries"].type == JsonValue::Array) {
        const auto& libs = versionData["libraries"];
        for (size_t i = 0; i < libs.size(); ++i) {
            const auto& lib = libs[i];
            if (!shouldIncludeLibrary(lib)) continue;

            MinecraftLibrary mlib;
            mlib.name = lib.contains("name") ? lib["name"].string() : "";

            if (lib.contains("downloads") && lib["downloads"].contains("artifact")) {
                const auto& artifact = lib["downloads"]["artifact"];
                mlib.url = artifact.contains("url") ? artifact["url"].string() : "";
                mlib.sha1 = artifact.contains("sha1") ? artifact["sha1"].string() : "";
                mlib.size = static_cast<int64_t>(artifact.contains("size") ? artifact["size"].number() : 0);
            }

            if (mlib.url.empty() && !mlib.name.empty()) {
                std::string path = mavenNameToPath(mlib.name);
                if (!path.empty()) mlib.url = std::string(LIBRARIES_BASE) + path;
            }

            if (!mlib.url.empty()) {
                libraries.push_back(std::move(mlib));
            }

            if (lib.contains("natives") && lib["natives"].contains("windows")) {
                std::string classifier = lib["natives"]["windows"].string();
                MinecraftLibrary nativeLib;
                nativeLib.name = lib.contains("name") ? lib["name"].string() : "";
                nativeLib.classifier = classifier;

                if (lib.contains("downloads") && lib["downloads"].contains("classifiers") &&
                    lib["downloads"]["classifiers"].contains(classifier)) {
                    const auto& cls = lib["downloads"]["classifiers"][classifier];
                    nativeLib.url = cls.contains("url") ? cls["url"].string() : "";
                    nativeLib.sha1 = cls.contains("sha1") ? cls["sha1"].string() : "";
                    nativeLib.size = static_cast<int64_t>(cls.contains("size") ? cls["size"].number() : 0);
                }

                if (nativeLib.url.empty() && !nativeLib.name.empty()) {
                    std::string path = mavenNameToPath(nativeLib.name, classifier);
                    if (!path.empty()) nativeLib.url = std::string(LIBRARIES_BASE) + path;
                }

                if (!nativeLib.url.empty()) {
                    nativeLib.isNative = true;
                    libraries.push_back(std::move(nativeLib));
                }
            }
        }
    }

    // 3. Download libraries
    {
        int totalLibs = static_cast<int>(libraries.size());
        state.set(InstallPhase::Libraries, "Скачивание библиотек...", "", totalLibs, 0);
        int libIdx = 0;
        for (const auto& lib : libraries) {
            if (state.shouldCancel()) {
                state.setError("Отменено");
                return {false, "cancelled"};
            }

            state.set(InstallPhase::Libraries,
                      "Скачивание библиотек...",
                      lib.name,
                      totalLibs,
                      libIdx);

            std::filesystem::path dest = gameDir / "libraries" / std::filesystem::path(
                utf8ToWide(mavenNameToPath(lib.name, lib.classifier)));

            if (!downloadFile(lib.url, dest, lib.sha1, dp)) {
                state.setError("Ошибка скачивания: " + lib.name);
                return {false, lib.name};
            }
            ++libIdx;
        }
    }

    // 4. Download client JAR
    state.set(InstallPhase::ClientJar, std::string("Скачивание клиента ") + mcVersion + "...");
    {
        std::string clientUrl;
        std::string clientSha1;

        if (versionData.contains("downloads") && versionData["downloads"].contains("client")) {
            const auto& client = versionData["downloads"]["client"];
            clientUrl = client.contains("url") ? client["url"].string() : "";
            clientSha1 = client.contains("sha1") ? client["sha1"].string() : "";
        }

        if (clientUrl.empty()) {
            state.setError("URL клиента не найден");
            return {false, "no client url"};
        }

        std::filesystem::path versionDir = gameDir / "versions" / std::filesystem::path(utf8ToWide(mcVersion));
        std::filesystem::create_directories(versionDir);

        std::filesystem::path clientJar = versionDir / std::filesystem::path(utf8ToWide(mcVersion + ".jar"));
        if (!downloadFile(clientUrl, clientJar, clientSha1, dp)) {
            state.setError("Ошибка скачивания клиента");
            return {false, "client jar"};
        }

        std::filesystem::path versionJsonPath = versionDir / std::filesystem::path(utf8ToWide(mcVersion + ".json"));
        std::ofstream vjFile(versionJsonPath);
        vjFile << versionJson;
        vjFile.close();
    }

    // 5. Download assets
    state.set(InstallPhase::Assets, "Скачивание ассетов...");
    if (versionData.contains("assetIndex") && versionData["assetIndex"].type == JsonValue::Object) {
        const auto& assetIndex = versionData["assetIndex"];
        std::string assetIndexUrl = assetIndex.contains("url") ? assetIndex["url"].string() : "";
        std::string assetIndexId = assetIndex.contains("id") ? assetIndex["id"].string() : "";
        std::string assetIndexSha1 = assetIndex.contains("sha1") ? assetIndex["sha1"].string() : "";

        if (!assetIndexUrl.empty() && !assetIndexId.empty()) {
            std::filesystem::path indexesDir = gameDir / "assets" / "indexes";
            std::filesystem::create_directories(indexesDir);
            std::filesystem::path indexPath = indexesDir / std::filesystem::path(utf8ToWide(assetIndexId + ".json"));

            if (!downloadFile(assetIndexUrl, indexPath, assetIndexSha1, dp)) {
                state.setError("Ошибка скачивания индекса ассетов");
                return {false, "asset index"};
            }

            std::ifstream indexFile(indexPath);
            std::string indexContent(
                (std::istreambuf_iterator<char>(indexFile)),
                std::istreambuf_iterator<char>());
            indexFile.close();

            JsonValue indexData = parseJson(indexContent);

            if (indexData.contains("objects") && indexData["objects"].type == JsonValue::Object) {
                const JsonValue& objects = indexData["objects"];
                int totalAssets = static_cast<int>(objects.objVal.size());
                int assetIdx = 0;

                state.set(InstallPhase::Assets, "Скачивание ассетов...", "", totalAssets, 0);

                for (const auto& kv : objects.objVal) {
                    if (state.shouldCancel()) {
                        state.setError("Отменено");
                        return {false, "cancelled"};
                    }

                    const std::string& assetName = kv.first;
                    const JsonValue& assetEntry = kv.second;

                    std::string hash = assetEntry.contains("hash") ? assetEntry["hash"].string() : "";
                    if (hash.empty() || hash.size() < 2) continue;

                    std::string hashPrefix = hash.substr(0, 2);
                    std::string assetUrl = std::string(RESOURCES_BASE) + hashPrefix + "/" + hash;

                    std::filesystem::path objectsDir = gameDir / "assets" / "objects" / std::filesystem::path(utf8ToWide(hashPrefix));
                    std::filesystem::create_directories(objectsDir);
                    std::filesystem::path assetPath = objectsDir / std::filesystem::path(utf8ToWide(hash));

                    state.set(InstallPhase::Assets, "Скачивание ассетов...", assetName, totalAssets, assetIdx);

                    if (!downloadFile(assetUrl, assetPath, hash, dp)) {
                        state.setError("Ошибка скачивания ассета: " + assetName);
                        return {false, assetName};
                    }
                    ++assetIdx;
                }
            }
        }
    }

    // 6. Install Fabric
    state.set(InstallPhase::Fabric, "Получение информации о Fabric...");
    std::string fabricLoaderVersion = getFabricLoaderVersion(mcVersion);
    if (fabricLoaderVersion.empty()) {
        state.setError("Не удалось получить версию Fabric loader");
        return {false, "fabric version"};
    }

    state.set(InstallPhase::Fabric, "Скачивание профиля Fabric " + fabricLoaderVersion + "...");
    std::string fabricProfileUrl = std::string(FABRIC_META_BASE) + mcVersion + "/" + fabricLoaderVersion;
    std::string fabricEntryJson = httpGetString(fabricProfileUrl);
    if (fabricEntryJson.empty()) {
        state.setError("Не удалось скачать профиль Fabric");
        return {false, "fabric profile"};
    }

    JsonValue fabricEntry = parseJson(fabricEntryJson);

    std::string fabricLoaderMaven = "net.fabricmc:fabric-loader:" + fabricLoaderVersion;
    std::string fabricVersionId = "fabric-loader-" + fabricLoaderVersion + "-" + mcVersion;

    if (fabricEntry.contains("loader") && fabricEntry["loader"].contains("maven")) {
        fabricLoaderMaven = fabricEntry["loader"]["maven"].string();
    }

    JsonValue fabricProfile;
    fabricProfile["id"] = JsonValue(fabricVersionId);
    fabricProfile["mainClass"] = JsonValue(std::string("net.fabricmc.loader.impl.launch.knot.KnotClient"));
    fabricProfile["type"] = JsonValue(std::string("release"));
    fabricProfile["inheritsFrom"] = JsonValue(mcVersion);

    std::vector<JsonValue> fabricLibEntries;

    if (fabricEntry.contains("launcherMeta") && fabricEntry["launcherMeta"].contains("libraries")) {
        const auto& lmLibs = fabricEntry["launcherMeta"]["libraries"];
        const std::string fabricMaven = "https://maven.fabricmc.net/";

        auto collectFabricLibs = [&](const JsonValue& section) {
            if (section.type != JsonValue::Array) return;
            for (size_t i = 0; i < section.size(); ++i) {
                const auto& lib = section[i];
                JsonValue entry;
                if (lib.contains("name")) entry["name"] = lib["name"];
                if (lib.contains("url")) {
                    entry["url"] = lib["url"];
                } else {
                    entry["url"] = JsonValue(fabricMaven);
                }
                if (lib.contains("sha1")) entry["sha1"] = lib["sha1"];
                if (lib.contains("size")) entry["size"] = lib["size"];
                fabricLibEntries.push_back(std::move(entry));
            }
        };

        if (lmLibs.contains("common")) collectFabricLibs(lmLibs["common"]);
        if (lmLibs.contains("client")) collectFabricLibs(lmLibs["client"]);
    }

    {
        JsonValue fabricIntermediary;
        std::string intMaven = "net.fabricmc:intermediary:" + mcVersion;
        if (fabricEntry.contains("intermediary") && fabricEntry["intermediary"].contains("maven")) {
            intMaven = fabricEntry["intermediary"]["maven"].string();
        }
        fabricIntermediary["name"] = JsonValue(intMaven);
        fabricIntermediary["url"] = JsonValue(std::string("https://maven.fabricmc.net/"));
        fabricIntermediary["server"] = JsonValue(std::string("https://maven.fabricmc.net/"));
        fabricLibEntries.insert(fabricLibEntries.begin(), std::move(fabricIntermediary));
    }

    {
        JsonValue fabricLoaderLib;
        fabricLoaderLib["name"] = JsonValue(fabricLoaderMaven);
        fabricLoaderLib["url"] = JsonValue(std::string("https://maven.fabricmc.net/"));
        fabricLoaderLib["server"] = JsonValue(std::string("https://maven.fabricmc.net/"));
        fabricLibEntries.insert(fabricLibEntries.begin(), std::move(fabricLoaderLib));
    }

    fabricProfile["libraries"] = JsonValue(std::move(fabricLibEntries));

    std::string fabricProfileJson;
    {
        auto serializeJsonValue = [](const JsonValue& v, auto& self) -> std::string {
            if (v.type == JsonValue::Null) return "null";
            if (v.type == JsonValue::Bool) return v.boolVal ? "true" : "false";
            if (v.type == JsonValue::Number) return std::to_string(static_cast<long long>(v.numVal));
            if (v.type == JsonValue::String) {
                std::string result = "\"";
                for (char c : v.strVal) {
                    if (c == '"') result += "\\\"";
                    else if (c == '\\') result += "\\\\";
                    else if (c == '\n') result += "\\n";
                    else if (c == '\r') result += "\\r";
                    else if (c == '\t') result += "\\t";
                    else result += c;
                }
                result += "\"";
                return result;
            }
            if (v.type == JsonValue::Array) {
                std::string result = "[";
                for (size_t i = 0; i < v.size(); ++i) {
                    if (i > 0) result += ",";
                    result += self(v[i], self);
                }
                result += "]";
                return result;
            }
            if (v.type == JsonValue::Object) {
                std::string result = "{";
                bool first = true;
                for (const auto& kv : v.objVal) {
                    if (!first) result += ",";
                    first = false;
                    result += "\"" + kv.first + "\":" + self(kv.second, self);
                }
                result += "}";
                return result;
            }
            return "null";
        };
        fabricProfileJson = serializeJsonValue(fabricProfile, serializeJsonValue);
    }

    // Save Fabric version JSON
    std::filesystem::path fabricVersionDir = gameDir / "versions" / std::filesystem::path(utf8ToWide(fabricVersionId));
    std::filesystem::create_directories(fabricVersionDir);

    std::filesystem::path fabricVersionJsonPath = fabricVersionDir / std::filesystem::path(utf8ToWide(fabricVersionId + ".json"));
    {
        std::ofstream fFile(fabricVersionJsonPath);
        fFile << fabricProfileJson;
        fFile.close();
    }

    // Download Fabric libraries
    std::vector<MinecraftLibrary> fabricLibs;
    if (fabricProfile.contains("libraries") && fabricProfile["libraries"].type == JsonValue::Array) {
        const auto& fLibs = fabricProfile["libraries"];
        for (size_t i = 0; i < fLibs.size(); ++i) {
            const auto& lib = fLibs[i];
            MinecraftLibrary mlib;
            mlib.name = lib.contains("name") ? lib["name"].string() : "";

            if (lib.contains("url")) {
                std::string baseUrl = lib["url"].string();
                std::string path = mavenNameToPath(mlib.name);
                if (!path.empty()) {
                    if (!baseUrl.empty() && baseUrl.back() != '/') baseUrl += '/';
                    mlib.url = baseUrl + path;
                }
            }

            if (mlib.url.empty() && !mlib.name.empty()) {
                std::string path = mavenNameToPath(mlib.name);
                if (!path.empty()) mlib.url = std::string(LIBRARIES_BASE) + path;
            }

            if (lib.contains("downloads") && lib["downloads"].contains("artifact")) {
                const auto& artifact = lib["downloads"]["artifact"];
                if (!artifact.contains("url") || mlib.url.empty()) {
                    mlib.url = artifact.contains("url") ? artifact["url"].string() : mlib.url;
                }
                mlib.sha1 = artifact.contains("sha1") ? artifact["sha1"].string() : "";
                mlib.size = static_cast<int64_t>(artifact.contains("size") ? artifact["size"].number() : 0);
            }

            if (!mlib.url.empty()) fabricLibs.push_back(std::move(mlib));
        }
    }

    {
        int totalFLibs = static_cast<int>(fabricLibs.size());
        state.set(InstallPhase::Fabric, "Скачивание библиотек Fabric...", "", totalFLibs, 0);
        int libIdx = 0;
        for (const auto& lib : fabricLibs) {
            if (state.shouldCancel()) {
                state.setError("Отменено");
                return {false, "cancelled"};
            }

            state.set(InstallPhase::Fabric, "Скачивание библиотек Fabric...", lib.name, totalFLibs, libIdx);

            std::filesystem::path dest = gameDir / "libraries" / std::filesystem::path(
                utf8ToWide(mavenNameToPath(lib.name, lib.classifier)));

            if (!downloadFile(lib.url, dest, lib.sha1, dp)) {
                state.setError("Ошибка скачивания Fabric библиотеки: " + lib.name);
                return {false, lib.name};
            }
            ++libIdx;
        }
    }

    // Download and install the project modpack after Fabric is ready.
    state.set(InstallPhase::Mods, "Скачивание модов...", "Dropbox");
    const std::filesystem::path modpackArchive = gameDir / ".lemon-modpack.zip";
    const std::filesystem::path modsDir = gameDir.parent_path() / "mods";
    if (!downloadModpack(MODPACK_URL, modpackArchive)) {
        state.setError("Не удалось скачать архив модов");
        return {false, "modpack download"};
    }
    if (state.shouldCancel()) {
        std::filesystem::remove(modpackArchive);
        state.setError("Отменено");
        return {false, "cancelled"};
    }
    if (!extractModpack(modpackArchive, modsDir)) {
        std::filesystem::remove(modpackArchive);
        state.setError("Не удалось распаковать архив модов");
        return {false, "modpack extract"};
    }
    std::filesystem::remove(modpackArchive);

    // Write install status
    {
        std::ofstream statusFile(gameDir / ".fabric_install_status");
        statusFile << "installed";
        statusFile.close();
    }

    state.finish();
    return {true, ""};
}

inline void startInstallThread(
    const std::filesystem::path& gameDir,
    const std::string& mcVersion,
    InstallState& state
) {
    {
        std::lock_guard lock(state.mtx);
        if (state.running) return;
        state.running = true;
        state.cancel = false;
        state.phase = InstallPhase::Idle;
        state.error.clear();
    }

    std::thread([gameDir, mcVersion, &state]() {
        installMinecraftAndFabric(gameDir, mcVersion, state);
    }).detach();
}
