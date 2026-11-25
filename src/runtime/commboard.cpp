// Register behavior from MAME's sega/m3comm.cpp (BSD-3-Clause, MetalliC)
// and Supermodel's Src/Network/NetBoard.cpp (GPL-3.0-or-later, Copyright
// 2011-2020 Bart Trzynadlowski, Nik Henson, Ian Curtis, Harry Tuttle, and
// Spindizzi), see commboard.hpp.
#include "runtime/commboard.hpp"
#include "runtime/state.hpp"

#include "runtime/moira_state.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
std::atomic<bool> g_wait_skip{!std::getenv("SPIKEOUT_NO_IDLE_SKIP")}; // (CommBoard::set_wait_skip)
} // namespace

namespace m3 {

namespace {

constexpr uint16_t swap16(uint16_t v) { return static_cast<uint16_t>(v << 8 | v >> 8); }

constexpr uint64_t kSlice = 256; // 68000 cycles run at a time, at most

// t * to / from without overflowing in a long run.
constexpr uint64_t rescale(uint64_t t, uint64_t from, uint64_t to) { return t / from * to + t % from * to / from; }

// Main board view: 0xC0000000 comm RAM, 0xC0010000 I/O, 0xC0020000 68000 RAM.
enum HostRegion : uint32_t { kHostComm = 0, kHostIo = 1 };
uint32_t host_region(uint32_t addr) { return (addr & 0x3FFFF) >> 16; }
uint32_t host_io_index(uint32_t addr) { return (addr & 0x1FF) / 4; }

// I/O register word indices (byte offset / 2).
constexpr uint32_t kIoAck = 0x00 / 2;
constexpr uint32_t kIoRxStatus = 0x10 / 2, kIoRxEnable = 0x14 / 2, kIoRxIrq = 0x16 / 2;
constexpr uint32_t kIoTxStatus = 0x18 / 2, kIoTxEnable = 0x1A / 2, kIoTxIrq = 0x1C / 2;
constexpr uint32_t kIoRxOffset = 0x40 / 2, kIoRxSize = 0x42 / 2, kIoTxOffset = 0x44 / 2, kIoTxSize = 0x46 / 2;
constexpr uint32_t kIoIrqStatus = 0x82 / 2, kIoStatus0 = 0x88 / 2, kIoRun = 0xC0 / 2;

} // namespace

class CommBoard::Cpu : public SavedMoira {
public:
    explicit Cpu(CommBoard& board) : board_(board) {}
    moira::u8 read8(moira::u32 a) const override { return board_.cpu_read8(a); }
    moira::u16 read16(moira::u32 a) const override { return board_.cpu_read16(a); }
    void write8(moira::u32 a, moira::u8 v) const override { board_.cpu_write8(a, v); }
    void write16(moira::u32 a, moira::u16 v) const override { board_.cpu_write16(a, v); }
    void willInterrupt(moira::u8 level) override { ++board_.irqs_taken[level & 7]; }

    // The firmware's waits (the 68000's own RAM, where it spends most of its time): gone round at once as many
    // times as they would before `stop`, their cycles (and a counted wait's counters) as if run. Each is a loop
    // that reads a byte only an interrupt sets; inside a run nothing else can (the main board's accesses come
    // between runs, the IRQ lines are set between slices), and with nothing pending (Moira's fast path, flags 0)
    // no interrupt is taken. One time round is run first, so the CPU is as every time round leaves it (flags,
    // prefetch); the rest the same state, its clock on. Moira runs what is left of the slice.
    //   wait:     tst.b $a.w; beq.s wait                 (0x0E8C, 0x1198, 0x11A0)
    //   counted:  addq.w #1,Dc; addq.w #1,Dn; tst.b $a.w; bne.w out; cmpi.w #n,Dn; blt.s counted   (0x0C52)
    void skip_waits(moira::i64 stop) {
        if (flags || clock >= stop) return;
        const auto& ram = board_.ram_;
        const uint32_t pc = reg.pc;
        if (pc >= 0xFFF0) return;
        auto word = [&](uint32_t a) { return static_cast<uint16_t>(ram[a] << 8 | ram[a + 1]); };
        int instructions = 0, counter = -1, count = -1;
        uint32_t watched = 0, limit = 0;
        if (word(pc) == 0x4A38 && word(pc + 4) == 0x67FA) {
            instructions = 2, watched = word(pc + 2);
        } else if ((word(pc) & 0xFFF8) == 0x5240 && (word(pc + 2) & 0xFFF8) == 0x5240 && word(pc + 4) == 0x4A38 &&
                   word(pc + 8) == 0x6600 && (word(pc + 12) & 0xFFF8) == 0x0C40 && (word(pc + 12) & 7) == (word(pc + 2) & 7) &&
                   word(pc + 16) == 0x6DEE) {
            instructions = 6, watched = word(pc + 6), counter = word(pc) & 7, count = word(pc + 2) & 7, limit = word(pc + 14);
            if (counter == count) return;
        } else {
            return;
        }
        if (watched >= 0x8000 || ram[watched] != 0) return; // (abs.w: below 0x8000 the 68000's own RAM)
        auto counting = [&] { // (its count, a word, at or above 0 and under the limit once more: the flags after its cmpi then
                              // N, C set, Z, V clear, X clear from its addq)
            const auto n = static_cast<int16_t>(reg.r[count] & 0xFFFF);
            return n >= 0 && n < static_cast<int16_t>(limit) - 1 && static_cast<int16_t>(limit) > 0;
        };
        if (count >= 0 && !counting()) return;
        const moira::i64 from = clock;
        for (int i = 0; i < instructions; ++i) { // (one time round, as executeUntil would)
            if (clock >= stop) return;
            execute();
            if (flags) return;
        }
        if (reg.pc != pc || ram[watched] != 0) return;
        const moira::i64 round = clock - from;
        if (round <= 0 || clock >= stop) return;
        if (count >= 0 && (!counting() || !reg.sr.n || reg.sr.z || reg.sr.v || !reg.sr.c || reg.sr.x)) return;
        moira::i64 times = (stop - clock - 1) / round; // (each still started before stop)
        if (count >= 0) {
            const auto n = static_cast<int16_t>(reg.r[count] & 0xFFFF);
            times = std::min<moira::i64>(times, static_cast<int16_t>(limit) - 1 - n); // (its count still under the limit after)
            const auto add = [&](int r) {
                reg.r[r] = (reg.r[r] & 0xFFFF0000u) | ((reg.r[r] + static_cast<uint32_t>(times)) & 0xFFFFu);
            };
            add(counter), add(count);
        }
        clock += times * round;
        skipped += static_cast<uint64_t>(times);
    }
    uint64_t skipped = 0; // (times round the waits not run)

private:
    CommBoard& board_;
};

CommBoard::CommBoard(std::unique_ptr<NetLink> link, const Options& options)
    : options_(options), link_(link ? std::move(link) : std::make_unique<LoopbackLink>()),
      cpu_(std::make_unique<Cpu>(*this)), ram_(0x10000, 0), comm_(0x20000, 0), late_(kLateTable, 0),
      tables_(kTables * kMaxNodes * kPart, 0) {
    if (options_.cut_through) options_.ring_second_pass = options_.slave_table_late = options_.count_matched = false;
}

CommBoard::~CommBoard() = default;

void CommBoard::set_wait_skip(bool on) { g_wait_skip.store(on, std::memory_order_relaxed); }
uint64_t CommBoard::waits_skipped() const { return cpu_->skipped; }
uint32_t CommBoard::pc() const { return cpu_->getPC(); }
uint64_t CommBoard::cpu_cycles() const { return static_cast<uint64_t>(cpu_->getClock()); }

std::string CommBoard::disassemble(uint32_t addr, uint32_t* length) const {
    char buf[128] = {};
    const int n = cpu_->disassemble(buf, addr);
    if (length) *length = static_cast<uint32_t>(n);
    return buf;
}

std::string CommBoard::describe() const {
    char buf[400];
    std::snprintf(buf, sizeof buf,
                  "68000 board, %s, PC %06x, %llu cycles; status0 %04x, status1 %04x; IRQs 2:%llu 4:%llu 5:%llu "
                  "6:%llu; packets %llu out, %llu in, %llu second passes; table reads %llu of the count, %llu earlier, "
                  "%llu as came; set up as %s",
                  in_reset_ ? "held in reset" : "running", pc(), static_cast<unsigned long long>(cpu_cycles()),
                  io_[kIoStatus0], io_[kIoStatus0 + 1], static_cast<unsigned long long>(irqs_taken[2]),
                  static_cast<unsigned long long>(irqs_taken[4]), static_cast<unsigned long long>(irqs_taken[5]),
                  static_cast<unsigned long long>(irqs_taken[6]), static_cast<unsigned long long>(packets_sent),
                  static_cast<unsigned long long>(packets_received), static_cast<unsigned long long>(ring_passes),
                  static_cast<unsigned long long>(table_reads_exact), static_cast<unsigned long long>(table_reads_earlier),
                  static_cast<unsigned long long>(table_reads_as_came), ram_[0x400] == 0 && ram_[0x401] == 0 ? "master" : "slave");
    return buf;
}

uint8_t* CommBoard::bank(bool host) {
    const bool other = options_.host_sees_other_bank || options_.double_buffer;
    const bool second = (commbank_ & 1) != (host && other);
    return comm_.data() + (second ? 0x10000 : 0);
}

uint8_t& CommBoard::host_ram(uint32_t addr, uint32_t byte) {
    const uint32_t word = ((addr & 0x1FFFF) / 4) * 2; // 68000 byte address of the word
    return ram_[(word + (byte == 0 ? 1 : 0)) & 0xFFFF]; // PowerPC byte 0 is the 68000's low byte
}

// ---- Time ----

void CommBoard::sync(uint64_t main_cycles) {
    const uint64_t target = rescale(main_cycles, options_.main_hz, options_.cpu_hz);
    if (in_reset_) {
        cpu_->setClock(static_cast<moira::i64>(target));
        return;
    }
    run_until(target);
}

void CommBoard::run_until(uint64_t target) {
    const uint64_t irq5_period = options_.cpu_hz / options_.irq5_hz;
    const bool timed = options_.irq5_per_frame == 0;
    while (!in_reset_ && cpu_cycles() < target) {
        if (send_irq_armed_ && send_complete()) set_irq(4, true); // transmit complete
        // Short runs, so what the firmware starts (a transmission, arming
        // the receiver) and what comes in over the link are timely.
        const uint64_t stop =
            std::min({target, timed ? next_irq5_ : UINT64_MAX, frame_irq5_next_, cpu_cycles() + kSlice});
        if (g_wait_skip.load(std::memory_order_relaxed)) cpu_->skip_waits(static_cast<moira::i64>(stop));
        cpu_->executeUntil(static_cast<moira::i64>(stop));
        if (send_irq_armed_ && send_complete()) set_irq(4, true);
        if (timed && cpu_cycles() >= next_irq5_) {
            set_irq(5, true);
            next_irq5_ += irq5_period;
        }
        if (cpu_cycles() >= frame_irq5_next_) {
            set_irq(5, true);
            frame_irq5_next_ = --frame_irq5_left_ ? frame_irq5_next_ + frame_irq5_period_ : UINT64_MAX;
        }
        deliver_receive();
    }
}

// IRQ 5 locked to the main board's frames: the first at the end of each
// frame (the board has caught up to it), the others spread over the next.
// Spikeout's link checks lock-step every frame (each cabinet's data must be
// the one from exactly one frame before, "Sync_sys_broken" otherwise): with
// MAME's free-running 100 Hz a lone master fails it within seconds, with
// IRQ 5 once a frame it doesn't. What drives it on the board isn't known.
void CommBoard::end_frame() {
    const uint32_t ticks = slave() && options_.slave_irq5_per_frame != CommBoardOptions::kSame ? options_.slave_irq5_per_frame
                                                                                              : options_.irq5_per_frame;
    if (in_reset_ || ticks == 0) return;
    const double frame_cycles = static_cast<double>(options_.cpu_hz) / options_.main_frame_hz;
    frame_irq5_period_ = static_cast<uint64_t>(frame_cycles / ticks);
    if (options_.irq5_phase > 0) { // the first a part of a frame after its end
        frame_irq5_left_ = ticks;
        frame_irq5_next_ = cpu_cycles() + static_cast<uint64_t>(frame_cycles * options_.irq5_phase);
        return;
    }
    set_irq(5, true);
    frame_irq5_left_ = ticks - 1;
    frame_irq5_next_ = frame_irq5_left_ ? cpu_cycles() + frame_irq5_period_ : UINT64_MAX;
}

void CommBoard::release_reset() {
    in_reset_ = false;
    const moira::i64 now = cpu_->getClock();
    cpu_->reset(); // fetches SSP and PC from 68000 address 0: the uploaded firmware
    cpu_->setClock(now);
    irq_lines_ = 0;
    update_ipl();
    next_irq5_ = cpu_cycles() + options_.cpu_hz / options_.irq5_hz;
}

void CommBoard::set_irq(int level, bool on) {
    const uint8_t bit = static_cast<uint8_t>(1u << level);
    irq_lines_ = on ? (irq_lines_ | bit) : (irq_lines_ & ~bit);
    update_ipl();
}

void CommBoard::update_ipl() {
    int level = 0;
    for (int l = 7; l > 0; --l)
        if (irq_lines_ & (1 << l)) {
            level = l;
            break;
        }
    cpu_->setIPL(static_cast<moira::u8>(level));
}

// ---- Link ----

// The ring's second pass (ring_second_pass), taken in as it comes, whatever the firmware is doing: it isn't the
// firmware's. A slave `hop` nodes on from the master puts in its data frame's parts after its own hop (the nodes after
// it and itself, which on the first pass came from last exchange), where the main board reads them, and passes it on a
// node's part on, as the first pass; the master, its own pass come back, drops it.
void CommBoard::take_passes() {
    for (;;) {
        const std::vector<uint8_t>* next = link_->peek();
        if (!next || !ring_pass(*next)) return;
        const std::vector<uint8_t> pass = link_->receive();
        if (!slave()) { // (its own pass come back, a node on at each cabinet: the ring's cabinets)
            nodes_ = pass[8] <= kMaxNodes ? pass[8] : 0, hop_ = 0;
            view_dirty_ = true;
            continue;
        }
        auto field = [&](std::size_t k) { return static_cast<std::size_t>(pass[9 + 2 * k] | pass[10 + 2 * k] << 8); };
        const std::size_t hop = pass[8], offset = field(0), size = field(1), part = field(2);
        const std::size_t stream = pass.size() - kRingPassHeader;
        if (part == 0 || offset < part || offset >= 0x10000u) continue;
        const uint8_t* in = pass.data() + kRingPassHeader;
        uint8_t* comm = options_.double_buffer ? bank(true) : bank(false);
        const std::size_t from = hop * part, to = std::min({stream, size, 0x10000u - offset});
        if (options_.slave_table_late && !options_.count_matched && late_size_) { // (into the table being built: shown
                                                                                  // at the next data frame)
            const std::size_t end = std::min<std::size_t>(to, late_size_);
            if (from < end) std::memcpy(late_.data() + from, in + from, end - from);
        } else if (from < to) std::memcpy(comm + offset + from, in + from, to - from);
        if (options_.count_matched) {
            // (the parts after this cabinet's own, positions hop to the ring's last, from the master's data frame come
            // back: into the newest table if it is this exchange's (the master's count), the parts before them came in
            // the data frame)
            const uint8_t n = pass[kRingPassNodes] <= kMaxNodes ? pass[kRingPassNodes] : 0;
            if (n != nodes_ || hop != hop_) view_dirty_ = true;
            nodes_ = n, hop_ = static_cast<uint8_t>(hop);
            const uint32_t count = static_cast<uint32_t>(pass[kRingPassCount]) | uint32_t{pass[kRingPassCount + 1]} << 8 |
                                   uint32_t{pass[kRingPassCount + 2]} << 16 | uint32_t{pass[kRingPassCount + 3]} << 24;
            int newest = -1;
            for (int t = 0; t < static_cast<int>(kTables); ++t)
                if (table_seq_[t] && (newest < 0 || table_seq_[t] > table_seq_[newest])) newest = t;
            if (newest >= 0 && table_count_[newest] == count && n > hop && part <= kPart) {
                for (std::size_t j = hop; j + 1 < n; ++j) { // (j = n - 1 is this cabinet's own, in already)
                    if (j * part + kBlock > stream) break;
                    const std::size_t at = hop + n - 1 - j;
                    table_put(newest, at, in + j * part, std::min(part, stream - j * part));
                    table_have_[newest] = static_cast<uint8_t>(table_have_[newest] | 1u << at);
                }
                view_dirty_ = true;
            }
        }
        // (on, as a slave's data frame: its own part first, then what came, a node's part on)
        std::vector<uint8_t> on(pass.begin(), pass.begin() + static_cast<std::ptrdiff_t>(kRingPassHeader));
        on[8] = static_cast<uint8_t>(hop + 1);
        const uint8_t* own = bank(false) + (offset - part);
        on.insert(on.end(), own, own + part);
        if (stream > part) on.insert(on.end(), in, in + (stream - part));
        link_->send(on.data(), on.size());
        ++ring_passes;
    }
}

// ---- Options::cut_through ----

void CommBoard::send_at(uint64_t cycle, const uint8_t* data, std::size_t size) {
    link_->set_time(rescale(cycle, options_.cpu_hz, options_.main_hz));
    link_->send(data, size);
    ++packets_sent;
}

// What has come in of the data frame now going out, passed on behind this node's own part, up to the frame's length.
// Each byte leaves no sooner than the wire allows from the frame's start, nor before it came in.
void CommBoard::stream_relay() {
    if (!tx_stream_) return;
    const std::size_t out = std::min<std::size_t>(send_size_ & 0x7FFFu, 0x10000u - send_offset_);
    const std::size_t part = std::size_t{recv_offset_} - send_offset_;
    const std::size_t have = std::min<std::size_t>(out, part + rx_got_);
    if (have > tx_sent_) {
        const uint64_t at = std::max(cpu_cycles(), tx_start_ + static_cast<uint64_t>(options_.link_cycles_per_byte * static_cast<double>(have)));
        std::vector<uint8_t> p(kStreamHeader);
        std::copy(std::begin(kStreamMagic), std::end(kStreamMagic), p.begin());
        for (int k = 0; k < 4; ++k) p[8 + k] = static_cast<uint8_t>(tx_sent_ >> (8 * k));
        const uint8_t* from = bank(false) + send_offset_;
        p.insert(p.end(), from + tx_sent_, from + have);
        send_at(at, p.data(), p.size());
        tx_sent_ = static_cast<uint32_t>(have);
        if (tx_sent_ >= out) tx_stream_ = false, send_done_ = at; // (all gone: transmit complete from then)
    }
}

// A data frame's bytes from `offset` on: into the receive area of the bank the 68000 has selected (the main board sees
// them after the next bank flip), passed on, and receive complete once its length is in.
void CommBoard::stream_take(const uint8_t* data, std::size_t size, std::size_t offset) {
    const std::size_t len = std::min<std::size_t>(recv_size_ & 0x7FFFu, 0x10000u - recv_offset_);
    if (offset < len) std::memcpy(bank(false) + recv_offset_ + offset, data, std::min(size, len - offset));
    rx_got_ = static_cast<uint32_t>(std::max<std::size_t>(rx_got_, std::min(offset + size, len)));
    stream_relay();
    if (rx_got_ >= len) {
        rx_stream_ = false;
        receive_armed_ = false;
        send_waiting_ = false;
        set_irq(6, true); // receive complete
    }
}

void CommBoard::stream_receive() {
    for (;;) {
        const std::vector<uint8_t>* next = link_->peek();
        if (!next) return;
        if (ring_pass(*next)) { // (another link model's, from a state saved with it: not this one's)
            link_->receive();
            continue;
        }
        if (m3::stream_relay(*next)) {
            const std::vector<uint8_t> p = link_->receive();
            if (!rx_stream_) continue; // (the rest of a frame already whole here, or one given up on)
            const std::size_t offset = p[8] | std::size_t{p[9]} << 8 | std::size_t{p[10]} << 16 | std::size_t{p[11]} << 24;
            stream_take(p.data() + kStreamHeader, p.size() - kStreamHeader, offset);
            continue;
        }
        // (a plain packet: a data frame's start, or a frame of its own: the sync)
        if (!receive_enabled_ || !receive_armed_) return;
        std::vector<uint8_t> packet = link_->receive();
        if (packet.empty()) return; // (the link is down)
        ++packets_received;
        if (data_descriptor()) {
            rx_stream_ = true, rx_got_ = 0;
            stream_take(packet.data(), packet.size(), 0);
            continue;
        }
        const std::size_t size = std::min<std::size_t>({packet.size(), recv_size_ & 0x7FFFu, 0x10000u - recv_offset_});
        receive_armed_ = false;
        send_waiting_ = false;
        std::memcpy(bank(false) + recv_offset_, packet.data(), size);
        set_irq(6, true); // receive complete
    }
}

void CommBoard::deliver_receive() {
    link_->set_time(rescale(cpu_cycles(), options_.cpu_hz, options_.main_hz));
    if (cut()) {
        stream_receive();
        return;
    }
    take_passes();
    if (!receive_enabled_ || !receive_armed_) return;
    std::vector<uint8_t> packet = link_->receive();
    if (packet.empty()) return; // nothing yet (or the link is down)
    ++packets_received;
    // (double_buffer: a data frame, into the bank the main board sees)
    const bool data_frame = recv_offset_ > send_offset_ && recv_offset_ < send_offset_ + (send_size_ & 0x7FFFu);
    uint8_t* comm = options_.double_buffer && data_frame ? bank(true) : bank(false);
    const std::size_t size = std::min<std::size_t>({packet.size(), recv_size_ & 0x7FFFu, 0x10000u - recv_offset_});
    receive_armed_ = false;
    send_waiting_ = false;
    const std::size_t part = recv_offset_ > send_offset_ ? std::size_t{recv_offset_} - send_offset_ : 0;
    if (data_frame && part && part <= kPart) { // (Options::count_matched: where the main board reads the nodes)
        if (view_base_ != recv_offset_ || view_send_ != send_offset_ || part_ != part) view_dirty_ = true;
        view_base_ = recv_offset_, view_send_ = send_offset_, part_ = static_cast<uint16_t>(part);
    }
    const bool tables = options_.count_matched && data_frame && part && part <= kPart && nodes_ >= 2 && nodes_ <= kMaxNodes &&
                        size >= (std::size_t{nodes_} - 1) * part + kBlock;
    if (send_done_ == kForwarding) { // a slave's frame, with what came in where it lands: on to the next
        const std::size_t out = std::min<std::size_t>(send_size_ & 0x7FFFu, 0x10000u - send_offset_);
        std::vector<uint8_t> frame(bank(false) + send_offset_, bank(false) + send_offset_ + out);
        // (where it lands, inside the frame: a loaded state's offsets aren't the firmware's, safe loading)
        const std::size_t at = recv_offset_ >= send_offset_ ? std::size_t{recv_offset_} - send_offset_ : out;
        if (at < out) std::memcpy(frame.data() + at, packet.data(), std::min(size, out - at));
        send_done_ = cpu_cycles() + static_cast<uint64_t>(options_.link_cycles_per_byte * static_cast<double>(at));
        transmit(frame.data(), out);
        if (tables && hop_ >= 1 && hop_ < nodes_ && at == part && out >= part) {
            // (count_matched: this exchange's table begun: the parts before this cabinet's (the master's and the
            // slaves' between, this exchange's: each put in front as it passed on) and its own as it goes out; the
            // ones after it come with the second pass)
            const std::size_t h = hop_;
            const int t = new_table(part_count(packet.data() + (h - 1) * part));
            for (std::size_t j = 0; j < h; ++j) table_put(t, h - 1 - j, packet.data() + j * part, std::min(part, size - j * part));
            table_put(t, h, frame.data(), part);
            table_have_[t] = static_cast<uint8_t>((1u << (h + 1)) - 1);
        }
        if (options_.slave_table_late && !options_.count_matched && options_.ring_second_pass && data_frame && size <= kLateTable) {
            // (slave_table_late: the main board shown the table of the exchange before, the one built since; this
            // exchange's built in its place, from this frame and its second pass; the first one: as it came)
            const bool had = late_size_ != 0;
            if (had) std::memcpy(comm + recv_offset_, late_.data(), std::min<std::size_t>(size, late_size_));
            std::memcpy(late_.data(), packet.data(), size);
            late_size_ = static_cast<uint32_t>(size);
            if (had) {
                set_irq(6, true); // receive complete
                return;
            }
        }
    } else if (options_.ring_second_pass && !slave() && data_frame && options_.ring_forwarding) {
        // The master's data frame come back round, every node's part of this exchange in it: on once more
        // (ring_second_pass), for the slaves' parts after their own.
        const std::size_t out = std::min<std::size_t>(send_size_ & 0x7FFFu, 0x10000u - send_offset_);
        std::vector<uint8_t> pass(kRingPassHeader);
        std::copy(std::begin(kRingPassMagic), std::end(kRingPassMagic), pass.begin());
        pass[8] = 1;
        const uint16_t fields[3] = {recv_offset_, static_cast<uint16_t>(recv_size_ & 0x7FFFu), static_cast<uint16_t>(part)};
        for (int k = 0; k < 3; ++k) pass[9 + 2 * k] = static_cast<uint8_t>(fields[k]), pass[10 + 2 * k] = static_cast<uint8_t>(fields[k] >> 8);
        pass[kRingPassNodes] = nodes_;
        const uint32_t count = part >= kNodeCount + 4 ? part_count(bank(false) + send_offset_) : 0; // (its own part's)
        for (int k = 0; k < 4; ++k) pass[kRingPassCount + k] = static_cast<uint8_t>(count >> (8 * k));
        if (tables) { // (count_matched: the whole table of this exchange, positions from the ring's last back to itself)
            const int t = new_table(count);
            for (std::size_t j = 0; j < nodes_; ++j)
                table_put(t, nodes_ - 1u - j, packet.data() + j * part, std::min(part, size - j * part));
            table_have_[t] = static_cast<uint8_t>((1u << nodes_) - 1);
        }
        pass.insert(pass.end(), bank(false) + send_offset_, bank(false) + send_offset_ + part);
        pass.insert(pass.end(), packet.begin(), packet.begin() + static_cast<std::ptrdiff_t>(std::min(packet.size(), out - part)));
        link_->set_time(rescale(cpu_cycles(), options_.cpu_hz, options_.main_hz));
        link_->send(pass.data(), pass.size());
        ++ring_passes;
    }
    if (options_.count_matched && data_frame) show_whole_tables(); // (the exchanges' tables whole by now: shown from here)
    std::memcpy(comm + recv_offset_, packet.data(), size);
    set_irq(6, true); // receive complete
}

// options_.ring_forwarding: a slave sending a frame its receive area lies
// inside (the firmware's data frame: out from 0x100, in at 0xD00, a node's
// part on; it arms the receiver just after) sends as the next frame comes
// in, as a ring's node shifts on what it receives.
bool CommBoard::forwards(std::size_t size) const {
    return options_.ring_forwarding && slave() && recv_offset_ > send_offset_ && recv_offset_ < send_offset_ + size;
}

void CommBoard::transmit(const uint8_t* data, std::size_t size) {
    link_->set_time(rescale(send_done_, options_.cpu_hz, options_.main_hz));
    link_->send(data, size);
    ++packets_sent;
}

// ---- Options::count_matched ----

// (the PowerPC's 32-bit word, its bytes where the main board reads them: comm RAM byte n is its byte n ^ comm_ram_xor;
// a part starts on a word)
uint32_t CommBoard::part_count(const uint8_t* part) const {
    uint32_t v = 0;
    for (uint32_t i = 0; i < 4; ++i) v = v << 8 | part[(kNodeCount + i) ^ options_.comm_ram_xor];
    return v;
}

int CommBoard::new_table(uint32_t count) {
    int t = 0;
    for (int k = 1; k < static_cast<int>(kTables); ++k)
        if (table_seq_[k] < table_seq_[t]) t = k;
    table_count_[t] = count, table_seq_[t] = ++table_seq_next_, table_have_[t] = 0, table_shown_[t] = 0;
    std::fill_n(table_len_.begin() + t * static_cast<int>(kMaxNodes), kMaxNodes, uint16_t{0});
    view_dirty_ = true;
    return t;
}

void CommBoard::show_whole_tables() {
    const uint8_t whole = static_cast<uint8_t>(nodes_ >= 2 && nodes_ <= kMaxNodes ? (1u << nodes_) - 1 : 0xFF);
    for (std::size_t t = 0; t < kTables; ++t)
        if (table_seq_[t] && !table_shown_[t] && (table_have_[t] & whole) == whole) table_shown_[t] = 1, view_dirty_ = true;
}

// The newest whole table of the count this cabinet last wrote; none: the newest whole one of the latest count before
// it (never a later one); none: -1.
int CommBoard::chosen_table() {
    if (!view_dirty_) return view_;
    view_dirty_ = false, view_ = -1;
    if (own_known_ != 0xF || nodes_ < 2 || nodes_ > kMaxNodes || part_ == 0) return view_;
    const uint32_t own = own_count();
    const uint8_t whole = static_cast<uint8_t>((1u << nodes_) - 1);
    int32_t best_d = 0;
    for (int t = 0; t < static_cast<int>(kTables); ++t) {
        if (!table_seq_[t] || !table_shown_[t] || (table_have_[t] & whole) != whole) continue;
        const auto d = static_cast<int32_t>(table_count_[t] - own); // (0: its count; below: an earlier one)
        if (d > 0) continue;
        if (view_ < 0 || d > best_d || (d == best_d && table_seq_[t] > table_seq_[view_])) view_ = t, best_d = d;
    }
    return view_;
}

uint8_t CommBoard::host_view(uint32_t at, bool count) {
    at &= 0xFFFF;
    if (options_.count_matched && part_ && at >= view_base_ && at < view_base_ + std::size_t{nodes_} * part_) {
        const uint32_t j = (at - view_base_) / part_, off = (at - view_base_) % part_;
        const int t = chosen_table();
        // (once a read of the table, at the first node's count; "earlier" and "as came" also while the cabinets aren't
        // linked in play: each its own active node, their counts apart)
        if (count && j == 0 && off == (kNodeCount ^ options_.comm_ram_xor))
            ++(t < 0 ? table_reads_as_came : table_count_[t] == own_count() ? table_reads_exact : table_reads_earlier);
        // (position j: the node j + 1 back up the ring from this one; the whole part from the one exchange, the area
        // state past the records too: it goes with the header's flags that ask for it)
        const uint32_t node = (hop_ + 2u * nodes_ - 1u - j) % nodes_;
        if (t >= 0 && off < table_len_[std::size_t(t) * kMaxNodes + node]) return table_part(t, node)[off];
    }
    return bank(true)[at];
}

// ---- 68000 side ----

void CommBoard::note(uint32_t addr, uint16_t value, bool write, uint8_t size) {
    if (trace.size() < trace_limit) trace.push_back({cpu_cycles(), cpu_->getPC0(), addr, value, write, size});
}

uint16_t CommBoard::ctrl_read(uint32_t reg) const { return reg == 0 ? commbank_ : 0; }

void CommBoard::ctrl_write(uint32_t reg, uint16_t v) {
    ctrl_[(reg & 0xFF) / 2] = v;
    switch (reg & 0xFE) {
    case 0x00: commbank_ = v; break; // flipped by the firmware's IRQ 2 and IRQ 5 handlers
    case 0x40: set_irq(5, false); break;
    case 0xA0: // IRQ 2 ack; the main board then sees bit 0 of I/O register 0 (Supermodel's guess)
        set_irq(2, false);
        io_[kIoAck] |= 1;
        break;
    case 0xE0: // written by a slave's IRQ 6 handler: its tick (see Options::irq5_on_e0)
        if (options_.irq5_on_e0) set_irq(5, true);
        break;
    default: break; // 0x80 LEDs, 0xC0 (0x7FFF, or a value from a table by node: a timer?)
    }
}

uint16_t CommBoard::io_read(uint32_t index) const {
    switch (index) {
    case kIoRxStatus:
    case kIoTxStatus: return options_.link_status;
    case kIoIrqStatus: return 0xA0; // MAME
    default: return io_[index & 0x7F];
    }
}

void CommBoard::io_write(uint32_t index, uint16_t v, uint16_t mask, bool from_host) {
    index &= 0x7F;
    const uint16_t old = io_[index];
    io_[index] = static_cast<uint16_t>((old & ~mask) | (v & mask));
    const uint16_t low = io_[index] & 0xFF;
    if (from_host) {
        if (index == kIoStatus0) set_irq(2, true); // MAME: a command from the main board raises IRQ 2
        if (index == kIoRun) {
            if (io_[index] == 0) {
                in_reset_ = true;
            } else if (in_reset_) {
                release_reset();
            }
        }
        return;
    }
    switch (index) {
    case kIoRxEnable:
        receive_enabled_ = low == 0x80;
        if (low != 0x80) set_irq(6, false); // also how the IRQ 6 handler acknowledges
        deliver_receive();
        break;
    case kIoRxIrq:
        receive_armed_ = low == 0x8C;
        if (cut() && receive_armed_) rx_stream_ = false, rx_got_ = 0; // (armed anew: a frame from its start)
        deliver_receive();
        break;
    case kIoTxEnable:
        if (low == 0x80) { // data transfer enable: send now; it arrives once all of it has gone
            const std::size_t size = std::min<std::size_t>(send_size_ & 0x7FFFu, 0x10000u - send_offset_);
            send_waiting_ = options_.send_waits_for_receive;
            if (cut() && data_descriptor()) { // (its own part now, then what comes in as it comes: stream_relay)
                const std::size_t part = std::size_t{recv_offset_} - send_offset_;
                if (!rx_stream_) rx_got_ = 0; // (nothing of this exchange in yet)
                tx_stream_ = true, tx_start_ = cpu_cycles(), tx_sent_ = static_cast<uint32_t>(std::min(part, size));
                send_done_ = kForwarding;
                send_at(tx_start_ + static_cast<uint64_t>(options_.link_cycles_per_byte * static_cast<double>(tx_sent_)),
                        bank(false) + send_offset_, tx_sent_);
                if (tx_sent_ >= size) tx_stream_ = false, send_done_ = tx_start_ + static_cast<uint64_t>(options_.link_cycles_per_byte * static_cast<double>(tx_sent_));
                stream_relay();
                deliver_receive();
                break;
            }
            if (forwards(size)) { // passed on as it comes in (deliver_receive)
                send_done_ = kForwarding;
                deliver_receive();
                break;
            }
            send_done_ = cpu_cycles() + static_cast<uint64_t>(options_.link_cycles_per_byte * static_cast<double>(size));
            transmit(bank(false) + send_offset_, size);
            deliver_receive();
        }
        break;
    case kIoTxIrq: // transmit complete while 0x8C (MAME), once the frame has gone
        send_irq_armed_ = low == 0x8C;
        set_irq(4, send_irq_armed_ && send_complete());
        break;
    case kIoRxOffset: recv_offset_ = static_cast<uint16_t>(recv_offset_ >> 8 | low << 8); break;
    case kIoRxSize:   recv_size_ = static_cast<uint16_t>(recv_size_ >> 8 | low << 8); break;
    case kIoTxOffset: send_offset_ = static_cast<uint16_t>(send_offset_ >> 8 | low << 8); break;
    case kIoTxSize:   send_size_ = static_cast<uint16_t>(send_size_ >> 8 | low << 8); break;
    default: break;
    }
}

uint8_t CommBoard::cpu_read8(uint32_t a) {
    switch ((a >> 16) & 0xF) {
    case 0x0: return ram_[a & 0xFFFF];
    case 0x4: {
        const uint16_t v = ctrl_read(a & 0xFE);
        note(a, v, false, 1);
        return static_cast<uint8_t>((a & 1) ? v : v >> 8);
    }
    case 0x8: return bank(false)[a & 0xFFFF];
    case 0xC: {
        const uint16_t v = io_read((a & 0xFF) / 2);
        note(a, v, false, 1);
        return static_cast<uint8_t>((a & 1) ? v : v >> 8);
    }
    default: return 0;
    }
}

uint16_t CommBoard::cpu_read16(uint32_t a) {
    a &= ~1u;
    switch ((a >> 16) & 0xF) {
    case 0x0: return static_cast<uint16_t>(ram_[a & 0xFFFF] << 8 | ram_[(a + 1) & 0xFFFF]);
    case 0x4: {
        const uint16_t v = ctrl_read(a & 0xFE);
        note(a, v, false, 2);
        return v;
    }
    case 0x8: {
        const uint8_t* comm = bank(false);
        return static_cast<uint16_t>(comm[a & 0xFFFF] << 8 | comm[(a + 1) & 0xFFFF]);
    }
    case 0xC: {
        const uint16_t v = io_read((a & 0xFF) / 2);
        note(a, v, false, 2);
        return v;
    }
    default: return 0;
    }
}

void CommBoard::cpu_write8(uint32_t a, uint8_t v) {
    switch ((a >> 16) & 0xF) {
    case 0x0: ram_[a & 0xFFFF] = v; break;
    case 0x4:
        note(a, v, true, 1);
        ctrl_write(a & 0xFE, (a & 1) ? v : static_cast<uint16_t>(v << 8)); // byte lane of a 16-bit register
        break;
    case 0x8: bank(false)[a & 0xFFFF] = v; break;
    case 0xC:
        note(a, v, true, 1);
        io_write((a & 0xFF) / 2, (a & 1) ? v : static_cast<uint16_t>(v << 8), (a & 1) ? 0x00FF : 0xFF00, false);
        break;
    default: break;
    }
}

void CommBoard::cpu_write16(uint32_t a, uint16_t v) {
    a &= ~1u;
    switch ((a >> 16) & 0xF) {
    case 0x0:
        ram_[a & 0xFFFF] = static_cast<uint8_t>(v >> 8);
        ram_[(a + 1) & 0xFFFF] = static_cast<uint8_t>(v);
        break;
    case 0x4:
        note(a, v, true, 2);
        ctrl_write(a & 0xFE, v);
        break;
    case 0x8: {
        uint8_t* comm = bank(false);
        comm[a & 0xFFFF] = static_cast<uint8_t>(v >> 8);
        comm[(a + 1) & 0xFFFF] = static_cast<uint8_t>(v);
        break;
    }
    case 0xC:
        note(a, v, true, 2);
        io_write((a & 0xFF) / 2, v, 0xFFFF, false);
        break;
    default: break;
    }
}

// ---- Main board side ----

void CommBoard::host_note(uint32_t addr, bool write, uint32_t value) {
    if (!host_tracing) return;
    const uint8_t region = static_cast<uint8_t>(host_region(addr));
    const uint8_t which = static_cast<uint8_t>(commbank_ & 1);
    const uint32_t a = addr & 0x3FFFF;
    const uint64_t now = cpu_cycles();
    if (!host_trace.empty() && region != kHostIo) { // a burst: the same kind of access, close by, soon after
        HostRun& r = host_trace.back();
        if (r.write == write && r.region == region && r.bank == which && now - r.last < 2000 && a + 64 >= r.lo && a <= r.hi + 64) {
            r.last = now, r.lo = std::min(r.lo, a), r.hi = std::max(r.hi, a);
            ++r.count;
            return;
        }
    }
    host_trace.push_back({now, now, a, a, value, 1, write, region, which});
}

// (read8 of each, without its trace)
bool CommBoard::host_comm_view(uint8_t* out, uint32_t bytes) {
    if (host_tracing || bytes > 0x10000) return false;
    for (uint32_t i = 0; i < bytes; ++i) out[i] = host_view(i ^ options_.comm_ram_xor, false);
    return true;
}

void CommBoard::bank_view(bool host_bank, uint8_t* out) const {
    const bool other = options_.host_sees_other_bank || options_.double_buffer;
    const bool host_second = (commbank_ & 1) != other; // (bank(true))
    const bool second = host_bank ? host_second : !host_second;
    const uint8_t* comm = comm_.data() + (second ? 0x10000 : 0);
    for (uint32_t i = 0; i < 0x10000; ++i) out[i] = comm[(i ^ options_.comm_ram_xor) & 0xFFFF];
}

uint8_t CommBoard::read8(uint32_t addr) {
    host_note(addr, false, 0);
    switch (host_region(addr)) {
    case kHostComm: return host_view(addr ^ options_.comm_ram_xor, true);
    case kHostIo: {
        const uint16_t v = io_read(host_io_index(addr));
        const uint32_t k = addr & 3; // the register is the upper half of the word, byte-swapped
        return k == 0 ? static_cast<uint8_t>(v) : k == 1 ? static_cast<uint8_t>(v >> 8) : 0;
    }
    default: return (addr & 2) ? 0 : host_ram(addr, addr & 1);
    }
}

uint16_t CommBoard::read16(uint32_t addr) {
    return static_cast<uint16_t>(read8(addr) << 8 | read8(addr + 1));
}

uint32_t CommBoard::read32(uint32_t addr) {
    uint32_t v = 0;
    for (uint32_t i = 0; i < 4; ++i) v = v << 8 | read8(addr + i);
    return v;
}

void CommBoard::write8(uint32_t addr, uint8_t data) {
    host_note(addr, true, data);
    switch (host_region(addr)) {
    case kHostComm: {
        host_comm(addr) = data;
        // (count_matched: the count this cabinet writes into its part's header, a byte at a time, the PowerPC's order)
        const uint32_t a = addr & 0xFFFF;
        if (view_send_ && a >= view_send_ + kNodeCount && a < view_send_ + kNodeCount + 4) {
            own_count_[a - view_send_ - kNodeCount] = data;
            own_known_ = static_cast<uint8_t>(own_known_ | 1u << (a - view_send_ - kNodeCount));
            view_dirty_ = true;
        }
        break;
    }
    case kHostIo: {
        const uint32_t k = addr & 3;
        if (k == 0) io_write(host_io_index(addr), data, 0x00FF, true);
        else if (k == 1) io_write(host_io_index(addr), static_cast<uint16_t>(data << 8), 0xFF00, true);
        break;
    }
    default:
        if (!(addr & 2)) {
            uint8_t& cell = host_ram(addr, addr & 1);
            const bool role = &cell == &ram_[0x400] || &cell == &ram_[0x401];
            cell = force_master && role ? 0 : data;
        }
        break;
    }
}

void CommBoard::write16(uint32_t addr, uint16_t data) {
    if (host_region(addr) == kHostIo && (addr & 3) == 0) { // one access to the whole register
        host_note(addr, true, data);
        io_write(host_io_index(addr), swap16(data), 0xFFFF, true);
        return;
    }
    write8(addr, static_cast<uint8_t>(data >> 8));
    write8(addr + 1, static_cast<uint8_t>(data));
}

void CommBoard::write32(uint32_t addr, uint32_t data) {
    if (host_region(addr) == kHostIo) { // the register is the upper half
        host_note(addr, true, data);
        io_write(host_io_index(addr), swap16(static_cast<uint16_t>(data >> 16)), 0xFFFF, true);
        return;
    }
    for (uint32_t i = 0; i < 4; ++i) write8(addr + i, static_cast<uint8_t>(data >> (24 - 8 * i)));
}

void CommBoard::state(State& s) {
    s.tag("commboard");
    link_->state(s);
    cpu_->cpu_state(s);
    s.fixed(ram_), s.fixed(comm_), s.io(io_), s.io(ctrl_), s.io(commbank_);
    s.io(in_reset_), s.io(irq_lines_), s.io(next_irq5_), s.io(frame_irq5_next_), s.io(frame_irq5_period_);
    s.io(frame_irq5_left_), s.io(recv_offset_), s.io(recv_size_), s.io(send_offset_), s.io(send_size_);
    s.io(receive_enabled_), s.io(receive_armed_), s.io(send_done_), s.io(send_irq_armed_), s.io(send_waiting_);
    s.io(irqs_taken), s.io(packets_sent), s.io(packets_received);
    // (Options::cut_through's data frames in and out, 2026-11-25: a state from before has none; saved first, so a state
    // of the tables' format with this section can't be read by a build from before it (kStateFormat 5))
    if (s.loading() && !s.tagged("commboard stream")) {
        rx_stream_ = tx_stream_ = false, rx_got_ = tx_sent_ = 0, tx_start_ = 0;
    } else {
        if (!s.loading()) s.tag("commboard stream");
        s.io(rx_stream_), s.io(rx_got_), s.io(tx_stream_), s.io(tx_sent_), s.io(tx_start_);
        if (rx_got_ > 0x10000 || tx_sent_ > 0x10000) rx_stream_ = tx_stream_ = false, rx_got_ = tx_sent_ = 0; // (safe loading)
    }
    // (Options::slave_table_late's table, 2026-11-16: a state from before has none, the next data frame shown as it
    // comes, the table built from there)
    view_dirty_ = true;
    auto no_tables = [&] {
        table_seq_.fill(0), table_have_.fill(0), table_count_.fill(0), table_len_.fill(0), table_shown_.fill(0);
        table_seq_next_ = 0, nodes_ = 0, hop_ = 0, view_base_ = 0, view_send_ = 0, part_ = 0, own_known_ = 0;
        own_count_.fill(0);
    };
    if (s.loading() && !s.tagged("commboard late")) {
        late_size_ = 0;
        no_tables();
        return;
    }
    if (!s.loading()) s.tag("commboard late");
    s.fixed(late_), s.io(late_size_);
    if (late_size_ > kLateTable) late_size_ = 0; // (safe loading)
    // (Options::count_matched's tables, 2026-11-19: a state from before has none, the main board shown the data as it
    // came until the ring's cabinets are known again (the next second pass) and a whole table is in)
    if (s.loading() && !s.tagged("commboard tables")) {
        no_tables();
        return;
    }
    if (!s.loading()) s.tag("commboard tables");
    s.fixed(tables_), s.io(table_count_), s.io(table_seq_), s.io(table_have_), s.io(table_seq_next_);
    s.io(nodes_), s.io(hop_), s.io(view_base_), s.io(view_send_), s.io(part_), s.io(own_count_), s.io(own_known_);
    s.io(table_len_), s.io(table_shown_);
    if (nodes_ > kMaxNodes || hop_ >= kMaxNodes || part_ > kPart) no_tables(); // (safe loading)
    for (uint16_t& n : table_len_) n = static_cast<uint16_t>(std::min<std::size_t>(n, kPart));
}

} // namespace m3
