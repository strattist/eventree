#include <chrono>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <CLI/CLI.hpp>
#include <metavision/sdk/base/events/event_cd.h>
#include <metavision/sdk/stream/camera.h>

using namespace Metavision;

namespace {

// RAW recordings start with a header of lines beginning with '%'.
bool looks_like_raw_recording(const std::string &path) {
    std::ifstream in(path, std::ios::binary);
    char first = 0;
    return in.get(first) && first == '%';
}

// splitmix64 finaliser: good avalanche, so summing the hashes is an
// order-independent checksum of the multiset of events.
std::uint64_t mix(std::uint64_t z) {
    z += 0x9e3779b97f4a7c15ULL;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

} // namespace

int main(int argc, char **argv) {
    CLI::App app{"reference_decoder: decode a RAW file with openeb, print CD count and checksum"};

    std::string input_file;
    app.add_option("-i,--input", input_file, "Input raw file")->required();
    CLI11_PARSE(app, argc, argv);

    if (!looks_like_raw_recording(input_file)) {
        std::cerr << "error: " << input_file << " is not a RAW recording" << std::endl;
        return 2;
    }

    try {
        FileConfigHints hints;
        hints.real_time_playback(false);
        Camera camera = Camera::from_file(input_file, hints);

        std::uint64_t count = 0;
        std::uint64_t checksum = 0;
        camera.cd().add_callback([&](const EventCD *begin, const EventCD *end) {
            for (auto it = begin; it != end; ++it) {
                const std::uint64_t key = (static_cast<std::uint64_t>(it->t) << 24) |
                                          (static_cast<std::uint64_t>(it->y & 0x7ff) << 13) |
                                          (static_cast<std::uint64_t>(it->x & 0x7ff) << 2) |
                                          static_cast<std::uint64_t>(it->p & 1);
                checksum += mix(key);
            }
            count += static_cast<std::uint64_t>(end - begin);
        });

        const auto &config = camera.get_camera_configuration();
        std::cout << "Data encoding format: " << config.data_encoding_format << std::endl;

        camera.start();
        while (camera.is_running()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        camera.stop();

        std::cout << "CD events           : " << count << std::endl;
        std::cout << "Checksum            : 0x" << std::hex << checksum << std::dec << std::endl;
    } catch (const std::exception &e) {
        std::cerr << "error: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}
