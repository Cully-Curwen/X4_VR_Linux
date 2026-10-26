// For tests that run the real `x4vr` command as players run it: in a fake home directory, with
// none of the caller's environment, so the player's own settings and X4 install are never seen.

#pragma once

#include <sys/wait.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

inline int failures = 0;
inline void check(bool ok, const std::string& what) { if (!ok) { std::printf("FAIL: %s\n", what.c_str()); ++failures; } }

inline std::string read_file(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

inline bool contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

// What one run printed (stdout and stderr together) and its exit code.
struct Run {
    std::string output;
    int exit_code = -1;
};

// Runs `argv` with only `env` as its environment, waiting for it.
inline Run run_program(const std::vector<std::string>& argv, const std::vector<std::string>& env) {
    int out[2];
    if (pipe(out) != 0) return {};

    const pid_t pid = fork();
    if (pid == 0) {
        dup2(out[1], STDOUT_FILENO);
        dup2(out[1], STDERR_FILENO);
        close(out[0]);
        close(out[1]);
        std::vector<char*> args, vars;
        for (const std::string& arg : argv) args.push_back(const_cast<char*>(arg.c_str()));
        for (const std::string& var : env) vars.push_back(const_cast<char*>(var.c_str()));
        args.push_back(nullptr);
        vars.push_back(nullptr);
        execve(args[0], args.data(), vars.data());
        _exit(127);
    }
    close(out[1]);

    Run run;
    char buffer[4096];
    ssize_t n;
    while ((n = read(out[0], buffer, sizeof buffer)) > 0) run.output.append(buffer, size_t(n));
    close(out[0]);

    int status = 0;
    waitpid(pid, &status, 0);
    if (WIFEXITED(status)) run.exit_code = WEXITSTATUS(status);
    return run;
}

// The caller's PATH, so x4vr finds the programs it runs (tar for the bug report).
inline std::string get_path_var() {
    const char* path = std::getenv("PATH");
    return std::string("PATH=") + (path ? path : "/usr/bin:/bin");
}

// Runs `x4vr <args>` with HOME=`home`, SteamVR not asked and X4's resolution fixed at 1920x1080.
inline Run run_x4vr(const char* x4vr, const std::filesystem::path& home, const std::vector<std::string>& args) {
    std::vector<std::string> argv{x4vr};
    argv.insert(argv.end(), args.begin(), args.end());
    return run_program(argv, {"HOME=" + home.string(), get_path_var(), "X4VR_NO_STEAMVR_QUERY=1", "X4VR_RESOLUTION=1920x1080"});
}
