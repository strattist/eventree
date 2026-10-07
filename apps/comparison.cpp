/**
 * @file comparison.cpp
 * @brief Comparison app: decodes every recording of a directory with the reference decoder (openeb) and with
 * eventree, checks that both produce the same CD events, and reports how much smaller the trees are.
 *
 * Usage: `comparison [-d <datasets dir>]` (default: `datasets`)
 *
 * The wire format of each file is read from its RAW header. Files that are not recordings are ignored with a
 * message. EVT2 and EVT3 files can be mixed in the same directory.
 *
 * Exit codes:
 * - 0: every decoded file matched the reference decoder
 * - 1: a file failed to decode or its events differ from the reference decoder's (the first mismatch is printed)
 * - 2: the directory does not exist
 */

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <tuple>
#include <vector>
#include <CLI/CLI.hpp>
#include <metavision/sdk/base/events/event_cd.h>
#include <metavision/sdk/stream/camera.h>

#include <eventree/evt2_decoder.hpp>
#include <eventree/evt3_decoder.hpp>

namespace fs = std::filesystem;

namespace {

enum class WireFormat { evt2, evt3, other };

struct RawHeader {
  WireFormat format = WireFormat::other;
  std::size_t data_offset = 0;  // bytes of header before the event data
};

/**
 * @brief Reads the RAW header of a file: the leading lines starting with '%'.
 *
 * The format comes from the `% evt 2.0` / `% evt 3.0` line or from `% format EVT2` / `% format EVT3[;...]`.
 *
 * @param path File to read.
 * @return The header, or nothing if the file does not start with '%' (not a recording) or cannot be read.
 */
std::optional<RawHeader> read_header(const fs::path &path) {
  std::ifstream in(path, std::ios::binary);
  if (in.peek() != '%') return std::nullopt;

  RawHeader header;
  std::string line;
  while (in.peek() == '%' && std::getline(in, line)) {
    if (line.rfind("% evt 2.0", 0) == 0 || line.rfind("% format EVT2", 0) == 0) header.format = WireFormat::evt2;
    if (line.rfind("% evt 3.0", 0) == 0 || line.rfind("% format EVT3", 0) == 0) header.format = WireFormat::evt3;
    header.data_offset = static_cast<std::size_t>(in.tellg());
  }
  return header;
}

struct Event {
  std::int64_t t;
  std::uint16_t y;
  std::uint16_t x;
  std::uint8_t p;
  friend auto operator<=>(const Event &, const Event &) = default;
};

/// Decodes a file with openeb into a vector of events.
std::vector<Event> decode_reference(const fs::path &path) {
  Metavision::FileConfigHints hints;
  hints.real_time_playback(false);
  hints.time_shift(false);  // keep absolute timestamps, as eventree does
  auto camera = Metavision::Camera::from_file(path.string(), hints);

  std::vector<Event> events;
  camera.cd().add_callback([&](const Metavision::EventCD *begin, const Metavision::EventCD *end) {
    for (auto it = begin; it != end; ++it)
      events.push_back({it->t, static_cast<std::uint16_t>(it->y), static_cast<std::uint16_t>(it->x),
                        static_cast<std::uint8_t>(it->p)});
  });
  camera.start();
  while (camera.is_running()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
  camera.stop();
  return events;
}

/// Decodes the event data of a file with eventree's decoder `Decoder` into an event tree, read in 1 MiB chunks.
template <typename Decoder>
eventree::EventTree<> decode_eventree(const fs::path &path, std::size_t data_offset) {
  std::ifstream in(path, std::ios::binary);
  in.seekg(static_cast<std::streamoff>(data_offset));
  eventree::EventTree<> tree;
  Decoder decoder;
  std::vector<std::byte> buffer(1 << 20);
  while (in.read(reinterpret_cast<char *>(buffer.data()), static_cast<std::streamsize>(buffer.size())) || in.gcount() > 0)
    decoder.decode(std::span<const std::byte>(buffer.data(), static_cast<std::size_t>(in.gcount())), tree);
  return tree;
}

std::ostream &operator<<(std::ostream &os, const Event &e) {
  return os << "t=" << e.t << " x=" << e.x << " y=" << e.y << " p=" << int(e.p);
}

/// Prints the first difference between two sorted event sets; returns false if there is one.
bool same_events(const std::vector<Event> &reference, const std::vector<Event> &ours) {
  const auto [r, o] = std::mismatch(reference.begin(), reference.end(), ours.begin(), ours.end());
  if (r == reference.end() && o == ours.end()) return true;
  std::cerr << "  MISMATCH at sorted index " << (r - reference.begin()) << ": reference ";
  if (r == reference.end()) std::cerr << "has no more events"; else std::cerr << *r;
  std::cerr << ", eventree ";
  if (o == ours.end()) std::cerr << "has no more events"; else std::cerr << *o;
  std::cerr << " (counts: reference " << reference.size() << ", eventree " << ours.size() << ")\n";
  return false;
}

/// Compares one file decoded with `Decoder`; returns true if it matches the reference decoder.
template <typename Decoder>
bool compare(const fs::path &path, const RawHeader &header) {
  auto reference = decode_reference(path);
  const auto tree = decode_eventree<Decoder>(path, header.data_offset);

  std::vector<Event> ours;
  ours.reserve(tree.size());
  for (const auto &e : tree) ours.push_back({e.t, e.y, e.x, e.polarity});

  std::sort(reference.begin(), reference.end());
  std::sort(ours.begin(), ours.end());
  if (!same_events(reference, ours)) return false;

  const double n = static_cast<double>(tree.size());
  const double tree_bytes = n > 0 ? static_cast<double>(tree.memory_use()) / n : 0;
  const double cd_bytes = sizeof(Metavision::EventCD);
  std::cout << "  OK  events " << tree.size() << "  tree " << std::fixed << std::setprecision(2) << tree_bytes
            << " B/event  EventCD " << cd_bytes << " B/event  ratio " << (n > 0 ? tree_bytes / cd_bytes : 0) << "\n";
  return true;
}

}  // namespace

int main(int argc, char **argv) {
  CLI::App app{"comparison: check eventree against openeb on every recording of a directory"};
  std::string directory = "datasets";
  app.add_option("-d,--directory", directory, "Directory holding the recordings")->capture_default_str();
  CLI11_PARSE(app, argc, argv);

  if (!fs::is_directory(directory)) {
    std::cerr << "error: " << directory << " is not a directory\n";
    return 2;
  }

  std::vector<fs::path> files;
  for (const auto &entry : fs::recursive_directory_iterator(directory))
    if (entry.is_regular_file() && entry.path().extension() != ".tmp_index") files.push_back(entry.path());
  std::sort(files.begin(), files.end());

  bool all_match = true;
  for (const auto &path : files) {
    std::cout << path.string() << "\n";
    const auto header = read_header(path);
    if (!header) {
      std::cout << "  ignored: not a RAW recording\n";
      continue;
    }
    try {
      switch (header->format) {
        case WireFormat::evt2:
          std::cout << "  EVT2\n";
          all_match &= compare<eventree::Evt2Decoder>(path, *header);
          break;
        case WireFormat::evt3:
          std::cout << "  EVT3\n";
          all_match &= compare<eventree::Evt3Decoder>(path, *header);
          break;
        default:
          std::cout << "  ignored: unsupported wire format\n";
      }
    } catch (const std::exception &e) {
      std::cerr << "  error: " << e.what() << "\n";
      all_match = false;
    }
  }
  return all_match ? 0 : 1;
}
