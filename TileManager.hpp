#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>

// io_uring is Linux-specific; keep it optional so the engine still builds on macOS.
#if defined(__linux__) && __has_include(<liburing.h>)
#define QE_HAVE_IO_URING 1
#include <liburing.h>
#else
#define QE_HAVE_IO_URING 0
#endif

namespace qe {

// Simple binary layout for complex<double> stored as interleaved (re, im).
struct Complex {
  double re = 0.0;
  double im = 0.0;
};

inline Complex operator+(const Complex& a, const Complex& b) { return {a.re + b.re, a.im + b.im}; }
inline Complex operator-(const Complex& a, const Complex& b) { return {a.re - b.re, a.im - b.im}; }
inline Complex operator*(const Complex& a, const Complex& b) {
  return {a.re * b.re - a.im * b.im, a.re * b.im + a.im * b.re};
}
inline Complex operator*(double s, const Complex& a) { return {s * a.re, s * a.im}; }

struct Tile {
  std::uint64_t tile_index = 0;
  std::vector<Complex> data; // size == tile_size
  bool dirty = false;
};

// Metadata bitmap: Active means "tile is known non-zero / may contain data".
// Empty tiles are treated as all-zeros (skip disk IO for them).
class TileActivityMap {
 public:
  explicit TileActivityMap(std::uint64_t num_tiles) : num_tiles_(num_tiles) {
    const std::uint64_t words = (num_tiles_ + 63) / 64;
    bits_.assign(words, 0ull);
  }

  std::uint64_t num_tiles() const { return num_tiles_; }

  bool is_active(std::uint64_t tile) const {
    if (tile >= num_tiles_) throw std::out_of_range("tile out of range");
    return (bits_[tile / 64] >> (tile % 64)) & 1ull;
  }

  void set_active(std::uint64_t tile, bool active) {
    if (tile >= num_tiles_) throw std::out_of_range("tile out of range");
    const std::uint64_t mask = 1ull << (tile % 64);
    auto& w = bits_[tile / 64];
    if (active) w |= mask;
    else w &= ~mask;
  }

  std::uint64_t active_count() const {
    std::uint64_t c = 0;
    for (auto w : bits_) c += static_cast<std::uint64_t>(__builtin_popcountll(w));
    return c;
  }

  void save_to(const std::filesystem::path& path) const {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("failed to open meta for write: " + path.string());
    const std::uint64_t n = num_tiles_;
    const std::uint64_t words = static_cast<std::uint64_t>(bits_.size());
    out.write(reinterpret_cast<const char*>(&n), sizeof(n));
    out.write(reinterpret_cast<const char*>(&words), sizeof(words));
    out.write(reinterpret_cast<const char*>(bits_.data()), static_cast<std::streamsize>(words * sizeof(std::uint64_t)));
    if (!out) throw std::runtime_error("failed writing meta: " + path.string());
  }

  void load_from(const std::filesystem::path& path) {
    if (!std::filesystem::exists(path)) return;
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("failed to open meta for read: " + path.string());
    std::uint64_t n = 0, words = 0;
    in.read(reinterpret_cast<char*>(&n), sizeof(n));
    in.read(reinterpret_cast<char*>(&words), sizeof(words));
    if (!in) throw std::runtime_error("failed reading meta header: " + path.string());
    if (n != num_tiles_) throw std::runtime_error("meta num_tiles mismatch");
    if (words != bits_.size()) throw std::runtime_error("meta words mismatch");
    in.read(reinterpret_cast<char*>(bits_.data()), static_cast<std::streamsize>(words * sizeof(std::uint64_t)));
    if (!in) throw std::runtime_error("failed reading meta bits: " + path.string());
  }

 private:
  std::uint64_t num_tiles_ = 0;
  std::vector<std::uint64_t> bits_;
};

// DiskController: reads/writes fixed-size tiles inside a single backing file.
// For Phase 1 we implement async IO via io_uring where available; otherwise
// it falls back to synchronous pread/pwrite (same API).
class DiskController {
 public:
  struct Options {
    std::filesystem::path backing_file; // simulation_state.bin
    std::uint64_t tile_bytes = 0;
    std::uint64_t total_bytes = 0;
    std::uint32_t io_uring_queue_depth = 8;
    bool preallocate = false; // avoid huge ftruncate; rely on sparse-file growth
  };

  explicit DiskController(Options opts) : opts_(std::move(opts)) {
    if (opts_.tile_bytes == 0) throw std::invalid_argument("tile_bytes must be > 0");
    if (opts_.backing_file.empty()) throw std::invalid_argument("backing_file required");

    fd_ = ::open(opts_.backing_file.c_str(), O_CREAT | O_RDWR, 0644);
    if (fd_ < 0) {
      throw std::system_error(errno, std::generic_category(), "open backing file");
    }

    if (opts_.preallocate && opts_.total_bytes != 0) {
      if (::ftruncate(fd_, static_cast<off_t>(opts_.total_bytes)) != 0) {
        throw std::system_error(errno, std::generic_category(), "ftruncate backing file");
      }
    }

#if QE_HAVE_IO_URING
    if (io_uring_queue_init(opts_.io_uring_queue_depth, &ring_, 0) == 0) {
      ring_inited_ = true;
    }
#endif
  }

  DiskController(const DiskController&) = delete;
  DiskController& operator=(const DiskController&) = delete;

  ~DiskController() {
    if (fd_ >= 0) ::close(fd_);
#if QE_HAVE_IO_URING
    if (ring_inited_) io_uring_queue_exit(&ring_);
#endif
  }

  void read_tile(std::uint64_t tile_index, void* dst) {
    const off_t off = static_cast<off_t>(tile_index * opts_.tile_bytes);
    char* p = reinterpret_cast<char*>(dst);
    std::size_t remaining = static_cast<std::size_t>(opts_.tile_bytes);

#if QE_HAVE_IO_URING
    if (ring_inited_) {
      io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
      io_uring_prep_read(sqe, fd_, p, remaining, off);
      sqe->user_data = 0x1234;
      int ret = io_uring_submit(&ring_);
      if (ret < 0) throw std::runtime_error("io_uring_submit failed");
      io_uring_cqe* cqe = nullptr;
      ret = io_uring_wait_cqe(&ring_, &cqe);
      if (ret < 0 || cqe == nullptr) throw std::runtime_error("io_uring_wait_cqe failed");
      if (cqe->res < 0) throw std::runtime_error("io_uring read failed");
      // If we didn't read the full tile (EOF), zero the rest so callers see all-zeros.
      const std::size_t bytes_read = static_cast<std::size_t>(cqe->res);
      if (bytes_read < remaining) {
        std::memset(p + bytes_read, 0, remaining - bytes_read);
      }
      io_uring_cqe_seen(&ring_, cqe);
      return;
    }
#endif

    while (remaining > 0) {
      const ssize_t n = ::pread(fd_, p, remaining, off + static_cast<off_t>(static_cast<std::uint64_t>(opts_.tile_bytes - remaining)));
      if (n < 0) throw std::system_error(errno, std::generic_category(), "pread tile");
      if (n == 0) break; // past EOF => remaining stays zero
      p += n;
      remaining -= static_cast<std::size_t>(n);
    }
    if (remaining > 0) std::memset(p, 0, remaining);
  }

  void write_tile(std::uint64_t tile_index, const void* src) {
    const off_t off = static_cast<off_t>(tile_index * opts_.tile_bytes);
    const char* p = reinterpret_cast<const char*>(src);
    std::size_t remaining = static_cast<std::size_t>(opts_.tile_bytes);

#if QE_HAVE_IO_URING
    if (ring_inited_) {
      io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
      io_uring_prep_write(sqe, fd_, p, remaining, off);
      sqe->user_data = 0x2345;
      int ret = io_uring_submit(&ring_);
      if (ret < 0) throw std::runtime_error("io_uring_submit failed");
      io_uring_cqe* cqe = nullptr;
      ret = io_uring_wait_cqe(&ring_, &cqe);
      if (ret < 0 || cqe == nullptr) throw std::runtime_error("io_uring_wait_cqe failed");
      if (cqe->res < 0) throw std::runtime_error("io_uring write failed");
      io_uring_cqe_seen(&ring_, cqe);
      return;
    }
#endif

    while (remaining > 0) {
      const ssize_t n = ::pwrite(fd_, p, remaining, off + static_cast<off_t>(static_cast<std::uint64_t>(opts_.tile_bytes - remaining)));
      if (n < 0) throw std::system_error(errno, std::generic_category(), "pwrite tile");
      p += n;
      remaining -= static_cast<std::size_t>(n);
    }
  }

  void deallocate_tile(std::uint64_t tile_index) {
#if defined(__linux__)
    // Linux-specific sparse-file punch-hole.
    // Best-effort: if unsupported, it becomes a no-op.
    const off_t off = static_cast<off_t>(tile_index * opts_.tile_bytes);
    const off_t len = static_cast<off_t>(opts_.tile_bytes);
    (void)::fallocate(fd_, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, off, len);
#else
    (void)tile_index;
#endif
  }

 private:
  Options opts_;
  int fd_ = -1;

#if QE_HAVE_IO_URING
  mutable io_uring ring_;
  bool ring_inited_ = false;
#endif
};

// TileManager: VirtualMemoryGrid wrapper.
// This implements Phase 1's: single-file tile state + activity bitmap.
class TileManager {
 public:
  struct Options {
    std::filesystem::path backing_dir; // holds simulation_state.bin + simulation_state.meta
    std::uint64_t tile_size = 1ull << 20; // amplitudes per tile
    std::uint64_t max_cached_tiles = 4; // small RAM cache
    bool fsync_on_flush = false; // reserved
  };

  TileManager(std::uint32_t num_qubits, Options opts)
      : num_qubits_(num_qubits), opts_(std::move(opts)) {
    if (num_qubits_ == 0 || num_qubits_ > 62) {
      throw std::invalid_argument("num_qubits must be in [1, 62]");
    }
    if (opts_.tile_size == 0) throw std::invalid_argument("tile_size must be > 0");
    if (opts_.max_cached_tiles == 0) throw std::invalid_argument("max_cached_tiles must be > 0");

    total_amps_ = 1ull << num_qubits_;
    num_tiles_ = (total_amps_ + opts_.tile_size - 1) / opts_.tile_size;

    std::filesystem::create_directories(opts_.backing_dir);
    backing_file_ = opts_.backing_dir / "simulation_state.bin";
    meta_file_ = opts_.backing_dir / "simulation_state.meta";

    const std::uint64_t tile_bytes = opts_.tile_size * sizeof(Complex);
    const std::uint64_t total_bytes = num_tiles_ * tile_bytes;

    disk_ = std::make_unique<DiskController>(DiskController::Options{
        .backing_file = backing_file_,
        .tile_bytes = tile_bytes,
        .total_bytes = total_bytes,
    });

    activity_ = std::make_unique<TileActivityMap>(num_tiles_);
    activity_->load_from(meta_file_);
  }

  std::uint32_t num_qubits() const { return num_qubits_; }
  std::uint64_t total_amplitudes() const { return total_amps_; }
  std::uint64_t tile_size() const { return opts_.tile_size; }
  std::uint64_t num_tiles() const { return num_tiles_; }

  const TileActivityMap& activity_map() const { return *activity_; }
  TileActivityMap& activity_map() { return *activity_; }

  // Export active tiles as individual files: tile_<i>.bin (complex128 interleaved re,im).
  // This is purely for external testing/reporting scripts.
  void export_active_tiles_as_files(const std::filesystem::path& out_dir) {
    std::filesystem::create_directories(out_dir);
    std::vector<Complex> buf(opts_.tile_size);
    for (std::uint64_t ti = 0; ti < num_tiles_; ++ti) {
      if (!activity_->is_active(ti)) continue;
      disk_->read_tile(ti, buf.data());
      const auto path = out_dir / ("tile_" + std::to_string(ti) + ".bin");
      std::ofstream out(path, std::ios::binary | std::ios::trunc);
      if (!out) throw std::runtime_error("failed to open tile export: " + path.string());
      out.write(reinterpret_cast<const char*>(buf.data()),
                static_cast<std::streamsize>(buf.size() * sizeof(Complex)));
      if (!out) throw std::runtime_error("failed writing tile export: " + path.string());
    }
  }

  // Export the concatenation of all active tiles into one complex128 file.
  // External tools (NumPy) can use this to verify probability sums.
  void export_active_tiles_to_flat_bin(const std::filesystem::path& out_file) {
    std::vector<Complex> buf(opts_.tile_size);
    std::ofstream out(out_file, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("failed to open flat export: " + out_file.string());
    for (std::uint64_t ti = 0; ti < num_tiles_; ++ti) {
      if (!activity_->is_active(ti)) continue;
      disk_->read_tile(ti, buf.data());
      out.write(reinterpret_cast<const char*>(buf.data()),
                static_cast<std::streamsize>(buf.size() * sizeof(Complex)));
      if (!out) throw std::runtime_error("failed writing flat export: " + out_file.string());
    }
  }

  // Reads a tile (creates an all-zero tile in RAM if tile is inactive).
  Tile& get_tile(std::uint64_t tile_index) {
    if (tile_index >= num_tiles_) throw std::out_of_range("tile_index out of range");

    if (auto it = cache_.find(tile_index); it != cache_.end()) {
      touch_lru(tile_index);
      return *(it->second);
    }

    if (cache_.size() >= opts_.max_cached_tiles) evict_one();

    auto t = std::make_unique<Tile>();
    t->tile_index = tile_index;
    t->data.assign(opts_.tile_size, Complex{0.0, 0.0});

    if (activity_->is_active(tile_index)) {
      disk_->read_tile(tile_index, t->data.data());
    }

    lru_.push_back(tile_index);
    auto [ins, ok] = cache_.emplace(tile_index, std::move(t));
    (void)ok;
    return *(ins->second);
  }

  void mark_dirty(std::uint64_t tile_index) {
    Tile& t = get_tile(tile_index);
    t.dirty = true;
  }

  void flush() {
    for (auto& [idx, tp] : cache_) {
      Tile& t = *(tp);
      if (!t.dirty) continue;
      // Any dirty tile becomes active once written.
      activity_->set_active(t.tile_index, true);
      disk_->write_tile(t.tile_index, t.data.data());
      t.dirty = false;
    }
    activity_->save_to(meta_file_);
  }

  // Phase 3: prune cached tiles that are "empty" (all amplitudes ~0).
  // This deactivates the tile in the metadata bitmap and punches a hole
  // to deallocate space in the single backing file.
  void prune_cached_tiles(double abs_epsilon) {
    const double eps2 = abs_epsilon * abs_epsilon;
    for (auto& [idx, tp] : cache_) {
      (void)idx;
      Tile& t = *(tp);
      if (!activity_->is_active(t.tile_index)) continue;

      double max_abs2 = 0.0;
      for (const auto& a : t.data) {
        const double mag2 = a.re * a.re + a.im * a.im;
        if (mag2 > max_abs2) max_abs2 = mag2;
        if (max_abs2 > eps2) break;
      }

      if (max_abs2 <= eps2) {
        deactivate_tile(t.tile_index);
      }
    }

    activity_->save_to(meta_file_);
  }

  ~TileManager() {
    try {
      flush();
    } catch (...) {
      // best-effort in destructor
    }
  }

  void set_amplitude(std::uint64_t idx, Complex v) {
    const auto [ti, off] = tile_and_offset(idx);
    Tile& t = get_tile(ti);
    t.data[off] = v;
    t.dirty = true;

    // Phase 1: mark the tile active when it receives non-zero data.
    if (v.re != 0.0 || v.im != 0.0) activity_->set_active(ti, true);
  }

  Complex get_amplitude(std::uint64_t idx) {
    const auto [ti, off] = tile_and_offset(idx);
    Tile& t = get_tile(ti);
    return t.data[off];
  }

  std::pair<std::uint64_t, std::uint64_t> tile_and_offset(std::uint64_t amplitude_index) const {
    if (amplitude_index >= total_amps_) throw std::out_of_range("amplitude_index out of range");
    return {amplitude_index / opts_.tile_size, amplitude_index % opts_.tile_size};
  }

 private:
  void touch_lru(std::uint64_t tile_index) {
    auto it = std::find(lru_.begin(), lru_.end(), tile_index);
    if (it != lru_.end()) lru_.erase(it);
    lru_.push_back(tile_index);
  }

  void evict_one() {
    if (lru_.empty()) return;
    const std::uint64_t victim = lru_.front();
    lru_.erase(lru_.begin());

    auto it = cache_.find(victim);
    if (it == cache_.end()) return;
    Tile& t = *(it->second);
    if (t.dirty) {
      activity_->set_active(t.tile_index, true);
      disk_->write_tile(t.tile_index, t.data.data());
      t.dirty = false;
    }
    cache_.erase(it);
  }

  void deactivate_tile(std::uint64_t tile_index) {
    activity_->set_active(tile_index, false);
    disk_->deallocate_tile(tile_index);

    if (auto it = cache_.find(tile_index); it != cache_.end()) {
      Tile& t = *(it->second);
      std::fill(t.data.begin(), t.data.end(), Complex{0.0, 0.0});
      t.dirty = false;
    }
  }

  std::uint32_t num_qubits_ = 0;
  Options opts_;
  std::uint64_t total_amps_ = 0;
  std::uint64_t num_tiles_ = 0;

  std::filesystem::path backing_file_;
  std::filesystem::path meta_file_;

  std::unique_ptr<DiskController> disk_;
  std::unique_ptr<TileActivityMap> activity_;

  std::unordered_map<std::uint64_t, std::unique_ptr<Tile>> cache_;
  std::vector<std::uint64_t> lru_;
};

} // namespace qe

