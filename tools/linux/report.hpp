// The bug report: logs, settings, X4's config.xml and a summary of the system in one archive.

#pragma once

#include <string>

namespace x4vr::linux_launcher {

// The commit this x4vr was built from and the ones before it (build_info.h, made at build time):
// `x4vr version`, and build.txt in the report.
std::string make_build_text();

// What make_report() made: the archive's path, or empty with why it couldn't be made.
struct Report {
    std::string file;
    std::string error;
};

// The bug report, ~/x4vr-report-<time>.tar.gz, readable only by the player (it holds their logs and
// X4's config.xml). Like the Windows launcher's report_bug() without its dialog: the caller says
// where to send it.
Report make_report();

} // namespace x4vr::linux_launcher
