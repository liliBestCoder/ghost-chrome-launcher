#define WIN32_LEAN_AND_MEAN
#include "loopback_ui.h"

#include <windows.h>
#include <shellapi.h>
#include <winhttp.h>
#include <algorithm>
#include <charconv>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string_view>
#include <thread>
#include "str_conv.h"

namespace {
using nlohmann::json;
namespace fs = std::filesystem;

constexpr std::string_view kOriginPrefix = "http://127.0.0.1:";
constexpr std::string_view kDataDirFlag = "--user-data-dir=";

json Error(const std::string& code) { return {{"status", "error"}, {"error", code}}; }

bool Ok(const json& result) { return result.value("status", "") == "ok"; }

INTERNET_PORT LocalPort(const std::string& origin) {
    if (origin.compare(0, kOriginPrefix.size(), kOriginPrefix) != 0)
        throw std::runtime_error("expected a loopback API address");
    const auto start = origin.data() + kOriginPrefix.size();
    const auto end = origin.data() + origin.size();
    INTERNET_PORT port = 0;
    const auto parsed = std::from_chars(start, end, port);
    if (parsed.ec != std::errc{} || parsed.ptr != end || !port)
        throw std::runtime_error("invalid API port");
    return port;
}

struct HttpHandle {
    HINTERNET value;
    ~HttpHandle() { if (value) WinHttpCloseHandle(value); }
};

class GhostApi {
public:
    GhostApi(INTERNET_PORT port, const std::string& token)
        : port_(port), headers_(L"Content-Type: application/json\r\nX-Ghost-Plugin-Token: ") {
        if (token.empty() || token.size() > 128 ||
            token.find_first_not_of("0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz{}-") !=
                std::string::npos)
            throw std::runtime_error("invalid plugin token");
        headers_ += StringToWChar(token) + L"\r\n";
    }

    json Call(const wchar_t* operation, const json* payload = nullptr) const {
        const std::wstring path = std::wstring(L"/api/plugin/v1/") + operation;
        std::string body = payload ? payload->dump() : "";
        const auto bytes = static_cast<DWORD>(body.size());
        const HttpHandle session{WinHttpOpen(L"Ghost Chrome Launcher", WINHTTP_ACCESS_TYPE_NO_PROXY,
                                            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)};
        if (!session.value || !WinHttpSetTimeouts(session.value, 2000, 2000, 4000, 42000))
            throw std::runtime_error("HTTP session unavailable");
        const HttpHandle connection{WinHttpConnect(session.value, L"127.0.0.1", port_, 0)};
        if (!connection.value) throw std::runtime_error("HTTP connection unavailable");
        const HttpHandle request{WinHttpOpenRequest(connection.value, payload ? L"POST" : L"GET", path.c_str(),
                                                   nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0)};
        DWORD redirects = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
        if (!request.value ||
            !WinHttpSetOption(request.value, WINHTTP_OPTION_REDIRECT_POLICY, &redirects, sizeof(redirects)) ||
            !WinHttpSendRequest(request.value, headers_.c_str(), static_cast<DWORD>(headers_.size()),
                                body.data(), bytes, bytes, 0) ||
            !WinHttpReceiveResponse(request.value, nullptr))
            throw std::runtime_error("Ghost API is unavailable");

        DWORD status = 0;
        DWORD size = sizeof(status);
        if (!WinHttpQueryHeaders(request.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                 WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX))
            throw std::runtime_error("missing Ghost API response status");
        if (status == 401 || status == 403) return Error("permission_denied");
        if (status == 429) return Error("rate_limited");
        if (status != 200) throw std::runtime_error("unexpected Ghost API response status");
        std::string response;
        char chunk[4096];
        for (;;) {
            DWORD read = 0;
            if (!WinHttpReadData(request.value, chunk, sizeof(chunk), &read) ||
                response.size() + read > 2 * 1024 * 1024)
                throw std::runtime_error("invalid Ghost API response size");
            if (!read) break;
            response.append(chunk, read);
        }
        return json::parse(response);
    }

private:
    INTERNET_PORT port_;
    std::wstring headers_;
};

bool EqualPath(const fs::path& left, const fs::path& right) {
    return CompareStringOrdinal(left.c_str(), -1, right.c_str(), -1, TRUE) == CSTR_EQUAL;
}

bool IsControl(unsigned char c) { return c < 32 || c == 127; }

fs::path LocalPath(const std::string& input) {
    if (input.empty() || input.size() > 3000 || input.find_first_of("\"<>|?*") != std::string::npos ||
        std::any_of(input.begin(), input.end(), IsControl))
        return {};
    fs::path path(StringToWChar(input));
    path.make_preferred();
    const auto drive = path.root_name().native();
    if (drive.size() != 2 || drive[1] != L':' || !path.is_absolute() ||
        path.relative_path().native().find(L':') != std::wstring::npos)
        return {};
    for (const auto& component : path.relative_path()) {
        const auto& name = component.native();
        if (!name.empty() && name != L"." && name != L".." && (name.back() == L' ' || name.back() == L'.'))
            return {};
    }
    path = path.lexically_normal();
    if (!path.has_filename()) path = path.parent_path();
    return path;
}

fs::path ComparablePath(const fs::path& path) {
    if (path.empty()) return {};
    std::error_code error;
    auto canonical = fs::weakly_canonical(path, error);
    return error ? path : canonical;
}

std::string DataDirectory(const json& target) {
    const std::wstring command = L"chrome.exe " + StringToWChar(target.at("args").get<std::string>());
    int count = 0;
    const auto argv = CommandLineToArgvW(command.c_str(), &count);
    if (!argv) return {};
    std::string directory;
    if (count == 2) {
        const std::string arg = WCharToString(argv[1]);
        if (arg.compare(0, kDataDirFlag.size(), kDataDirFlag) == 0)
            directory = arg.substr(kDataDirFlag.size());
    }
    LocalFree(argv);
    return directory;
}

std::string DefaultChromePath() {
    for (const auto variable : {L"ProgramFiles", L"ProgramFiles(x86)", L"LOCALAPPDATA"}) {
        wchar_t base[32768];
        const DWORD size = GetEnvironmentVariableW(variable, base, static_cast<DWORD>(std::size(base)));
        if (!size || size >= std::size(base)) continue;
        const auto path = fs::path(base) / LR"(Google\Chrome\Application\chrome.exe)";
        std::error_code error;
        if (fs::is_regular_file(path, error)) return WCharToString(path.c_str());
    }
    return {};
}

json ListProfiles(const GhostApi& api) {
    const auto targetResult = api.Call(L"targets/list");
    if (!Ok(targetResult)) return targetResult;
    const auto nodeResult = api.Call(L"upstream/list");
    if (!Ok(nodeResult)) return nodeResult;
    json profiles = json::array();
    for (const auto& target : targetResult.at("targets")) {
        const auto directory = DataDirectory(target);
        if (directory.empty()) continue;
        profiles.push_back({{"targetId", target.at("targetId")}, {"name", target.at("name")},
                            {"dataDir", directory}, {"status", target.at("status")},
                            {"proxyNodeId", target.at("proxyNodeId")}});
    }
    json nodes = json::array();
    for (const auto& node : nodeResult.at("nodes")) {
        const auto id = node.at("id").get<std::string>();
        if (!id.empty() && id != "direct" && id != "default")
            nodes.push_back({{"id", id}, {"name", node.at("name")}});
    }
    return {{"status", "ok"}, {"profiles", profiles}, {"nodes", nodes}, {"chromePath", DefaultChromePath()}};
}

json ChangeProfile(const GhostApi& api, const json& request, bool create) {
    const auto name = request.at("name").get<std::string>();
    if (name.size() > 240 || name.find_first_not_of(" \t") == std::string::npos ||
        std::any_of(name.begin(), name.end(), IsControl))
        return Error("invalid_name");
    const auto directory = LocalPath(request.at("dataDir").get<std::string>());
    std::error_code error;
    if (!directory.has_relative_path() || fs::is_regular_file(directory, error))
        return Error("invalid_data_dir");
    const auto node = request.at("proxyNodeId").get<std::string>();
    if (node.empty() || node == "default") return Error("select_node");
    const auto selectedId = create ? std::string{} : request.at("targetId").get<std::string>();

    json body = {{"name", name}, {"proxyNodeId", node}, {"dnsMode", node == "direct" ? "system" : "dot"},
                 {"args", json::array({std::string(kDataDirFlag) + WCharToString(directory.c_str())})}};
    if (create) {
        const auto chrome = LocalPath(request.at("chromePath").get<std::string>());
        if (!EqualPath(chrome.filename(), L"chrome.exe") || !fs::is_regular_file(chrome, error))
            return Error("invalid_chrome_path");
        body["exePath"] = WCharToString(chrome.c_str());
    } else {
        body["targetId"] = selectedId;
    }

    const auto result = api.Call(L"targets/list");
    if (!Ok(result)) return result;
    bool found = false;
    const auto destination = ComparablePath(directory);
    for (const auto& target : result.at("targets")) {
        const auto existing = DataDirectory(target);
        if (existing.empty()) continue;
        if (!create && target.at("targetId") == selectedId) {
            found = true;
            if (target.at("status") == "running") return Error("target_running");
        } else if (EqualPath(ComparablePath(LocalPath(existing)), destination)) {
            return Error("duplicate_data_dir");
        }
    }
    if (!create && !found) return Error("target_not_found");
    return api.Call(create ? L"targets/create" : L"targets/update", &body);
}

json ControlProfile(const GhostApi& api, const json& request, bool launch) {
    const auto id = request.at("targetId").get<std::string>();
    const auto result = api.Call(L"targets/list");
    if (!Ok(result)) return result;
    for (const auto& target : result.at("targets")) {
        if (target.at("targetId") != id || DataDirectory(target).empty()) continue;
        const bool running = target.at("status") == "running";
        if (launch && running) return Error("target_running");
        if (!launch && !running) return {{"status", "ok"}};
        const json body = {{"targetId", id}};
        return api.Call(launch ? L"targets/launch" : L"targets/stop", &body);
    }
    return Error("target_not_found");
}

json Dispatch(const GhostApi& api, const json& request) {
    const auto action = request.at("action").get<std::string>();
    if (action == "list") return ListProfiles(api);
    if (action == "create" || action == "update") return ChangeProfile(api, request, action == "create");
    if (action == "launch" || action == "stop") return ControlProfile(api, request, action == "launch");
    return Error("invalid_request");
}
}  // namespace

int main() {
    if (!GetEnvironmentVariableW(L"GHOST_PLUGIN_ID", nullptr, 0)) return 1;
    try {
        std::string line;
        if (!std::getline(std::cin, line) || line.size() > 65536)
            throw std::runtime_error("missing plugin handshake");
        const auto hello = json::parse(line);
        if (hello.at("v") != 1 || hello.at("pluginId") != "com.ghostproxifier.chrome-launcher")
            throw std::runtime_error("invalid plugin handshake");
        const auto origin = hello.at("apiBase").get<std::string>();
        const GhostApi api(LocalPort(origin), hello.at("token").get<std::string>());
        const auto stopName = StringToWChar(hello.at("stopEvent").get<std::string>());
        struct StopEvent {
            HANDLE value;
            ~StopEvent() { if (value) CloseHandle(value); }
        } stop{OpenEventW(SYNCHRONIZE, FALSE, stopName.c_str())};
        if (!stop.value) throw std::runtime_error("missing plugin stop event");
        WSADATA wsa{};
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) throw std::runtime_error("Winsock startup failed");
        const fs::path pluginDir(StringToWChar(hello.at("pluginDir").get<std::string>()));
        const LoopbackUi ui(pluginDir / "ui", origin + " 'self'");
        std::thread stopper([event = stop.value] {
            // A pending host approval can block the request thread for 30 seconds.
            if (WaitForSingleObject(event, INFINITE) == WAIT_OBJECT_0) ExitProcess(0);
        });
        stopper.detach();
        std::cout << json({{"v", 1}, {"ok", true}, {"uiUrl", ui.Url()}}).dump() << std::endl;
        ui.Serve([&](const json& request) { return Dispatch(api, request); });
        return 1;
    } catch (const std::exception& error) {
        std::cout << json({{"v", 1}, {"ok", false}, {"error", error.what()}}).dump(
            -1, ' ', false, json::error_handler_t::replace) << std::endl;
        return 1;
    }
}
