/**
 * low_latency_tcp_v2.hpp
 * ============================================================
 * Low-Latency TCP/IP Library  —  v2.0
 * ============================================================
 * Features
 *   B) SPSC lock-free ring buffer + CPU affinity / NUMA pinning
 *      integrated into a refactored LowLatencyTCP class
 *   C) HL7 v2.x MLLP layer with ACK/NACK, message sequencing,
 *      heartbeat (keep-alive NUL frames), and auto-reconnect
 *   D) io_uring back-end (Linux 5.1+) as a drop-in alternative
 *      to the epoll reactor — zero-syscall submission path
 *
 * Build
 *   g++ -std=c++20 -O3 -march=native \
 *       -pthread -lssl -lcrypto -luring \
 *       your_app.cpp -o app
 *
 * Dependencies
 *   libssl-dev   (OpenSSL >= 1.1)
 *   liburing-dev (liburing >= 2.0, kernel >= 5.1)
 *   POSIX threads, Linux sockets
 *
 * Changelog
 *   2026-05-22  v2.0  SPSC queue, CPU pinning, HL7 ACK/NACK
 *                     sequencing, heartbeat, reconnect manager,
 *                     io_uring reactor.
 *   2025-10-31  v1.0  Initial: blocking TCP + TLS + HL7 bench.
 * ============================================================
 */

#pragma once

// ── Standard headers ─────────────────────────────────────────
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

// ── POSIX / Linux ─────────────────────────────────────────────
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <sched.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <numa.h>       // libnuma-dev — omit if unavailable

// ── io_uring ─────────────────────────────────────────────────
#ifdef USE_IO_URING
#  include <liburing.h>
#endif

// ── OpenSSL ──────────────────────────────────────────────────
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

namespace LowLatency {

// ═══════════════════════════════════════════════════════════════
// SECTION B — SPSC Queue + CPU / NUMA utilities
// ═══════════════════════════════════════════════════════════════

/**
 * SPSCQueue<T, N>
 * Single-Producer / Single-Consumer lock-free ring buffer.
 *
 * Each of head_ and tail_ lives on its own cache line (64 B) to
 * prevent false sharing between the producer and consumer threads.
 *
 * N must be a power of two so the modulo reduces to a bitmask.
 */
template<typename T, std::size_t N>
class SPSCQueue {
    static_assert((N & (N - 1)) == 0, "N must be a power of two");
    static constexpr std::size_t MASK = N - 1;

    alignas(64) std::array<T, N> buf_{};
    alignas(64) std::atomic<std::size_t> head_{0};
    alignas(64) std::atomic<std::size_t> tail_{0};

public:
    // Producer side — returns false if the queue is full.
    [[nodiscard]] bool push(const T& item) noexcept {
        const std::size_t t = tail_.load(std::memory_order_relaxed);
        const std::size_t next = (t + 1) & MASK;
        if (next == head_.load(std::memory_order_acquire))
            return false;   // full
        buf_[t] = item;
        tail_.store(next, std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool push(T&& item) noexcept {
        const std::size_t t = tail_.load(std::memory_order_relaxed);
        const std::size_t next = (t + 1) & MASK;
        if (next == head_.load(std::memory_order_acquire))
            return false;
        buf_[t] = std::move(item);
        tail_.store(next, std::memory_order_release);
        return true;
    }

    // Consumer side — returns false if empty.
    [[nodiscard]] bool pop(T& item) noexcept {
        const std::size_t h = head_.load(std::memory_order_relaxed);
        if (h == tail_.load(std::memory_order_acquire))
            return false;   // empty
        item = std::move(buf_[h]);
        head_.store((h + 1) & MASK, std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool empty() const noexcept {
        return head_.load(std::memory_order_acquire) ==
               tail_.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::size_t size() const noexcept {
        const auto h = head_.load(std::memory_order_acquire);
        const auto t = tail_.load(std::memory_order_acquire);
        return (t - h) & MASK;
    }

    [[nodiscard]] static constexpr std::size_t capacity() noexcept { return N; }
};

// ─── CPU / NUMA helpers ───────────────────────────────────────

struct AffinityConfig {
    int  cpu_core   = -1;    // -1 = no pinning
    int  numa_node  = -1;    // -1 = no NUMA preference
    int  rt_priority = 0;    // 0  = normal SCHED_OTHER; >0 → SCHED_FIFO
};

/**
 * Apply AffinityConfig to the calling thread.
 * Must be called from within the thread to be pinned.
 */
inline void apply_affinity(const AffinityConfig& cfg) {
    // CPU pinning
    if (cfg.cpu_core >= 0) {
        cpu_set_t cs;
        CPU_ZERO(&cs);
        CPU_SET(cfg.cpu_core, &cs);
        if (pthread_setaffinity_np(pthread_self(), sizeof(cs), &cs) != 0)
            std::cerr << "[affinity] setaffinity failed: " << strerror(errno) << '\n';
    }

    // NUMA memory locality
#ifdef HAVE_LIBNUMA
    if (cfg.numa_node >= 0 && numa_available() >= 0) {
        struct bitmask* mask = numa_bitmask_alloc(numa_num_configured_nodes());
        numa_bitmask_setbit(mask, cfg.numa_node);
        numa_set_membind(mask);
        numa_bitmask_free(mask);
    }
#endif

    // Real-time scheduling
    if (cfg.rt_priority > 0) {
        sched_param sp{};
        sp.sched_priority = cfg.rt_priority;
        if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) != 0)
            std::cerr << "[affinity] setschedparam failed: " << strerror(errno) << '\n';
    }
}

// ─── Socket kernel-level low-latency knobs ────────────────────

struct SocketLatencyConfig {
    bool     no_delay        = true;   // TCP_NODELAY
    bool     quick_ack       = true;   // TCP_QUICKACK
    bool     busy_poll       = false;  // SO_BUSY_POLL (spin in kernel)
    int      busy_poll_us    = 50;     // microseconds for SO_BUSY_POLL
    bool     incoming_cpu    = true;   // SO_INCOMING_CPU (IRQ affinity)
    int      send_buf        = 256*1024;
    int      recv_buf        = 256*1024;
    bool     reuse_addr      = true;
    bool     reuse_port      = false;
    bool     keep_alive      = true;
    int      keepalive_idle  = 30;
    int      keepalive_intvl = 5;
    int      keepalive_cnt   = 3;
};

inline void apply_socket_options(int fd, const SocketLatencyConfig& c) {
    auto sopt = [&](int level, int opt, int val) {
        setsockopt(fd, level, opt, &val, sizeof(val));
    };
    if (c.no_delay)      sopt(IPPROTO_TCP, TCP_NODELAY,    1);
#ifdef TCP_QUICKACK
    if (c.quick_ack)     sopt(IPPROTO_TCP, TCP_QUICKACK,   1);
#endif
    if (c.reuse_addr)    sopt(SOL_SOCKET,  SO_REUSEADDR,   1);
    if (c.reuse_port)    sopt(SOL_SOCKET,  SO_REUSEPORT,   1);
    if (c.keep_alive) {
        sopt(SOL_SOCKET,  SO_KEEPALIVE,                     1);
        sopt(IPPROTO_TCP, TCP_KEEPIDLE,    c.keepalive_idle);
        sopt(IPPROTO_TCP, TCP_KEEPINTVL,   c.keepalive_intvl);
        sopt(IPPROTO_TCP, TCP_KEEPCNT,     c.keepalive_cnt);
    }
    sopt(SOL_SOCKET, SO_SNDBUF, c.send_buf);
    sopt(SOL_SOCKET, SO_RCVBUF, c.recv_buf);
#ifdef SO_BUSY_POLL
    if (c.busy_poll) sopt(SOL_SOCKET, SO_BUSY_POLL, c.busy_poll_us);
#endif
#ifdef SO_INCOMING_CPU
    if (c.incoming_cpu) {
        int cpu = sched_getcpu();
        sopt(SOL_SOCKET, SO_INCOMING_CPU, cpu);
    }
#endif
}

// ─── Non-blocking helper ─────────────────────────────────────

inline void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

// ═══════════════════════════════════════════════════════════════
// SECTION C — HL7 MLLP with ACK/NACK, Sequencing & Reconnect
// ═══════════════════════════════════════════════════════════════

namespace HL7 {

// ── MLLP framing constants ────────────────────────────────────
constexpr char MLLP_START  = 0x0B;   // VT
constexpr char MLLP_END1   = 0x1C;   // FS
constexpr char MLLP_END2   = 0x0D;   // CR
constexpr char SEG_TERM    = '\r';
constexpr char FLD         = '|';
constexpr char CMP         = '^';
constexpr char REP         = '~';
constexpr char ESC         = '\\';
constexpr char SUB         = '&';

// ── Message types ─────────────────────────────────────────────
enum class AckCode { AA, AE, AR };   // Accept, Error, Reject

struct Segment {
    std::string id;                    // e.g. "MSH", "OBX"
    std::vector<std::string> fields;   // fields[0] = after segment id

    std::string_view field(std::size_t idx) const {
        return idx < fields.size() ? std::string_view(fields[idx]) : "";
    }
    std::string to_string() const {
        std::string s = id;
        for (auto& f : fields) { s += FLD; s += f; }
        s += SEG_TERM;
        return s;
    }
};

struct Message {
    std::vector<Segment> segments;

    // Convenience accessors
    const Segment* get(std::string_view id) const {
        for (auto& s : segments)
            if (s.id == id) return &s;
        return nullptr;
    }
    Segment* get(std::string_view id) {
        for (auto& s : segments)
            if (s.id == id) return &s;
        return nullptr;
    }

    std::string message_id() const {
        if (auto* msh = get("MSH"))
            return std::string(msh->field(9));   // MSH.10
        return "";
    }
    std::string message_type() const {
        if (auto* msh = get("MSH"))
            return std::string(msh->field(8));   // MSH.9
        return "";
    }
    std::string sending_app() const {
        if (auto* msh = get("MSH"))
            return std::string(msh->field(2));   // MSH.3
        return "";
    }

    std::string to_wire() const {
        std::string body;
        for (auto& s : segments) body += s.to_string();
        std::string framed;
        framed += MLLP_START;
        framed += body;
        framed += MLLP_END1;
        framed += MLLP_END2;
        return framed;
    }
};

// ── Parser ────────────────────────────────────────────────────

inline Segment parse_segment(std::string_view raw) {
    Segment seg;
    if (raw.size() < 3) return seg;
    seg.id = std::string(raw.substr(0, 3));
    std::size_t pos = 3;
    while (pos <= raw.size()) {
        std::size_t next = raw.find(FLD, pos);
        if (next == std::string_view::npos) next = raw.size();
        seg.fields.emplace_back(raw.substr(pos, next - pos));
        pos = next + 1;
    }
    return seg;
}

inline std::optional<Message> parse(std::string_view raw) {
    // Strip MLLP framing if present
    if (!raw.empty() && raw.front() == MLLP_START) raw.remove_prefix(1);
    if (raw.size() >= 2 && raw.back() == MLLP_END2)  raw.remove_suffix(1);
    if (!raw.empty()    && raw.back() == MLLP_END1)   raw.remove_suffix(1);

    Message msg;
    std::size_t pos = 0;
    while (pos < raw.size()) {
        std::size_t end = raw.find(SEG_TERM, pos);
        if (end == std::string_view::npos) end = raw.size();
        auto line = raw.substr(pos, end - pos);
        if (!line.empty())
            msg.segments.push_back(parse_segment(line));
        pos = end + 1;
    }
    return msg.segments.empty() ? std::nullopt : std::optional<Message>(msg);
}

// ── Timestamp helper ─────────────────────────────────────────

inline std::string hl7_timestamp() {
    auto now = std::chrono::system_clock::now();
    std::time_t t  = std::chrono::system_clock::to_time_t(now);
    std::tm*    tm = std::gmtime(&t);
    char buf[20];
    std::strftime(buf, sizeof(buf), "%Y%m%d%H%M%S", tm);
    return buf;
}

// ── ACK / NACK builder ────────────────────────────────────────
/**
 * build_ack()
 *   orig      : the message being acknowledged
 *   code      : AA / AE / AR
 *   text      : optional free-text reason (ERR segment)
 *   recv_app  : receiving application name (default "ACK_ENGINE")
 *
 * Returns a fully framed MLLP ACK message.
 */
inline Message build_ack(const Message& orig,
                          AckCode        code,
                          std::string_view text      = "",
                          std::string_view recv_app  = "ACK_ENGINE",
                          std::string_view recv_fac  = "FACILITY") {
    static std::atomic<uint32_t> seq{1};

    std::string code_str = (code == AckCode::AA) ? "AA" :
                           (code == AckCode::AE) ? "AE" : "AR";

    auto* orig_msh = orig.get("MSH");
    std::string orig_id   = orig_msh ? std::string(orig_msh->field(9))  : "";
    std::string send_app  = orig_msh ? std::string(orig_msh->field(2))  : "";
    std::string send_fac  = orig_msh ? std::string(orig_msh->field(3))  : "";
    std::string version   = orig_msh ? std::string(orig_msh->field(11)) : "2.5";

    Message ack;

    // MSH
    {
        Segment msh;
        msh.id = "MSH";
        //  1: encoding chars   2: send app    3: send fac
        //  4: recv app         5: recv fac    6: datetime
        //  7: (empty security) 8: msg type    9: msg id
        // 10: proc id         11: version
        msh.fields = {
            "^~\\&",
            std::string(recv_app), std::string(recv_fac),
            send_app, send_fac,
            hl7_timestamp(), "",
            "ACK",
            "ACK" + std::to_string(seq++),
            "P", version
        };
        ack.segments.push_back(std::move(msh));
    }

    // MSA
    {
        Segment msa;
        msa.id = "MSA";
        msa.fields = { code_str, orig_id, std::string(text) };
        ack.segments.push_back(std::move(msa));
    }

    // ERR segment for AE / AR
    if (code != AckCode::AA && !text.empty()) {
        Segment err;
        err.id = "ERR";
        err.fields = { "", "", "", "E", "", "", std::string(text) };
        ack.segments.push_back(std::move(err));
    }

    return ack;
}

// ── Sequencing tracker ────────────────────────────────────────
/**
 * SequenceTracker
 *   Tracks outbound sequence numbers and pending ACKs.
 *   Thread-safe (mutex protected) — typically used by the I/O thread.
 */
class SequenceTracker {
public:
    struct PendingMsg {
        std::string              msg_id;
        std::string              wire;           // framed bytes to retransmit
        std::chrono::steady_clock::time_point sent_at;
        int                      retry_count = 0;
    };

    static constexpr int MAX_RETRIES = 3;
    static constexpr std::chrono::milliseconds ACK_TIMEOUT{5000};

    // Register a sent message — called before writing to the wire.
    void register_sent(const std::string& msg_id, const std::string& wire) {
        std::lock_guard lk(mu_);
        pending_[msg_id] = { msg_id, wire,
                             std::chrono::steady_clock::now(), 0 };
        last_sent_id_ = msg_id;
    }

    // Confirm receipt of an ACK — returns true if the msg was pending.
    bool confirm_ack(const std::string& msg_id, AckCode code) {
        std::lock_guard lk(mu_);
        auto it = pending_.find(msg_id);
        if (it == pending_.end()) return false;

        if (code == AckCode::AA) {
            pending_.erase(it);
            return true;
        }
        // AE / AR — increment retry counter, leave in map for retransmit.
        it->second.retry_count++;
        return true;
    }

    // Returns messages that have timed out and need retransmission.
    std::vector<PendingMsg> get_timed_out() {
        std::lock_guard lk(mu_);
        std::vector<PendingMsg> out;
        auto now = std::chrono::steady_clock::now();
        for (auto& [id, pm] : pending_) {
            if (now - pm.sent_at > ACK_TIMEOUT) {
                if (pm.retry_count < MAX_RETRIES)
                    out.push_back(pm);
                else
                    expired_.push_back(id);
            }
        }
        for (auto& id : expired_) pending_.erase(id);
        expired_.clear();
        return out;
    }

    [[nodiscard]] std::size_t pending_count() const {
        std::lock_guard lk(mu_);
        return pending_.size();
    }

    [[nodiscard]] std::string last_sent_id() const {
        std::lock_guard lk(mu_);
        return last_sent_id_;
    }

private:
    mutable std::mutex mu_;
    std::unordered_map<std::string, PendingMsg> pending_;
    std::vector<std::string> expired_;
    std::string last_sent_id_;
};

// ── MLLP receive buffer ───────────────────────────────────────
/**
 * MLLPBuffer
 *   Accumulates raw bytes and extracts complete MLLP frames.
 *   Call feed() with each recv() chunk; complete_frame() pops one frame.
 */
class MLLPBuffer {
    std::string buf_;
public:
    void feed(const char* data, std::size_t len) {
        buf_.append(data, len);
    }

    // Returns the next complete MLLP frame (without framing bytes), or nullopt.
    std::optional<std::string> complete_frame() {
        auto start = buf_.find(MLLP_START);
        if (start == std::string::npos) return std::nullopt;
        auto end1 = buf_.find(MLLP_END1, start + 1);
        if (end1 == std::string::npos) return std::nullopt;
        if (end1 + 1 >= buf_.size() || buf_[end1 + 1] != MLLP_END2)
            return std::nullopt;

        std::string frame = buf_.substr(start + 1, end1 - start - 1);
        buf_.erase(0, end1 + 2);
        return frame;
    }

    void clear() { buf_.clear(); }
};

} // namespace HL7

// ═══════════════════════════════════════════════════════════════
// Reconnect + Heartbeat Manager
// ═══════════════════════════════════════════════════════════════

class ReconnectManager {
public:
    struct Config {
        std::chrono::milliseconds heartbeat_interval{5'000};
        std::chrono::milliseconds reconnect_delay    {1'000};
        std::chrono::milliseconds max_reconnect_delay{30'000};
        int max_retries = -1;   // -1 = infinite
    };

    using ConnectFn   = std::function<bool()>;   // true = connected
    using HeartbeatFn = std::function<bool()>;   // true = still alive
    using EventFn     = std::function<void(std::string_view)>;

    explicit ReconnectManager(Config cfg = {}) : cfg_(cfg) {}

    void on_event(EventFn fn) { event_fn_ = std::move(fn); }

    void run(ConnectFn connect, HeartbeatFn heartbeat) {
        running_ = true;
        int  retries   = 0;
        auto delay     = cfg_.reconnect_delay;

        while (running_) {
            emit("Connecting…");
            if (!connect()) {
                emit("Connection failed, retrying in " +
                     std::to_string(delay.count()) + " ms");
                std::this_thread::sleep_for(delay);
                delay = std::min(delay * 2, cfg_.max_reconnect_delay);
                if (cfg_.max_retries > 0 && ++retries >= cfg_.max_retries) {
                    emit("Max retries reached, giving up.");
                    break;
                }
                continue;
            }
            retries = 0;
            delay   = cfg_.reconnect_delay;
            emit("Connected — starting heartbeat");

            while (running_) {
                std::this_thread::sleep_for(cfg_.heartbeat_interval);
                if (!heartbeat()) {
                    emit("Heartbeat failed — reconnecting");
                    break;
                }
            }
        }
        emit("ReconnectManager stopped.");
    }

    void stop() { running_ = false; }

private:
    Config cfg_;
    std::atomic<bool> running_{false};
    EventFn event_fn_;

    void emit(std::string_view msg) {
        if (event_fn_) event_fn_(msg);
        else std::cerr << "[reconnect] " << msg << '\n';
    }
};

// ═══════════════════════════════════════════════════════════════
// Refactored LowLatencyTCP  (ties B + C together)
// ═══════════════════════════════════════════════════════════════

/**
 * LowLatencyTCP<InQ, OutQ>
 *
 * Architecture
 *   • Network thread  — epoll/io_uring, recv bytes → MLLPBuffer
 *     → parses frames → pushes into recv_queue_
 *   • Application thread — pops recv_queue_, pushes into send_queue_
 *   • Send thread — pops send_queue_, serialises HL7, wraps MLLP,
 *     calls SequenceTracker, writes to socket
 *
 * Template parameters set the queue depth (must be power of two).
 */
template<std::size_t InQ = 4096, std::size_t OutQ = 4096>
class LowLatencyTCP {
public:
    using RecvQueue = SPSCQueue<HL7::Message, InQ>;
    using SendQueue = SPSCQueue<HL7::Message, OutQ>;

    struct Config {
        std::string host;
        uint16_t    port      = 2575;
        SocketLatencyConfig sock{};
        AffinityConfig      net_affinity{};   // for the network thread
        AffinityConfig      send_affinity{};  // for the send thread
        ReconnectManager::Config reconnect{};
        bool        auto_ack  = true;   // send AA on every inbound message
        bool        client_mode = true; // false = server (listen)
    };

    explicit LowLatencyTCP(Config cfg)
        : cfg_(std::move(cfg)), seq_tracker_() {}

    ~LowLatencyTCP() { stop(); }

    // ── Lifecycle ──────────────────────────────────────────────
    void start() {
        running_ = true;
        net_thread_  = std::thread([this]{ net_loop();  });
        send_thread_ = std::thread([this]{ send_loop(); });
    }

    void stop() {
        running_ = false;
        reconnect_mgr_.stop();
        if (net_thread_.joinable())  net_thread_.join();
        if (send_thread_.joinable()) send_thread_.join();
        if (sock_fd_ >= 0) { ::close(sock_fd_); sock_fd_ = -1; }
    }

    // ── Application API ────────────────────────────────────────

    /** Enqueue a message for transmission.  Non-blocking. */
    bool send(HL7::Message msg) {
        return send_queue_.push(std::move(msg));
    }

    /** Pop the next received message.  Non-blocking. */
    bool recv(HL7::Message& out) {
        return recv_queue_.pop(out);
    }

    /** Block until a message is available or timeout elapses. */
    bool recv_wait(HL7::Message& out,
                   std::chrono::milliseconds timeout = std::chrono::milliseconds(100)) {
        auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            if (recv_queue_.pop(out)) return true;
            std::this_thread::yield();
        }
        return false;
    }

    [[nodiscard]] std::size_t pending_acks() const {
        return seq_tracker_.pending_count();
    }

    void set_message_callback(std::function<void(const HL7::Message&)> cb) {
        msg_cb_ = std::move(cb);
    }

private:
    Config                  cfg_;
    std::atomic<bool>       running_{false};
    int                     sock_fd_{-1};

    RecvQueue               recv_queue_;
    SendQueue               send_queue_;
    HL7::SequenceTracker    seq_tracker_;
    HL7::MLLPBuffer         mllp_buf_;
    ReconnectManager        reconnect_mgr_{cfg_.reconnect};
    std::thread             net_thread_;
    std::thread             send_thread_;
    std::function<void(const HL7::Message&)> msg_cb_;

    // ── Connect / accept ───────────────────────────────────────
    bool do_connect() {
        if (sock_fd_ >= 0) { ::close(sock_fd_); sock_fd_ = -1; }

        sock_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (sock_fd_ < 0) return false;

        apply_socket_options(sock_fd_, cfg_.sock);
        set_nonblocking(sock_fd_);

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port   = htons(cfg_.port);
        ::inet_pton(AF_INET, cfg_.host.c_str(), &addr.sin_addr);

        if (::connect(sock_fd_,
                      reinterpret_cast<sockaddr*>(&addr),
                      sizeof(addr)) < 0
            && errno != EINPROGRESS) {
            ::close(sock_fd_);
            sock_fd_ = -1;
            return false;
        }
        return true;
    }

    // ── Network loop (epoll path) ──────────────────────────────
    void net_loop() {
        apply_affinity(cfg_.net_affinity);
        mllp_buf_.clear();

        reconnect_mgr_.run(
            [this]{ return do_connect(); },
            [this]{
                // heartbeat: send a raw NUL byte (MLLP keep-alive)
                char nul = 0;
                return ::write(sock_fd_, &nul, 1) > 0;
            }
        );

        if (!running_) return;

        int epoll_fd = ::epoll_create1(EPOLL_CLOEXEC);
        epoll_event ev{};
        ev.events   = EPOLLIN | EPOLLET;
        ev.data.fd  = sock_fd_;
        ::epoll_ctl(epoll_fd, EPOLL_CTL_ADD, sock_fd_, &ev);

        constexpr int MAX_EV = 64;
        epoll_event events[MAX_EV];
        char rbuf[65536];

        while (running_) {
            int n = ::epoll_wait(epoll_fd, events, MAX_EV, 100 /*ms*/);
            for (int i = 0; i < n; ++i) {
                if (!(events[i].events & EPOLLIN)) continue;
                for (;;) {
                    ssize_t r = ::read(sock_fd_, rbuf, sizeof(rbuf));
                    if (r <= 0) break;
                    mllp_buf_.feed(rbuf, static_cast<std::size_t>(r));
                    while (auto frame = mllp_buf_.complete_frame()) {
                        if (auto msg = HL7::parse(*frame)) {
                            handle_inbound(*msg);
                        }
                    }
                }
            }
            // Retransmit timed-out messages
            for (auto& pm : seq_tracker_.get_timed_out()) {
                pm.sent_at = std::chrono::steady_clock::now();
                pm.retry_count++;
                ::write(sock_fd_, pm.wire.data(), pm.wire.size());
            }
        }
        ::close(epoll_fd);
    }

    void handle_inbound(const HL7::Message& msg) {
        // Resolve pending ACK if this is an ACK message
        if (msg.message_type() == "ACK") {
            if (auto* msa = msg.get("MSA")) {
                std::string code = std::string(msa->field(0));
                std::string id   = std::string(msa->field(1));
                HL7::AckCode ac  = (code == "AA") ? HL7::AckCode::AA :
                                   (code == "AE") ? HL7::AckCode::AE :
                                                    HL7::AckCode::AR;
                seq_tracker_.confirm_ack(id, ac);
            }
            return;
        }

        // Auto-ACK
        if (cfg_.auto_ack) {
            auto ack = HL7::build_ack(msg, HL7::AckCode::AA);
            auto wire = ack.to_wire();
            ::write(sock_fd_, wire.data(), wire.size());
        }

        if (msg_cb_) msg_cb_(msg);
        recv_queue_.push(msg);   // best-effort; drop if full
    }

    // ── Send loop ─────────────────────────────────────────────
    void send_loop() {
        apply_affinity(cfg_.send_affinity);
        HL7::Message msg;
        while (running_) {
            if (!send_queue_.pop(msg)) {
                std::this_thread::yield();
                continue;
            }
            if (sock_fd_ < 0) continue;
            auto wire = msg.to_wire();
            seq_tracker_.register_sent(msg.message_id(), wire);
            ::write(sock_fd_, wire.data(), wire.size());
        }
    }
};

// ═══════════════════════════════════════════════════════════════
// SECTION D — io_uring Reactor
// ═══════════════════════════════════════════════════════════════

#ifdef USE_IO_URING

/**
 * IoUringReactor
 *
 * Provides a single-threaded event loop built on io_uring.
 * Compared with epoll, io_uring reduces syscall overhead to zero
 * for the submission path — SQEs are written to shared memory;
 * only io_uring_submit() is needed once per batch, and only when
 * the kernel has not already drained the SQ via SQPOLL.
 *
 * Usage
 *   IoUringReactor reactor(queue_depth=256, sqpoll=false);
 *   reactor.add_recv(fd, buf, len, [](int res){ … });
 *   reactor.add_send(fd, buf, len, [](int res){ … });
 *   reactor.run();   // blocks until stop() is called
 *
 * SQPOLL mode (sqpoll=true)
 *   The kernel spawns a dedicated polling thread that continuously
 *   drains the SQ.  This eliminates the io_uring_submit() syscall
 *   entirely — achieving the absolute minimum kernel-crossing
 *   overhead.  Requires CAP_SYS_NICE or kernel >= 5.11 with
 *   IORING_FEAT_SQPOLL_NONFIXED.
 */
class IoUringReactor {
public:
    static constexpr std::size_t DEFAULT_QUEUE = 256;

    using Callback = std::function<void(int /*res*/)>;

    explicit IoUringReactor(unsigned queue_depth = DEFAULT_QUEUE,
                            bool     sqpoll      = false)
        : queue_depth_(queue_depth)
    {
        io_uring_params params{};
        if (sqpoll) params.flags |= IORING_SETUP_SQPOLL;

        int ret = io_uring_queue_init_params(queue_depth_, &ring_, &params);
        if (ret < 0)
            throw std::runtime_error(
                std::string("io_uring_queue_init: ") + strerror(-ret));
    }

    ~IoUringReactor() {
        io_uring_queue_exit(&ring_);
    }

    // ── Submission helpers ─────────────────────────────────────

    void add_recv(int fd, void* buf, unsigned len, Callback cb) {
        io_uring_sqe* sqe = get_sqe();
        io_uring_prep_recv(sqe, fd, buf, len, 0);
        attach_callback(sqe, std::move(cb));
    }

    void add_send(int fd, const void* buf, unsigned len, Callback cb) {
        io_uring_sqe* sqe = get_sqe();
        io_uring_prep_send(sqe, fd,
                           const_cast<void*>(buf), len, MSG_NOSIGNAL);
        attach_callback(sqe, std::move(cb));
    }

    void add_accept(int server_fd, sockaddr_in* addr, socklen_t* addrlen,
                    Callback cb) {
        io_uring_sqe* sqe = get_sqe();
        io_uring_prep_accept(sqe, server_fd,
                             reinterpret_cast<sockaddr*>(addr),
                             addrlen, 0);
        attach_callback(sqe, std::move(cb));
    }

    /** Submit all pending SQEs and process at least min_completions CQEs. */
    int submit_and_wait(unsigned min_completions = 0) {
        int ret = io_uring_submit_and_wait(&ring_,
                                           static_cast<unsigned>(min_completions));
        if (ret < 0 && ret != -EINTR)
            throw std::runtime_error(
                std::string("io_uring_submit_and_wait: ") + strerror(-ret));
        drain_cq();
        return ret;
    }

    /** Non-blocking submit + drain. */
    int submit() {
        int ret = io_uring_submit(&ring_);
        drain_cq();
        return ret;
    }

    // ── Event loop ────────────────────────────────────────────
    void run() {
        running_ = true;
        while (running_) {
            submit_and_wait(1);
        }
    }
    void stop() { running_ = false; }

    [[nodiscard]] io_uring* ring() { return &ring_; }

private:
    io_uring ring_{};
    unsigned queue_depth_;
    std::atomic<bool> running_{false};

    // We store callbacks as heap-allocated objects whose pointer is
    // stuffed into the SQE user_data field — the conventional pattern.
    io_uring_sqe* get_sqe() {
        io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
        if (!sqe)
            throw std::runtime_error("io_uring SQ full — increase queue depth");
        return sqe;
    }

    void attach_callback(io_uring_sqe* sqe, Callback cb) {
        auto* p = new Callback(std::move(cb));
        io_uring_sqe_set_data(sqe, p);
    }

    void drain_cq() {
        io_uring_cqe* cqe;
        unsigned head;
        io_uring_for_each_cqe(&ring_, head, cqe) {
            auto* cb = static_cast<Callback*>(io_uring_cqe_get_data(cqe));
            if (cb) {
                (*cb)(cqe->res);
                delete cb;
            }
        }
        io_uring_cq_advance(&ring_, /* count */ [&]() -> unsigned {
            unsigned cnt = 0;
            io_uring_cqe* c;
            unsigned h;
            io_uring_for_each_cqe(&ring_, h, c) ++cnt;
            return cnt;
        }());
    }
};

// ── io_uring MLLP server skeleton ────────────────────────────
/**
 * IoUringMLLPServer
 *
 * Wraps IoUringReactor to provide a zero-copy MLLP receive loop.
 * For each accepted connection a RecvContext is allocated; recv
 * completions feed MLLPBuffer and dispatch complete HL7 frames to
 * the user callback.
 */
class IoUringMLLPServer {
public:
    using MsgCallback = std::function<void(int /*fd*/, HL7::Message)>;

    IoUringMLLPServer(uint16_t port,
                      MsgCallback cb,
                      unsigned queue_depth = 512,
                      bool sqpoll = false)
        : reactor_(queue_depth, sqpoll)
        , cb_(std::move(cb))
    {
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        int one = 1;
        setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        set_nonblocking(listen_fd_);

        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port        = htons(port);
        ::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        ::listen(listen_fd_, 128);
    }

    void run() {
        arm_accept();
        reactor_.run();
    }
    void stop() { reactor_.stop(); }

private:
    IoUringReactor   reactor_;
    MsgCallback      cb_;
    int              listen_fd_{-1};

    struct ConnCtx {
        int           fd;
        std::array<char, 65536> buf{};
        HL7::MLLPBuffer mllp;
    };

    sockaddr_in  client_addr_{};
    socklen_t    client_addrlen_{sizeof(client_addr_)};

    void arm_accept() {
        reactor_.add_accept(listen_fd_, &client_addr_, &client_addrlen_,
            [this](int res) {
                if (res >= 0) {
                    auto* ctx = new ConnCtx{ res, {}, {} };
                    arm_recv(ctx);
                }
                arm_accept();  // re-arm for next connection
            });
    }

    void arm_recv(ConnCtx* ctx) {
        reactor_.add_recv(ctx->fd, ctx->buf.data(),
                          static_cast<unsigned>(ctx->buf.size()),
            [this, ctx](int res) {
                if (res <= 0) {
                    ::close(ctx->fd);
                    delete ctx;
                    return;
                }
                ctx->mllp.feed(ctx->buf.data(), static_cast<std::size_t>(res));
                while (auto frame = ctx->mllp.complete_frame()) {
                    if (auto msg = HL7::parse(*frame)) {
                        cb_(ctx->fd, std::move(*msg));
                    }
                }
                arm_recv(ctx);  // re-arm
            });
    }
};

#endif // USE_IO_URING

// ═══════════════════════════════════════════════════════════════
// Usage Example — compile with:
//   g++ -std=c++20 -O3 -march=native -pthread -lssl -lcrypto \
//       [-DUSE_IO_URING -luring] example.cpp -o example
// ═══════════════════════════════════════════════════════════════
/*
#include "low_latency_tcp_v2.hpp"
using namespace LowLatency;

// ── B: Epoll client with SPSC + CPU pinning ──────────────────
void run_epoll_client() {
    LowLatencyTCP<4096, 4096>::Config cfg;
    cfg.host                  = "127.0.0.1";
    cfg.port                  = 2575;
    cfg.sock.no_delay         = true;
    cfg.sock.busy_poll        = true;
    cfg.sock.busy_poll_us     = 50;
    cfg.net_affinity.cpu_core = 2;
    cfg.net_affinity.rt_priority = 80;
    cfg.send_affinity.cpu_core   = 3;
    cfg.auto_ack              = true;

    LowLatencyTCP<4096, 4096> conn(cfg);
    conn.set_message_callback([](const HL7::Message& m) {
        std::cout << "[rx] " << m.message_type()
                  << " id=" << m.message_id() << '\n';
    });
    conn.start();

    // Build and send an ORM message
    HL7::Message msg;
    HL7::Segment msh; msh.id = "MSH";
    msh.fields = { "^~\\&","LAB","FAC","HIS","FAC",
                   HL7::hl7_timestamp(),"","ORM^O01",
                   "MSG00001","P","2.5" };
    msg.segments.push_back(msh);
    conn.send(std::move(msg));

    std::this_thread::sleep_for(std::chrono::seconds(5));
    conn.stop();
}

// ── C: HL7 ACK/NACK + sequencing demo ────────────────────────
void demo_ack_nack() {
    // Simulate receiving an ORM, building ACK/NACK
    HL7::Message orm;
    HL7::Segment msh; msh.id = "MSH";
    msh.fields = { "^~\\&","SENDER","FAC","RECV","FAC",
                   "20260522120000","","ORM^O01",
                   "MSG00042","P","2.5" };
    orm.segments.push_back(msh);

    auto ack  = HL7::build_ack(orm, HL7::AckCode::AA);
    auto nack = HL7::build_ack(orm, HL7::AckCode::AE,
                               "Required field MSH.3 missing");

    std::cout << "ACK wire:\n" << ack.to_wire()  << '\n';
    std::cout << "NACK wire:\n"<< nack.to_wire() << '\n';
}

#ifdef USE_IO_URING
// ── D: io_uring MLLP server ──────────────────────────────────
void run_uring_server() {
    IoUringMLLPServer server(2575,
        [](int fd, HL7::Message msg) {
            std::cout << "[io_uring] rx fd=" << fd
                      << " type=" << msg.message_type() << '\n';
            // Auto-reply AA
            auto ack  = HL7::build_ack(msg, HL7::AckCode::AA);
            auto wire = ack.to_wire();
            ::write(fd, wire.data(), wire.size());
        },
        512, false);

    std::cout << "io_uring MLLP server on :2575\n";
    server.run();
}
#endif

int main() {
    demo_ack_nack();
    // run_epoll_client();
    // run_uring_server();
}
*/

} // namespace LowLatency
