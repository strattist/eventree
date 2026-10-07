/**
 * @file benchmark.cpp
 * @brief Benchmark app: times eventree's decoders against the reference decoder (openeb) on every recording of a
 * directory.
 *
 * Usage: `benchmark [-d <datasets dir>] [-r <runs>] [-c <chunk KiB>]` (defaults: `datasets`, 5 runs, 64 KiB)
 *
 * For each recording, one block is printed with:
 * - eventree decode latency: time of one `decode` call on a chunk (median, 99th percentile and worst case over all
 *   chunks of all runs). Runs reuse a reserved tree and a reset decoder, after one untimed warm-up run.
 * - eventree throughput (M events/s and input MB/s), from the median run's total decode time.
 * - end-to-end time from file to tree, reading the file in chunks into a fresh tree (median of the runs).
 * - the reference decoder's end-to-end time and throughput, from opening the file until the camera stops
 *   (median of the runs). openeb delivers events through callbacks, so it has no per-chunk decode latency.
 *
 * A summary per wire format follows (median over the files of each format). The output is plain text, one
 * `key: value` line per number, so it can be diffed over time.
 *
 * Exit codes:
 * - 0: success
 * - 1: a file failed to decode, the decoders disagree on the event count, or no recording was found
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
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <CLI/CLI.hpp>
#include <metavision/sdk/base/events/event_cd.h>
#include <metavision/sdk/stream/camera.h>

#include <eventree/evt2_decoder.hpp>
#include <eventree/evt3_decoder.hpp>

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace {

enum class WireFormat { evt2, evt3, other };

struct RawHeader {
  WireFormat format = WireFormat::other;
  std::size_t data_offset = 0;  // bytes of header before the event data
};

/// Reads the RAW header of a file (see comparison.cpp); nothing if the file is not a recording.
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

double seconds(Clock::duration d) { return std::chrono::duration<double>(d).count(); }

double median(std::vector<double> values) {
  if (values.empty()) return 0;
  std::sort(values.begin(), values.end());
  return values[values.size() / 2];
}

double percentile(const std::vector<double> &sorted, double fraction) {
  if (sorted.empty()) return 0;
  return sorted[std::min(sorted.size() - 1, static_cast<std::size_t>(fraction * static_cast<double>(sorted.size())))];
}

struct Options {
  int runs = 5;
  std::size_t chunk_bytes = 64 << 10;
};

struct Result {
  std::size_t events = 0;
  std::size_t bytes = 0;
  double latency_median_us = 0, latency_p99_us = 0, latency_worst_us = 0;
  double mevents_per_s = 0, mb_per_s = 0;
  double end_to_end_s = 0;
  double reference_end_to_end_s = 0, reference_mevents_per_s = 0, reference_mb_per_s = 0;
};

/// Reads the event data of a file into memory.
std::vector<std::byte> read_data(const fs::path &path, std::size_t data_offset) {
  const auto size = fs::file_size(path);
  if (data_offset > size) throw std::runtime_error("header extends past the end of the file");
  std::ifstream in(path, std::ios::binary);
  std::vector<std::byte> data(size - data_offset);
  in.seekg(static_cast<std::streamoff>(data_offset));
  if (!in.read(reinterpret_cast<char *>(data.data()), static_cast<std::streamsize>(data.size())))
    throw std::runtime_error("cannot read " + path.string());
  return data;
}

/// File to tree in one go: reads the file in chunks and decodes into a fresh tree. Returns the elapsed time.
template <typename Decoder>
double end_to_end(const fs::path &path, std::size_t data_offset, const Options &options, std::size_t &events) {
  const auto start = Clock::now();
  std::ifstream in(path, std::ios::binary);
  in.seekg(static_cast<std::streamoff>(data_offset));
  eventree::EventTree<> tree;
  Decoder decoder;
  std::vector<std::byte> buffer(options.chunk_bytes);
  while (in.read(reinterpret_cast<char *>(buffer.data()), static_cast<std::streamsize>(buffer.size())) || in.gcount() > 0)
    decoder.decode(std::span<const std::byte>(buffer.data(), static_cast<std::size_t>(in.gcount())), tree);
  const double elapsed = seconds(Clock::now() - start);
  events = tree.size();
  return elapsed;
}

/// Decodes a file with openeb, counting events without storing them. Returns the elapsed time.
double reference_end_to_end(const fs::path &path, std::size_t &events) {
  const auto start = Clock::now();
  Metavision::FileConfigHints hints;
  hints.real_time_playback(false);
  hints.time_shift(false);
  auto camera = Metavision::Camera::from_file(path.string(), hints);
  std::size_t count = 0;
  camera.cd().add_callback([&](const Metavision::EventCD *begin, const Metavision::EventCD *end) {
    count += static_cast<std::size_t>(end - begin);
  });
  camera.start();
  while (camera.is_running()) std::this_thread::sleep_for(std::chrono::microseconds(200));
  camera.stop();
  events = count;
  return seconds(Clock::now() - start);
}

/// Benchmarks one file with `Decoder` and the reference decoder.
template <typename Decoder>
Result benchmark(const fs::path &path, const RawHeader &header, const Options &options) {
  Result result;
  const auto data = read_data(path, header.data_offset);
  result.bytes = data.size();
  const std::span<const std::byte> all(data);

  // Warm-up run: grows the tree to its final size, so timed runs reuse memory and never allocate.
  eventree::EventTree<> tree;
  Decoder decoder;
  for (std::size_t pos = 0; pos < all.size(); pos += options.chunk_bytes)
    decoder.decode(all.subspan(pos, std::min(options.chunk_bytes, all.size() - pos)), tree);
  result.events = tree.size();

  // Latency runs time every chunk; throughput runs time the whole decode, so timer overhead does not count.
  std::vector<double> chunk_us, run_totals;
  for (int run = 0; run < options.runs; ++run) {
    tree.reset();
    decoder.reset();
    for (std::size_t pos = 0; pos < all.size(); pos += options.chunk_bytes) {
      const auto chunk = all.subspan(pos, std::min(options.chunk_bytes, all.size() - pos));
      const auto start = Clock::now();
      decoder.decode(chunk, tree);
      chunk_us.push_back(seconds(Clock::now() - start) * 1e6);
    }
  }
  for (int run = 0; run < options.runs; ++run) {
    tree.reset();
    decoder.reset();
    const auto start = Clock::now();
    for (std::size_t pos = 0; pos < all.size(); pos += options.chunk_bytes)
      decoder.decode(all.subspan(pos, std::min(options.chunk_bytes, all.size() - pos)), tree);
    run_totals.push_back(seconds(Clock::now() - start));
  }
  std::sort(chunk_us.begin(), chunk_us.end());
  result.latency_median_us = percentile(chunk_us, 0.5);
  result.latency_p99_us = percentile(chunk_us, 0.99);
  result.latency_worst_us = chunk_us.empty() ? 0 : chunk_us.back();
  const double decode_s = median(run_totals);
  result.mevents_per_s = decode_s > 0 ? static_cast<double>(result.events) / decode_s / 1e6 : 0;
  result.mb_per_s = decode_s > 0 ? static_cast<double>(result.bytes) / decode_s / 1e6 : 0;

  std::vector<double> ours_e2e, reference_e2e;
  for (int run = 0; run < options.runs; ++run) {
    std::size_t ours_events = 0, reference_events = 0;
    ours_e2e.push_back(end_to_end<Decoder>(path, header.data_offset, options, ours_events));
    reference_e2e.push_back(reference_end_to_end(path, reference_events));
    if (ours_events != result.events || reference_events != result.events)
      throw std::runtime_error("event counts differ: reference " + std::to_string(reference_events) + ", eventree " +
                               std::to_string(ours_events) + " (warm-up " + std::to_string(result.events) + ")");
  }
  result.end_to_end_s = median(ours_e2e);
  result.reference_end_to_end_s = median(reference_e2e);
  const auto ref_s = result.reference_end_to_end_s;
  result.reference_mevents_per_s = ref_s > 0 ? static_cast<double>(result.events) / ref_s / 1e6 : 0;
  result.reference_mb_per_s = ref_s > 0 ? static_cast<double>(result.bytes) / ref_s / 1e6 : 0;
  return result;
}

void print(const Result &r) {
  std::cout << std::fixed << std::setprecision(2);
  std::cout << "  events: " << r.events << "  input_MB: " << static_cast<double>(r.bytes) / 1e6 << "\n"
            << "  eventree latency_us:    median " << r.latency_median_us << "  p99 " << r.latency_p99_us << "  worst "
            << r.latency_worst_us << "\n"
            << "  eventree throughput:    " << r.mevents_per_s << " Mevents/s  " << r.mb_per_s << " MB/s\n"
            << "  eventree end_to_end_s:  " << std::setprecision(4) << r.end_to_end_s << std::setprecision(2) << "\n"
            << "  reference throughput:   " << r.reference_mevents_per_s << " Mevents/s  " << r.reference_mb_per_s
            << " MB/s\n"
            << "  reference end_to_end_s: " << std::setprecision(4) << r.reference_end_to_end_s << "\n"
            << "  speedup end_to_end:     " << std::setprecision(2)
            << (r.end_to_end_s > 0 ? r.reference_end_to_end_s / r.end_to_end_s : 0) << "x\n";
}

void print_summary(const std::string &name, const std::vector<Result> &results) {
  auto med = [&](auto field) {
    std::vector<double> values;
    for (const auto &r : results) values.push_back(r.*field);
    return median(values);
  };
  auto max_of = [&](auto field) {
    double m = 0;
    for (const auto &r : results) m = std::max(m, r.*field);
    return m;
  };
  std::cout << name << " summary (" << results.size() << " files, medians over files, worst is the maximum)\n"
            << std::fixed << std::setprecision(2)
            << "  eventree latency_us median: " << med(&Result::latency_median_us)
            << "  worst: " << max_of(&Result::latency_worst_us) << "\n"
            << "  eventree throughput: " << med(&Result::mevents_per_s) << " Mevents/s  " << med(&Result::mb_per_s)
            << " MB/s\n"
            << "  reference throughput: " << med(&Result::reference_mevents_per_s) << " Mevents/s  "
            << med(&Result::reference_mb_per_s) << " MB/s\n";
}

}  // namespace

int main(int argc, char **argv) {
  CLI::App app{"benchmark: time eventree against openeb on every recording of a directory"};
  std::string directory = "datasets";
  Options options;
  std::size_t chunk_kib = options.chunk_bytes >> 10;
  app.add_option("-d,--directory", directory, "Directory holding the recordings")->capture_default_str();
  app.add_option("-r,--runs", options.runs, "Timed runs per file")->capture_default_str()->check(CLI::PositiveNumber);
  app.add_option("-c,--chunk-kib", chunk_kib, "Input chunk size in KiB")->capture_default_str()->check(CLI::PositiveNumber);
  CLI11_PARSE(app, argc, argv);
  options.chunk_bytes = chunk_kib << 10;

  if (!fs::is_directory(directory)) {
    std::cerr << "error: " << directory << " is not a directory\n";
    return 2;
  }

  std::vector<fs::path> files;
  for (const auto &entry : fs::recursive_directory_iterator(directory))
    if (entry.is_regular_file() && entry.path().extension() != ".tmp_index") files.push_back(entry.path());
  std::sort(files.begin(), files.end());

  std::cout << "runs: " << options.runs << "  chunk_KiB: " << chunk_kib << "\n";
  std::map<std::string, std::vector<Result>> by_format;
  bool ok = true;
  for (const auto &path : files) {
    const auto header = read_header(path);
    if (!header || header->format == WireFormat::other) continue;
    const std::string name = header->format == WireFormat::evt2 ? "EVT2" : "EVT3";
    std::cout << path.string() << "  " << name << "\n";
    try {
      auto result = header->format == WireFormat::evt2 ? benchmark<eventree::Evt2Decoder>(path, *header, options)
                                                       : benchmark<eventree::Evt3Decoder>(path, *header, options);
      print(result);
      by_format[name].push_back(result);
    } catch (const std::exception &e) {
      std::cerr << "  error: " << e.what() << "\n";
      ok = false;
    }
  }
  if (by_format.empty()) {
    std::cerr << "error: no EVT2 or EVT3 recording found in " << directory << "\n";
    return 1;
  }
  for (const auto &[name, results] : by_format) print_summary(name, results);
  return ok ? 0 : 1;
}
