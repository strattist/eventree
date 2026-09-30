/**
 * @file reference_decoder.cpp
 * @brief Reference decoder: decodes a RAW recording with the installed openeb.
 *
 * Prints the wire format (EVT2, EVT3, ...), the number of CD events and an
 * order-independent checksum. Later tickets compare the eventree decoders
 * against these numbers.
 *
 * Usage: `reference_decoder -i <file.raw>`
 *
 * Exit codes:
 * - 0: decoded successfully
 * - 1: openeb failed to open or decode the file
 * - 2: the file is not a RAW recording
 */

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

/**
 * @brief Cheap check that a file is a RAW recording and not, for example, a web page.
 *
 * RAW recordings start with a header of lines beginning with '%'.
 *
 * @param path Path of the file to test.
 * @return true if the first byte is '%'; false if it differs or the file cannot be read.
 */
bool looks_like_raw_recording(const std::string &path) {
    std::ifstream in(path, std::ios::binary);
    char first = 0;
    return in.get(first) && first == '%';
}

/**
 * @brief splitmix64 finaliser, used to hash one packed event key.
 *
 * It has good avalanche, so summing the hashes of all events gives an
 * order-independent checksum of the multiset of events.
 *
 * @param z Packed event key (see main()).
 * @return 64-bit hash of @p z.
 */
std::uint64_t mix(std::uint64_t z) {
    z += 0x9e3779b97f4a7c15ULL;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

} // namespace

/**
 * @brief Entry point.
 *
 * Each CD event is packed into a 64-bit key, hashed with mix(), and the
 * hashes are summed (with wraparound) into the checksum:
 *
 * | bits  | field | width | notes                  |
 * |-------|-------|-------|------------------------|
 * | 0     | p     | 1     | polarity               |
 * | 1     | -     | 1     | unused, always 0       |
 * | 2-12  | x     | 11    | masked with 0x7ff      |
 * | 13-23 | y     | 11    | masked with 0x7ff      |
 * | 24-63 | t     | 40    | timestamp in µs        |
 *
 * @param argc Argument count.
 * @param argv Arguments; `-i/--input` names the RAW file.
 * @return 0 on success, 1 on decode error, 2 if the input is not a RAW recording.
 */
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
