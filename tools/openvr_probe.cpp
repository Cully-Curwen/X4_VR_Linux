#include <x4vr/session.hpp>
#include <x4vr/runtime_bootstrap.hpp>
#include <chrono>
#include <iostream>
#include <string_view>

int main(int argc, char** argv) {
    try {
        if (argc > 2 || (argc == 2 && std::string_view(argv[1]) != "--scene" && std::string_view(argv[1]) != "--tracking")) {
            std::cerr << "Usage: openvr_probe [--scene|--tracking]\n"
                         "Default: installation/HMD presence check, no scene initialization.\n"
                         "--scene: initialize SteamVR, query stereo transforms; renders no game.\n"
                         "--tracking: initialize shared runtime, query unpaced current poses; no frame/submission.\n";
            return 2;
        }
        std::cout << "runtime_installed=" << vr::VR_IsRuntimeInstalled() << '\n';
        const bool present = vr::VR_IsHmdPresent();
        std::cout << "headset_present=" << present << '\n';
        if (argc == 1) return vr::VR_IsRuntimeInstalled() ? 0 : 3;
        if (!present) {
            std::cerr << "No headset detected. Start Varjo Base with Aero connected and OpenVR enabled.\n";
            return 4;
        }
        if (std::string_view(argv[1]) == "--tracking") {
            auto runtime = x4vr::acquire_runtime_bootstrap();
            auto second_owner = x4vr::acquire_runtime_bootstrap();
            if (runtime != second_owner) throw std::runtime_error("Runtime sharing failed");
            x4vr::Matrix head;
            auto status = x4vr::FrameStatus::tracking_unavailable;
            for (int i = 0; i < 250; ++i) {
                status = runtime->sample_tracking(head);
                if (status == x4vr::FrameStatus::ready || status == x4vr::FrameStatus::quit_requested) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            if (status != x4vr::FrameStatus::ready) {
                std::cerr << "tracking_unavailable=" << static_cast<int>(status) << '\n'; return 5;
            }
            std::cout << "shared_runtime_tracking_ready=1\nhead_position_metres=" << head.m[0][3] << ',' << head.m[1][3] << ',' << head.m[2][3] << '\n';
            return 0;
        }
        x4vr::Session session;
        session.initialize();
        std::cout << "headset_model=" << session.headset_model() << '\n';
        std::cout << "required_instance_extensions=";
        for (const auto& ext : session.instance_extensions()) std::cout << ext << ' ';
        std::cout << '\n';
        x4vr::Frame frame;
        auto status = x4vr::FrameStatus::tracking_unavailable;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        do {
            status = session.begin_frame(frame, 0.05f, 10000.f);
            if (status == x4vr::FrameStatus::ready || status == x4vr::FrameStatus::quit_requested) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        } while (std::chrono::steady_clock::now() < deadline);
        if (status != x4vr::FrameStatus::ready) {
            std::cerr << "frame_unavailable=" << static_cast<int>(status) << ' ' << session.last_error() << '\n';
            return 5;
        }
        std::cout << "per_eye_extent=" << frame.width << 'x' << frame.height << '\n';
        for (int i = 0; i < 2; ++i) {
            const auto& p = frame.eyes[i].seated_from_eye;
            std::cout << "eye_" << i << "_position_metres=" << p.m[0][3] << ',' << p.m[1][3] << ',' << p.m[2][3] << '\n';
        }
        std::cout << "Stereo pose query succeeded. X4 renderer is NOT connected.\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
