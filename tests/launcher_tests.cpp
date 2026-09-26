// Launcher logic: profile merge into stereo.txt, X4 config.xml checks and fixes, stats parse.
#include "../tools/launcher/launcher_settings.hpp"
#include <cstdio>
#include <cstdlib>

using namespace x4vr::launcher;

#define CHECK(condition) do { if (!(condition)) { std::printf("FAILED %s:%d %s\n", __FILE__, __LINE__, #condition); std::exit(1); } } while (0)

int main() {
    // Profiles overlay only their keys; calibration and the live recenter counter survive.
    const auto defaults = parse_settings("stereo=1\r\ndelay=2\n# comment\nrecenter=0\npos_scale=3.6\n");
    const auto profile = parse_settings("pair=1\nipd_scale=1.2\nx4_width=2560\nx4_height=1440\n");
    const auto live = compose_live(defaults, profile, "7");
    CHECK(get(live, "delay") == "2" && get(live, "pos_scale") == "3.6");
    CHECK(get(live, "pair") == "1" && get(live, "ipd_scale") == "1.2");
    CHECK(get(live, "recenter") == "7");
    CHECK(get(live, "x4_width").empty()); // launcher-only keys stay out of stereo.txt
    CHECK(parse_settings(format_settings(live)) == live);
    CHECK(get(profile_of(live), "delay").empty() && get(profile_of(live), "pair") == "1");

    // X4 config: flag what VR needs, fix exactly that, keep everything else.
    const std::string xml = "<?xml version=\"1.0\"?>\n<root>\n  <fullscreen>false</fullscreen>\n  <borderless>false</borderless>\n"
        "  <antialiasing>taa_high</antialiasing>\n  <fov>1.0000</fov>\n  <dlss>false</dlss>\n  <dlssmode>off</dlssmode>\n"
        "  <dlssg>off</dlssg>\n  <fsr3g>off</fsr3g>\n  <upmode>none</upmode>\n  <presentmode>immediate</presentmode>\n"
        "  <frameratelimit>true</frameratelimit>\n  <frameratetarget>60</frameratetarget>\n  <enableopentrack>true</enableopentrack>\n"
        "  <chromaticaberration>false</chromaticaberration>\n  <distortion>false</distortion>\n  <res_width>1920</res_width>\n"
        "  <res_height>1080</res_height>\n  <gamma>1.00</gamma>\n</root>\n";
    auto failing = [](const std::vector<Check>& checks) { int n = 0; for (const auto& c : checks) n += !c.ok; return n; };
    const auto checks = check_x4(xml, 2560, 1440);
    CHECK(failing(checks) == 5); // fullscreen, AA, FOV, frame limit, resolution
    const auto fixed = fix_x4(xml, checks);
    CHECK(failing(check_x4(fixed, 2560, 1440)) == 0);
    std::string v;
    CHECK(xml_value(fixed, "gamma", v) && v == "1.00");
    CHECK(xml_value(fixed, "res_width", v) && v == "2560");
    CHECK(failing(check_x4(fixed, 0, 0)) == 0); // resolution unchecked when the profile has none
    const auto inserted = xml_set("<root>\n</root>\n", "enableopentrack", "true");
    CHECK(xml_value(inserted, "enableopentrack", v) && v == "true");

    Stats stats{};
    CHECK(parse_stats_line("29994546 180 94 93 2 0 5 0.21 0.29 11.64 0.00 11.41 | 1 0 1 0 1", stats));
    CHECK(stats.fps == 90 && stats.late == 2 && stats.repeated == 5);
    CHECK(!parse_stats_line("# tick_ms submits", stats));
    std::printf("launcher logic ok\n");
}
