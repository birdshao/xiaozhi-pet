#include "kid_app/content_pack_reader.h"
#include "kid_app/memory_store.h"
#include "kid_app/scene_model.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <iterator>

namespace kid_app {
namespace {

// Layout constants: docs/V4_KPB2_CONTENT_FORMAT.md (KPB2) and
// docs/P4_OFFLINE_STORY_PACK.md (KPB1). Little-endian throughout.
constexpr uint32_t kSampleRate = 24000;
constexpr uint32_t kMaxPagePcm = kSampleRate * 2 * 60;
constexpr uint32_t kMaxPromptPcm = kSampleRate * 2 * 15;
constexpr uint32_t kMaxOptionPcm = kSampleRate * 2 * 5;
constexpr uint32_t kMaxTitle = 320;
constexpr uint32_t kMaxPageText = 1280;
constexpr uint32_t kMaxPromptText = 400;
constexpr uint32_t kMaxLabel = 96;
constexpr uint16_t kIconMin = 48;
constexpr uint16_t kIconMax = 240;

constexpr uint32_t kKpb1Header = 32;
constexpr uint32_t kKpb1Record = 64;
constexpr uint32_t kHeader = 96;
constexpr uint32_t kPageRecord = 96;
constexpr uint32_t kQuestionRecord = 192;
constexpr uint32_t kOptionRecord = 48;
constexpr uint32_t kOptionsAt = 32;
constexpr uint16_t kPageGrey = 0x0001;
constexpr uint16_t kHeaderBranching = 0x0001;  // v3: pages carry next, options jump
constexpr uint8_t kQuestionVoice = 0x01;
constexpr uint8_t kOptionLike = 0x01;
// Highest registered option id per like slot (index = slot; 0 unused).
// Must match PREFERENCE_TOPICS in tools/kpb2.py (tests/test_v4_kpb2.py
// checks this line). Append only: published ids never change.
constexpr uint16_t kLikeOptionMax[] = {0, 10, 9, 8, 6};

uint16_t U16(const uint8_t* d) {
    return static_cast<uint16_t>(d[0] | (uint16_t(d[1]) << 8));
}
uint32_t U32(const uint8_t* d) {
    return uint32_t(d[0]) | (uint32_t(d[1]) << 8) |
           (uint32_t(d[2]) << 16) | (uint32_t(d[3]) << 24);
}
ContentSegment Seg(const uint8_t* d) { return {U32(d), U32(d + 4), U32(d + 8)}; }
bool Empty(const ContentSegment& s) { return !s.offset && !s.bytes && !s.crc; }
bool AllZero(const uint8_t* d, size_t n) {
    return std::all_of(d, d + n, [](uint8_t v) { return v == 0; });
}
bool Pcm(const ContentSegment& s, uint32_t max_bytes) {
    return s.bytes >= 2 && s.bytes <= max_bytes && (s.bytes & 1u) == 0;
}
bool Text(const ContentSegment& s, uint32_t max_bytes) {
    return s.bytes >= 1 && s.bytes <= max_bytes;
}

uint32_t Advance(uint32_t crc, const uint8_t* data, size_t bytes) {
    for (size_t i = 0; i < bytes; ++i) {
        crc ^= data[i];
        for (unsigned k = 0; k < 8; ++k)
            crc = (crc >> 1) ^ ((crc & 1u) ? 0xEDB88320u : 0u);
    }
    return crc;
}
uint32_t Crc(const uint8_t* data, size_t bytes) {
    return ~Advance(0xFFFFFFFFu, data, bytes);
}
// Streaming strict UTF-8 check (no overlongs, surrogates or > U+10FFFF),
// the same acceptance as Python's bytes.decode("utf-8").
class Utf8Check {
public:
    void Feed(const uint8_t* d, size_t n) {
        for (size_t i = 0; i < n && ok_; ++i) {
            const uint8_t b = d[i];
            if (need_) {
                ok_ = b >= lo_ && b <= hi_;
                lo_ = 0x80;
                hi_ = 0xBF;
                --need_;
            } else if (b >= 0x80) {
                if (b >= 0xC2 && b <= 0xDF) need_ = 1;
                else if (b >= 0xE0 && b <= 0xEF) need_ = 2;
                else if (b >= 0xF0 && b <= 0xF4) need_ = 3;
                else ok_ = false;
                if (b == 0xE0) lo_ = 0xA0;
                if (b == 0xED) hi_ = 0x9F;
                if (b == 0xF0) lo_ = 0x90;
                if (b == 0xF4) hi_ = 0x8F;
            }
        }
    }
    bool Valid() const { return ok_ && need_ == 0; }

private:
    bool ok_ = true;
    uint8_t need_ = 0;
    uint8_t lo_ = 0x80, hi_ = 0xBF;
};

// Fixed 512-byte stack chunk, as in KPB1: no heap, no full-asset buffer.
bool Verified(StoryPackFile& file, const ContentSegment& s, bool utf8 = false) {
    std::array<uint8_t, 512> chunk{};
    uint32_t crc = 0xFFFFFFFFu;
    uint32_t done = 0;
    Utf8Check text;
    while (done < s.bytes) {
        const size_t n = std::min<size_t>(chunk.size(), s.bytes - done);
        if (!file.ReadAt(s.offset + done, chunk.data(), n)) return false;
        crc = Advance(crc, chunk.data(), n);
        if (utf8) text.Feed(chunk.data(), n);
        done += static_cast<uint32_t>(n);
    }
    return ~crc == s.crc && (!utf8 || text.Valid());
}

// KPB2 data region: every segment starts exactly at the previous end
// rounded up to 4, padding is zero, nothing overlaps and nothing trails.
class SegmentChain {
public:
    SegmentChain(StoryPackFile& file, uint32_t start, uint32_t total)
        : file_(file), end_(start), total_(total) {}
    bool Next(const ContentSegment& s, bool utf8 = false) {
        const uint64_t aligned = (uint64_t(end_) + 3u) & ~uint64_t(3u);
        if (s.offset != aligned || aligned + s.bytes > total_) return false;
        if (aligned > end_) {
            std::array<uint8_t, 3> pad{};
            const size_t n = static_cast<size_t>(aligned - end_);
            if (!file_.ReadAt(end_, pad.data(), n) || !AllZero(pad.data(), n))
                return false;
        }
        if (!Verified(file_, s, utf8)) return false;
        end_ = s.offset + s.bytes;
        return true;
    }
    uint32_t End() const { return end_; }

private:
    StoryPackFile& file_;
    uint32_t end_;
    uint32_t total_;
};

bool Overlap(const PageRect& a, const PageRect& b) {
    return a.x < b.x + b.w && b.x < a.x + a.w &&
           a.y < b.y + b.h && b.y < a.y + a.h;
}

bool ParsePage(const uint8_t* r, uint16_t number, uint16_t question_first,
               bool branching, ContentPage* out) {
    ContentPage p{};
    p.number = U16(r);
    const uint16_t flags = U16(r + 2);
    p.question_first = U16(r + 4);
    const uint16_t questions = U16(r + 6);
    p.mono = Seg(r + 8);
    p.grey = Seg(r + 20);
    p.pcm = Seg(r + 32);
    p.sample_rate = U32(r + 44);
    p.channels = U16(r + 48);
    p.bits = U16(r + 50);
    p.text = Seg(r + 52);
    p.has_grey = (flags & kPageGrey) != 0;
    if (p.number != number || (flags & ~kPageGrey) != 0 ||
        p.question_first != question_first ||
        questions > ContentPackReader::kMaxQuestionsPerPage ||
        (branching ? !AllZero(r + 66, kPageRecord - 66)
                   : !AllZero(r + 64, kPageRecord - 64)) ||
        p.mono.bytes != ContentPackReader::kMonoBytes ||
        (p.has_grey ? p.grey.bytes != ContentPackReader::kGreyBytes
                    : !Empty(p.grey)) ||
        p.sample_rate != kSampleRate || p.channels != 1 || p.bits != 16 ||
        !Pcm(p.pcm, kMaxPagePcm) || !Text(p.text, kMaxPageText))
        return false;
    p.question_count = static_cast<uint8_t>(questions);
    if (branching) p.next = U16(r + 64);
    *out = p;
    return true;
}

// Pure field and geometry checks of one 192-byte question record.
bool ParseQuestion(const uint8_t* r, uint16_t page, uint8_t index, bool branching,
                   ContentQuestion* out) {
    ContentQuestion q{};
    q.page = U16(r);
    q.index = r[2];
    q.option_count = r[3];
    q.correct = r[4];
    const uint8_t flags = r[5];
    q.like_slot = U16(r + 6);
    q.prompt_pcm = Seg(r + 8);
    q.prompt_text = Seg(r + 20);
    q.allow_voice = (flags & kQuestionVoice) != 0;
    if (q.page != page || q.index != index || q.option_count < 2 ||
        q.option_count > ContentPackReader::kMaxOptions ||
        q.correct > q.option_count || (flags & ~kQuestionVoice) != 0 ||
        !Pcm(q.prompt_pcm, kMaxPromptPcm) ||
        !Text(q.prompt_text, kMaxPromptText) ||
        !AllZero(r + kOptionsAt + 3 * kOptionRecord,
                 kQuestionRecord - kOptionsAt - 3 * kOptionRecord))
        return false;
    bool any_like = false;
    for (uint8_t k = 0; k < ContentPackReader::kMaxOptions; ++k) {
        const uint8_t* o = r + kOptionsAt + k * kOptionRecord;
        if (k >= q.option_count) {
            if (!AllZero(o, kOptionRecord)) return false;
            continue;
        }
        ContentOption opt{};
        opt.id = U16(o);
        const uint8_t oflags = o[2];
        opt.counts_as_like = (oflags & kOptionLike) != 0;
        opt.box = {U16(o + 4), U16(o + 6), U16(o + 8), U16(o + 10)};
        opt.icon = Seg(o + 12);
        opt.pcm = Seg(o + 24);
        opt.label = Seg(o + 36);
        const PageRect& b = opt.box;
        const uint16_t m = ContentPackReader::kHighlightMargin;
        if (branching) opt.goto_page = o[3];
        if ((oflags & ~kOptionLike) != 0 || (!branching && o[3] != 0) ||
            (opt.counts_as_like
                 ? (q.like_slot == 0 ||
                    q.like_slot >= std::size(kLikeOptionMax) || opt.id == 0 ||
                    opt.id > kLikeOptionMax[q.like_slot])
                 : opt.id != ContentPackReader::kNonLikeOptionBase + k + 1) ||
            (b.x % 8) != 0 || (b.w % 8) != 0 ||
            b.w < kIconMin || b.w > kIconMax ||
            b.h < kIconMin || b.h > kIconMax ||
            b.x < m || b.y < m ||
            uint32_t(b.x) + b.w + m > ContentPackReader::kWidth ||
            uint32_t(b.y) + b.h + m > ContentPackReader::kHeight ||
            opt.icon.bytes != uint32_t(b.w) * b.h / 8 ||
            !Pcm(opt.pcm, kMaxOptionPcm) || !Text(opt.label, kMaxLabel))
            return false;
        for (uint8_t j = 0; j < k; ++j) {
            if (Overlap(HighlightRect(q.options[j]), HighlightRect(opt)) ||
                (opt.counts_as_like && q.options[j].id == opt.id))
                return false;
        }
        any_like = any_like || opt.counts_as_like;
        q.options[k] = opt;
    }
    // A like question has no right answer and at least one like option;
    // a question without a slot never writes memory (checked per option).
    if (q.like_slot != 0 && (q.correct != 0 || !any_like)) return false;
    // Branch question: all options jump or none do, and nothing is "right".
    uint8_t jumps = 0;
    for (uint8_t k = 0; k < q.option_count; ++k) jumps += q.options[k].goto_page != 0;
    if (jumps != 0 && (jumps != q.option_count || q.correct != 0)) return false;
    *out = q;
    return true;
}


// v3 story graph over <= 32 pages (bit p-1 = page p). Edges to page 1 that
// are marked "restart" ("read it again") are the only ones allowed to close
// a loop; every page must be reachable from page 1 and must reach an ending
// without using a restart edge.
struct StoryGraph {
    std::array<uint32_t, ContentPackReader::kMaxPages> succ{};
    std::array<bool, ContentPackReader::kMaxPages> ends{};
    std::array<bool, ContentPackReader::kMaxPages> restarts{};
    std::array<bool, ContentPackReader::kMaxPages> explicit_link{};

    bool Valid(uint16_t count) const {
        const uint32_t all = count >= 32 ? 0xFFFFFFFFu : ((1u << count) - 1u);
        uint32_t reach = 1u, frontier = 1u;
        while (frontier) {
            uint32_t grown = 0;
            for (uint16_t p = 0; p < count; ++p)
                if (frontier & (1u << p)) grown |= succ[p];
            frontier = grown & ~reach;
            reach |= grown;
        }
        if (reach != all) return false;
        uint32_t removed = 0;  // acyclic: peel pages whose successors are gone
        for (bool again = true; again;) {
            again = false;
            for (uint16_t p = 0; p < count; ++p) {
                if (!(removed & (1u << p)) && (succ[p] & ~removed) == 0) {
                    removed |= 1u << p;
                    again = true;
                }
            }
        }
        if (removed != all) return false;
        uint32_t can_end = 0;
        for (bool again = true; again;) {
            again = false;
            for (uint16_t p = 0; p < count; ++p) {
                if (!(can_end & (1u << p)) && (ends[p] || (succ[p] & can_end))) {
                    can_end |= 1u << p;
                    again = true;
                }
            }
        }
        return can_end == all;
    }
};

// Adds the edges of page `number` (1-based); false on an invalid link.
bool AddStoryEdges(StoryGraph& g, uint16_t number, uint16_t count, const ContentPage& p,
                   const std::array<ContentQuestion, ContentPackReader::kMaxQuestionsPerPage>& qs) {
    const uint16_t i = number - 1;
    bool branch = false;
    for (uint8_t j = 0; j < p.question_count; ++j) {
        if (!qs[j].IsBranch()) continue;
        if (j + 1 != p.question_count) return false;  // last question only
        branch = true;
        for (uint8_t k = 0; k < qs[j].option_count; ++k) {
            const uint8_t go = qs[j].options[k].goto_page;
            if (go == ContentPackReader::kGotoRestart) {
                if (number == 1) return false;
                g.restarts[i] = true;
            } else if (go >= 1 && go <= count && go != number) {
                g.succ[i] |= 1u << (go - 1);
            } else {
                return false;
            }
        }
    }
    if (branch) {
        g.explicit_link[i] = true;
        return p.next == 0;
    }
    if (p.next == 0) {
        if (number < count) g.succ[i] |= 1u << number;
        else g.ends[i] = true;
        return true;
    }
    g.explicit_link[i] = true;
    if (p.next == ContentPackReader::kNextEnd) {
        g.ends[i] = true;
    } else if (p.next == ContentPackReader::kNextRestart) {
        if (number == 1) return false;
        g.restarts[i] = true;
    } else if (p.next >= 1 && p.next <= count && p.next != number) {
        g.succ[i] |= 1u << (p.next - 1);
    } else {
        return false;
    }
    return true;
}

}  // namespace

PageRect HighlightRect(const ContentOption& option) {
    const uint16_t m = ContentPackReader::kHighlightMargin;
    return {static_cast<uint16_t>(option.box.x - m),
            static_cast<uint16_t>(option.box.y - m),
            static_cast<uint16_t>(option.box.w + 2 * m),
            static_cast<uint16_t>(option.box.h + 2 * m)};
}

void ContentPackReader::Close() {
    file_ = nullptr;
    format_ = PackFormat::None;
    content_id_ = 0;
    revision_ = 0;
    version_ = 0;
    branching_ = false;
    series_id_ = series_index_ = series_count_ = series_next_ = 0;
    count_ = 0;
    question_count_ = 0;
    question_table_ = 0;
    title_ = {};
    pages_ = {};
}

bool ContentPackReader::Open(StoryPackFile& file) {
    Close();
    const uint64_t size = file.Size();
    std::array<uint8_t, 4> magic{};
    if (size < kKpb1Header || size > kMaxBytes ||
        !file.ReadAt(0, magic.data(), magic.size()))
        return false;
    bool ok = false;
    if (std::memcmp(magic.data(), "KPB1", 4) == 0) ok = OpenKpb1(file, size);
    else if (std::memcmp(magic.data(), "KPB2", 4) == 0) ok = OpenKpb2(file, size);
    if (!ok) {
        Close();
        return false;
    }
    file_ = &file;
    return true;
}

// KPB1 rules are kept exactly as P4 shipped them so existing cards still open.
bool ContentPackReader::OpenKpb1(StoryPackFile& file, uint64_t size) {
    std::array<uint8_t, kKpb1Header> head{};
    if (!file.ReadAt(0, head.data(), head.size())) return false;
    if (U16(head.data() + 4) != 1 || U16(head.data() + 6) != kWidth ||
        U16(head.data() + 8) != kHeight ||
        Crc(head.data(), 28) != U32(head.data() + 28))
        return false;
    const uint16_t count = U16(head.data() + 10);
    const uint32_t toc = U32(head.data() + 12);
    const uint32_t toc_bytes = U32(head.data() + 16);
    const uint32_t first_data = U32(head.data() + 20);
    const uint32_t total = U32(head.data() + 24);
    if (count == 0 || count > kMaxPages || toc != kKpb1Header ||
        toc_bytes != count * kKpb1Record || first_data != toc + toc_bytes ||
        total != size || total < first_data)
        return false;

    uint32_t previous_end = first_data;
    for (uint32_t i = 0; i < count; ++i) {
        std::array<uint8_t, kKpb1Record> record{};
        if (!file.ReadAt(toc + i * kKpb1Record, record.data(), record.size()))
            return false;
        const uint8_t* r = record.data();
        ContentPage p{};
        p.number = U16(r);
        p.mono = Seg(r + 4);
        p.pcm = Seg(r + 16);
        p.sample_rate = U32(r + 28);
        p.channels = U16(r + 32);
        p.bits = U16(r + 34);
        p.text = Seg(r + 36);
        if (U16(r + 2) != 0 || !AllZero(r + 48, 16) || p.number != i + 1 ||
            p.mono.bytes != kMonoBytes || p.sample_rate != kSampleRate ||
            p.channels != 1 || p.bits != 16 || !Pcm(p.pcm, kMaxPagePcm) ||
            !Text(p.text, kMaxPageText) || (p.mono.offset & 3u) != 0 ||
            (p.pcm.offset & 3u) != 0 || (p.text.offset & 3u) != 0)
            return false;
        const uint64_t image_end = uint64_t(p.mono.offset) + p.mono.bytes;
        const uint64_t pcm_end = uint64_t(p.pcm.offset) + p.pcm.bytes;
        const uint64_t text_end = uint64_t(p.text.offset) + p.text.bytes;
        if (p.mono.offset < previous_end ||
            p.mono.offset > uint64_t(previous_end) + 3 || image_end > total ||
            p.pcm.offset < image_end || p.pcm.offset > image_end + 3 ||
            pcm_end > total || p.text.offset < pcm_end ||
            p.text.offset > pcm_end + 3 || text_end > total)
            return false;
        // No content is trusted until its full CRC is verified.
        if (!Verified(file, p.mono) || !Verified(file, p.pcm) ||
            !Verified(file, p.text))
            return false;
        pages_[i] = p;
        previous_end = static_cast<uint32_t>(text_end);
    }
    if (previous_end != total) return false;
    format_ = PackFormat::Kpb1;
    count_ = count;
    return true;
}

bool ContentPackReader::OpenKpb2(StoryPackFile& file, uint64_t size) {
    std::array<uint8_t, kHeader> head{};
    if (size < kHeader || !file.ReadAt(0, head.data(), head.size()))
        return false;
    const uint8_t* h = head.data();
    const uint16_t count = U16(h + 20);
    const uint16_t questions = U16(h + 22);
    const uint32_t page_table = U32(h + 24);
    const uint32_t page_bytes = U32(h + 28);
    const uint32_t question_table = U32(h + 32);
    const uint32_t question_bytes = U32(h + 36);
    const uint32_t data = U32(h + 40);
    const uint32_t total = U32(h + 44);
    const ContentSegment title = Seg(h + 48);
    const uint32_t tables_crc = U32(h + 60);
    const uint16_t version = U16(h + 4);
    const uint16_t hflags = U16(h + 10);
    const bool v3 = version == 3;
    const bool branching = v3 && (hflags & kHeaderBranching) != 0;
    const uint16_t series_id = v3 ? U16(h + 64) : 0;
    const uint16_t series_index = v3 ? U16(h + 66) : 0;
    const uint16_t series_count = v3 ? U16(h + 68) : 0;
    const uint16_t series_next = v3 ? U16(h + 70) : 0;
    const bool series_ok =
        (series_id == 0 && series_index == 0 && series_count == 0 && series_next == 0) ||
        (series_id != 0 && series_count >= 1 && series_count <= ContentPackReader::kMaxSeries &&
         series_index >= 1 && series_index <= series_count &&
         (series_index < series_count ? series_next != 0 && series_next != U16(h + 16)
                                      : series_next == 0));
    if ((version != 2 && !v3) || U16(h + 6) != kHeader ||
        U16(h + 8) != static_cast<uint16_t>(PackType::Book) ||
        (hflags & ~(v3 ? kHeaderBranching : uint16_t{0})) != 0 || !series_ok ||
        U16(h + 12) != kWidth ||
        U16(h + 14) != kHeight || U16(h + 16) == 0 || U16(h + 18) == 0 ||
        count == 0 || count > kMaxPages || questions > kMaxQuestions ||
        page_table != kHeader || page_bytes != count * kPageRecord ||
        question_table != page_table + page_bytes ||
        question_bytes != questions * kQuestionRecord ||
        data != question_table + question_bytes || total != size ||
        total < data || !Text(title, kMaxTitle) ||
        (v3 ? !AllZero(h + 72, 20) : !AllZero(h + 64, 28)) ||
        Crc(h, 92) != U32(h + 92))
        return false;
    if (!Verified(file, {page_table, data - page_table, tables_crc}))
        return false;

    SegmentChain chain(file, data, total);
    if (!chain.Next(title, true)) return false;
    uint16_t seen = 0;
    StoryGraph graph{};
    for (uint16_t i = 0; i < count; ++i) {
        std::array<uint8_t, kPageRecord> record{};
        ContentPage p{};
        std::array<ContentQuestion, kMaxQuestionsPerPage> page_questions{};
        if (!file.ReadAt(page_table + i * kPageRecord, record.data(),
                         record.size()) ||
            !ParsePage(record.data(), static_cast<uint16_t>(i + 1), seen, branching, &p) ||
            seen + p.question_count > questions ||
            !chain.Next(p.mono) || (p.has_grey && !chain.Next(p.grey)) ||
            !chain.Next(p.pcm) || !chain.Next(p.text, true))
            return false;
        for (uint8_t j = 0; j < p.question_count; ++j) {
            std::array<uint8_t, kQuestionRecord> raw{};
            ContentQuestion q{};
            if (!file.ReadAt(question_table + (seen + j) * kQuestionRecord,
                             raw.data(), raw.size()) ||
                !ParseQuestion(raw.data(), p.number, j, branching, &q) ||
                !chain.Next(q.prompt_pcm) || !chain.Next(q.prompt_text, true))
                return false;
            for (uint8_t k = 0; k < q.option_count; ++k) {
                const ContentOption& o = q.options[k];
                if (!chain.Next(o.icon) || !chain.Next(o.pcm) ||
                    !chain.Next(o.label, true))
                    return false;
            }
            page_questions[j] = q;
        }
        if (!AddStoryEdges(graph, p.number, count, p, page_questions)) return false;
        seen = static_cast<uint16_t>(seen + p.question_count);
        pages_[i] = p;
    }
    if (seen != questions || chain.End() != total) return false;
    if (!graph.Valid(count)) return false;
    if (branching) {  // the flag must mean something
        bool any = false;
        for (uint16_t i = 0; i < count; ++i) any = any || graph.explicit_link[i];
        if (!any) return false;
    }
    format_ = PackFormat::Kpb2;
    version_ = version;
    branching_ = branching;
    series_id_ = series_id;
    series_index_ = series_index;
    series_count_ = series_count;
    series_next_ = series_next;
    content_id_ = U16(h + 16);
    revision_ = U16(h + 18);
    count_ = count;
    question_count_ = questions;
    question_table_ = question_table;
    title_ = title;
    return true;
}

bool ContentPackReader::Page(uint16_t number, ContentPage* out) const {
    if (!Ready() || !out || number < 1 || number > count_) return false;
    *out = pages_[number - 1];
    return true;
}

bool ContentPackReader::Question(uint16_t page, uint8_t index,
                                 ContentQuestion* out) {
    ContentPage p{};
    if (!out || !Page(page, &p) || index >= p.question_count) return false;
    std::array<uint8_t, kQuestionRecord> raw{};
    return file_->ReadAt(question_table_ +
                             (p.question_first + index) * kQuestionRecord,
                         raw.data(), raw.size()) &&
           ParseQuestion(raw.data(), page, index, branching_, out);
}

bool ContentPackReader::ReadRange(const ContentSegment& s, uint32_t offset,
                                  void* target, size_t bytes) {
    if (!Ready() || !target || bytes == 0 ||
        uint64_t(offset) + bytes > s.bytes)
        return false;
    return file_->ReadAt(s.offset + offset, target, bytes);
}

bool ContentPackReader::ReadString(const ContentSegment& s, char* target,
                                   size_t capacity) {
    if (!target || capacity <= s.bytes || !ReadRange(s, 0, target, s.bytes))
        return false;
    target[s.bytes] = '\0';
    return true;
}

bool ContentPackReader::ReadTitle(char* target, size_t capacity) {
    return format_ == PackFormat::Kpb2 && ReadString(title_, target, capacity);
}

bool ContentPackReader::ReadMono(uint16_t page, uint8_t* target,
                                 size_t capacity) {
    ContentPage p{};
    return Page(page, &p) && capacity >= kMonoBytes &&
           ReadRange(p.mono, 0, target, kMonoBytes);
}

bool ContentPackReader::ReadGrey(uint16_t page, uint8_t* target,
                                 size_t capacity) {
    return ReadGreyRows(page, 0, kHeight, target, capacity);
}

bool ContentPackReader::ReadGreyRows(uint16_t page, uint16_t first_row,
                                     uint16_t rows, uint8_t* target,
                                     size_t capacity) {
    ContentPage p{};
    const size_t bytes = size_t(rows) * kGreyRowBytes;
    return Page(page, &p) && p.has_grey && rows > 0 &&
           uint32_t(first_row) + rows <= kHeight && capacity >= bytes &&
           ReadRange(p.grey, uint32_t(first_row) * kGreyRowBytes, target, bytes);
}

bool ContentPackReader::ReadPagePcm(uint16_t page, uint32_t byte_offset,
                                    void* target, size_t bytes) {
    ContentPage p{};
    return Page(page, &p) && (byte_offset & 1u) == 0 && (bytes & 1u) == 0 &&
           ReadRange(p.pcm, byte_offset, target, bytes);
}

bool ContentPackReader::ReadPageText(uint16_t page, char* target,
                                     size_t capacity) {
    ContentPage p{};
    return Page(page, &p) && ReadString(p.text, target, capacity);
}

bool ContentPackReader::ReadPromptPcm(const ContentQuestion& q,
                                      uint32_t byte_offset, void* target,
                                      size_t bytes) {
    return (byte_offset & 1u) == 0 && (bytes & 1u) == 0 &&
           ReadRange(q.prompt_pcm, byte_offset, target, bytes);
}

bool ContentPackReader::ReadPromptText(const ContentQuestion& q, char* target,
                                       size_t capacity) {
    return ReadString(q.prompt_text, target, capacity);
}

bool ContentPackReader::ReadOptionPcm(const ContentQuestion& q, uint8_t option,
                                      uint32_t byte_offset, void* target,
                                      size_t bytes) {
    return option < q.option_count && (byte_offset & 1u) == 0 &&
           (bytes & 1u) == 0 &&
           ReadRange(q.options[option].pcm, byte_offset, target, bytes);
}

bool ContentPackReader::ReadOptionLabel(const ContentQuestion& q,
                                        uint8_t option, char* target,
                                        size_t capacity) {
    return option < q.option_count &&
           ReadString(q.options[option].label, target, capacity);
}

bool ContentPackReader::ReadOptionIcon(const ContentQuestion& q, uint8_t option,
                                       uint8_t* target, size_t capacity) {
    if (option >= q.option_count) return false;
    const ContentSegment& icon = q.options[option].icon;
    return capacity >= icon.bytes && ReadRange(icon, 0, target, icon.bytes);
}

bool ContentPackReader::BlitOptionIcon(const ContentQuestion& q, uint8_t option,
                                       MonoFrame& frame) {
    if (option >= q.option_count) return false;
    const ContentOption& o = q.options[option];
    const uint32_t stride = o.box.w / 8u;
    std::array<uint8_t, kIconMax / 8> row{};
    if (stride == 0 || stride > row.size() ||
        o.icon.bytes != stride * o.box.h)
        return false;
    for (uint16_t y = 0; y < o.box.h; ++y) {
        if (!ReadRange(o.icon, y * stride, row.data(), stride)) return false;
        for (uint16_t x = 0; x < o.box.w; ++x) {
            const bool white = (row[x / 8] & (0x80u >> (x % 8))) != 0;
            frame.SetPixel(o.box.x + x, o.box.y + y, !white);
        }
    }
    return true;
}

bool SplitGreyPlanes(const uint8_t* grey, size_t grey_bytes, uint8_t* hi,
                     uint8_t* lo) {
    if (!grey || !hi || !lo || grey_bytes == 0 || (grey_bytes & 1u) != 0)
        return false;
    for (size_t i = 0; i < grey_bytes / 2; ++i) {
        uint8_t h = 0, l = 0;
        for (unsigned p = 0; p < 8; ++p) {
            const uint8_t byte = grey[2 * i + p / 4];
            const uint8_t level =
                static_cast<uint8_t>((byte >> (6 - 2 * (p % 4))) & 3u);
            h = static_cast<uint8_t>(h | ((level >> 1) << (7 - p)));
            l = static_cast<uint8_t>(l | ((level & 1u) << (7 - p)));
        }
        hi[i] = h;
        lo[i] = l;
    }
    return true;
}

bool ChoiceMemoryEvent(const ContentQuestion& q, uint8_t option,
                       uint64_t event_id, uint32_t day, MemoryEvent* out) {
    if (!out || event_id == 0 || option >= q.option_count ||
        q.like_slot == 0 || !q.options[option].counts_as_like)
        return false;
    MemoryEvent e{};
    e.id = event_id;
    e.kind = MemoryEventKind::PreferenceChosen;
    e.source = MemorySource::DialChoice;
    e.day = day;
    e.slot = q.like_slot;
    e.option = q.options[option].id;
    *out = e;
    return true;
}

bool BookFinishedMemoryEvent(const ContentPackReader& reader,
                             uint64_t event_id, uint32_t day,
                             MemoryEvent* out) {
    if (!out || event_id == 0 || !reader.Ready() ||
        reader.Format() != PackFormat::Kpb2 || reader.ContentId() == 0)
        return false;
    MemoryEvent e{};
    e.id = event_id;
    e.kind = MemoryEventKind::BookFinished;
    e.source = MemorySource::Device;
    e.day = day;
    e.option = reader.ContentId();
    *out = e;
    return true;
}

}  // namespace kid_app
