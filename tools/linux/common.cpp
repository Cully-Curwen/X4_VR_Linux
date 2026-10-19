#include "common.hpp"
#include "steam_config.hpp"
#include <fcntl.h>
#include <filesystem>
#include <sys/wait.h>
#include <unistd.h>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <system_error>

namespace x4vr::linux_launcher {

std::filesystem::path program;

std::filesystem::path get_settings_file() {
    if (const char* dir = std::getenv("X4VR_DIR"); dir && *dir) return std::filesystem::path(dir)/"stereo.txt";
    if (const char* state = std::getenv("XDG_STATE_HOME"); state && *state) return std::filesystem::path(state)/"x4vr/stereo.txt";
    const char* home = std::getenv("HOME");
    return std::filesystem::path(home ? home : ".")/".local/state/x4vr/stereo.txt";
}

std::filesystem::path get_state_dir() { return get_settings_file().parent_path(); }

std::filesystem::path get_settings_marker() { return get_state_dir()/"x4-settings.vr"; }

std::string read_text(const std::filesystem::path& path) { return x4vr::steam::read_file(path); }

bool write_text(const std::filesystem::path& path, const std::string& text) {
    const std::filesystem::path temporary = path.string()+".x4vr-tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        out << text;
        if (!out) return false;
    }
    std::error_code error;
    std::filesystem::rename(temporary, path, error);
    return !error;
}

std::string quote_path(const std::filesystem::path& path) {
    const std::string text = path.string();
    return text.find(' ') == std::string::npos ? text : "\""+text+"\"";
}

bool run(const std::vector<std::string>& argv, const Run& options) {
    // The file descriptors: [0] becomes the child's standard input, [1] is where the input is written.
    int fds[2]{-1, -1}; // -1 is POSIX's "no file descriptor": both stay -1 when there is no input, and no pipe is made.
    if (options.input && pipe(fds) != 0) return false;

    // The child: its own session if detached, standard input and output set up, then the program.
    const pid_t child = fork();
    if (child == 0) {
        if (options.detach) { setsid(); if (fork() != 0) _exit(0); }
        const int null = ::open("/dev/null", O_RDWR | O_CLOEXEC);
        const int in = options.input ? fds[0] : null;
        const int out = options.log.empty() ? null : ::open(options.log.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
        dup2(in, 0); dup2(out, 1); dup2(out, 2);
        if (options.input) { close(fds[0]); close(fds[1]); }

        std::vector<char*> args;
        for (const std::string& a : argv) args.push_back(const_cast<char*>(a.c_str()));
        args.push_back(nullptr);
        execvp(args[0], args.data());
        _exit(127); // as the shell does; command not found
    }

    // The input, written all at once.
    if (options.input) {
        close(fds[0]);
        if (child > 0) {  // the fork worked: there is a child to read it
            // A program that exited already (not installed) leaves no reader: writing then raises
            // SIGPIPE, which would kill x4vr. Ignored for this write, so it just fails.
            struct sigaction ignore{}, previous{};
            ignore.sa_handler = SIG_IGN;
            sigaction(SIGPIPE, &ignore, &previous);
            (void)!::write(fds[1], options.input->data(), options.input->size());
            sigaction(SIGPIPE, &previous, nullptr);
        }
        close(fds[1]);
    }

    if (child < 0) return false;
    int status = 0;
    waitpid(child, &status, 0);
    return options.detach || (WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

} // namespace x4vr::linux_launcher
