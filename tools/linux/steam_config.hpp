#pragma once
// X4's Steam launch option, read from Steam's per-user localconfig.vdf (userdata/<id>/config/):
// UserLocalConfigStore > Software > Valve > Steam > apps > 392160 > "LaunchOptions". Only read:
// the user sets it in Steam (X4 > Properties > Launch options); x4vr shows and copies the line.
// VDF: quoted strings (escapes \" and \\), braces for blocks, keys case-insensitive.
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace x4vr::steam {
inline constexpr std::string_view x4_app = "392160";

struct Token { enum Kind { String, Open, Close } kind; size_t begin, end; std::string text; }; // [begin, end) in the file
inline std::vector<Token> tokenize(const std::string& vdf) {
    std::vector<Token> tokens;
    for (size_t i = 0; i < vdf.size();) {
        const char c = vdf[i];
        if (c == '{' || c == '}') { tokens.push_back({c == '{' ? Token::Open : Token::Close, i, i+1, {}}); ++i; }
        else if (c == '"') {
            Token t{Token::String, i, 0, {}};
            for (++i; i < vdf.size() && vdf[i] != '"'; ++i) {
                if (vdf[i] == '\\' && i+1 < vdf.size()) ++i;
                t.text += vdf[i];
            }
            t.end = std::min(i+1, vdf.size());
            i = t.end;
            tokens.push_back(std::move(t));
        } else if (c == '/' && i+1 < vdf.size() && vdf[i+1] == '/') { while (i < vdf.size() && vdf[i] != '\n') ++i; }
        else ++i;
    }
    return tokens;
}
inline bool same_key(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return std::tolower(x) == std::tolower(y); });
}

// Where X4's app block and its LaunchOptions value are in one localconfig.vdf.
struct AppBlock {
    bool found = false;
    size_t open = 0;                 // position of the app block's '{'
    std::optional<size_t> value;     // index of the LaunchOptions value token
};
inline AppBlock find_app(const std::vector<Token>& tokens, std::string_view app = x4_app) {
    static const std::vector<std::string_view> path{"UserLocalConfigStore", "Software", "Valve", "Steam", "apps"};
    std::vector<std::string> stack; // keys of the open blocks
    AppBlock result;
    for (size_t i = 0; i < tokens.size(); ++i) {
        const auto& t = tokens[i];
        if (t.kind == Token::Close) { if (!stack.empty()) stack.pop_back(); continue; }
        if (t.kind != Token::String) continue;
        if (i+1 < tokens.size() && tokens[i+1].kind == Token::Open) { // a block
            stack.push_back(t.text);
            const bool app_block = stack.size() == path.size()+1 && same_key(stack.back(), app) &&
                                   std::equal(path.begin(), path.end(), stack.begin(), [](auto a, const std::string& b) { return same_key(a, b); });
            if (app_block) { result = {true, tokens[i+1].begin, std::nullopt}; }
            ++i;
            continue;
        }
        // a key and its value
        if (i+1 < tokens.size() && tokens[i+1].kind == Token::String) {
            if (result.found && !result.value && stack.size() == path.size()+1 && same_key(stack.back(), app) && same_key(t.text, "LaunchOptions"))
                result.value = i+1;
            ++i;
        }
    }
    return result;
}
inline std::optional<std::string> launch_options(const std::string& vdf, std::string_view app = x4_app) {
    const auto tokens = tokenize(vdf);
    const auto block = find_app(tokens, app);
    if (!block.found) return std::nullopt;
    return block.value ? tokens[*block.value].text : std::string();
}
// Every Steam account's localconfig.vdf on this machine.
inline std::vector<std::filesystem::path> local_configs() {
    const char* home = std::getenv("HOME");
    const std::filesystem::path h = home ? home : ".";
    std::vector<std::filesystem::path> found;
    for (const auto& root : {h/".local/share/Steam", h/".steam/steam", h/".steam/root"}) {
        std::error_code error;
        const auto canonical = std::filesystem::canonical(root/"userdata", error);
        if (error) continue;
        for (const auto& user : std::filesystem::directory_iterator(canonical, error)) {
            const auto file = user.path()/"config/localconfig.vdf";
            if (std::filesystem::exists(file) && std::find(found.begin(), found.end(), file) == found.end()) found.push_back(file);
        }
    }
    return found;
}
inline std::string read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}
// Steam's client is running.
inline bool steam_running() {
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator("/proc", error)) {
        std::ifstream comm(entry.path()/"comm");
        std::string name;
        if (comm && std::getline(comm, name) && (name == "steam" || name == "steamwebhelper")) return true;
    }
    return false;
}
}
