#include "camera_sampling.hpp"
#include <iostream>
#include <limits>
#include <stdexcept>

int main() {
    using namespace x4vr::observe;
    unsigned checks{};
    auto require = [&](bool ok) { if (!ok) throw std::runtime_error("sample check " + std::to_string(checks)); ++checks; };
    try {
        std::array<std::byte, 576> data{};
        require(!camera_signature(data.data(), data.size()));
        std::array<float, 32> matrices{};
        matrices[0] = matrices[5] = matrices[10] = matrices[15] = 1;
        matrices[16] = matrices[21] = 1;
        std::memcpy(data.data(), matrices.data(), sizeof(matrices));
        const auto signature = camera_signature(data.data(), data.size());
        require(signature.has_value());
        require(!camera_signature(data.data(), 64));
        matrices[0] = std::numeric_limits<float>::quiet_NaN();
        std::memcpy(data.data(), matrices.data(), sizeof(matrices));
        require(!camera_signature(data.data(), data.size()));
        CameraSampling gate;
        require(gate.attempt(0));
        require(gate.accept(0, *signature) == 0u);
        require(!gate.accept(0, *signature));
        for (unsigned i = 1; i < 16; ++i) { require(gate.attempt(0)); require(gate.accept(0, i).has_value()); }
        require(!gate.attempt(0));
        require(gate.attempt(120));
        require(!gate.accept(0, 99)); // stale concurrent sample after epoch change
        require(gate.accept(120, *signature) == 16u);
        CameraSampling empty;
        for (unsigned i = 0; i < 512; ++i) require(empty.attempt(0));
        require(!empty.attempt(0)); require(empty.attempt(120));
        CameraSampling limited;
        for (unsigned i = 0; i < 4096; ++i) {
            const auto frame = (i/16)*120;
            require(limited.attempt(frame)); require(limited.accept(frame, i).has_value());
        }
        require(!limited.attempt(999999));
        std::cout << checks << " camera sampling checks passed\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
