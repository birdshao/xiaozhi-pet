#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace kid_app {

class MonoFrame;
struct MemoryEvent;

// Read-only random access to one package file on a mounted FAT/SD card.
// Do not call Open() or other file IO from an audio callback / ISR.
class StoryPackFile {
public:
    virtual ~StoryPackFile() = default;
    virtual uint64_t Size() const = 0;
    virtual bool ReadAt(uint32_t offset, void* out, size_t size) = 0;
};

enum class PackFormat : uint8_t { None, Kpb1, Kpb2 };

// KPB2 header package_type (docs/V4_KPB2_CONTENT_FORMAT.md §2). Only Book is
// read today; the other values are reserved so sound/game/costume/phrase
// packs never get mistaken for a book. KPB1 files are always books.
enum class PackType : uint16_t {
    Book = 1,
    SoundPack = 2,
    Game = 3,
    Costume = 4,
    Phrases = 5,
};

// One CRC-checked byte range inside the package.
struct ContentSegment {
    uint32_t offset = 0, bytes = 0, crc = 0;
};

struct ContentPage {
    uint16_t number = 0;          // 1-based
    bool has_grey = false;        // KPB2 4-level page present
    uint8_t question_count = 0;   // 0..2 (always 0 for KPB1)
    uint16_t question_first = 0;  // index into the package question table
    ContentSegment mono{};        // 800x480 1-bit, bit=1 white (48,000 B)
    ContentSegment grey{};        // 800x480 2-bit, 3=white (96,000 B)
    ContentSegment pcm{};         // 24 kHz mono PCM16 narration
    ContentSegment text{};        // reviewed UTF-8 page text
    // KPB2 v3 branching books: where the story goes after this page. 0 =
    // the next page (END after the last); kNextEnd = an ending;
    // kNextRestart = back to page 1 ("read it again"); else a page number.
    uint16_t next = 0;
    uint32_t sample_rate = 0;
    uint16_t channels = 0, bits = 0;
};

// Rectangle in page pixels.
struct PageRect {
    uint16_t x = 0, y = 0, w = 0, h = 0;
};

struct ContentOption {
    uint16_t id = 0;              // like: registry option id; else 0xFF00+n
    bool counts_as_like = false;  // dial pick writes PREFERENCE_CHOSEN
    // KPB2 v3 branch question: 1..32 = page the story jumps to when this
    // option is picked, kGotoRestart = back to page 1; 0 = no jump.
    uint8_t goto_page = 0;
    PageRect box{};               // 1-bit icon position = dial hit region
    ContentSegment icon{};        // box.w*box.h/8 B, bit=1 white
    ContentSegment pcm{};         // option name, 24 kHz mono PCM16
    ContentSegment label{};       // reviewed UTF-8 option name
};

struct ContentQuestion {
    uint16_t page = 0;            // 1-based owning page
    uint8_t index = 0;            // 0..1 on that page
    uint8_t option_count = 0;     // 2..3
    uint8_t correct = 0;          // 0 = no right/wrong, else 1-based option
    bool allow_voice = false;     // a voice answer may pick an option
    uint16_t like_slot = 0;       // 0 = never writes memory (ADR-012)
    ContentSegment prompt_pcm{};
    ContentSegment prompt_text{};
    std::array<ContentOption, 3> options{};

    // A branch question: every option jumps (all-or-none, checked on Open).
    bool IsBranch() const { return option_count != 0 && options[0].goto_page != 0; }
    bool IsCorrect(uint8_t option) const {
        return correct != 0 && option + 1u == correct;
    }
};

// Validating reader for KPB1 (1-bit pages + narration, P4) and KPB2 (grey
// pages, dial choice questions, package type; V4-04). Open() checks every
// header/table field and the CRC of every asset in 512-byte chunks before
// reporting success. CRC detects accidental corruption only; it is NOT a
// signature or parental authorization.
class ContentPackReader {
public:
    static constexpr uint32_t kWidth = 800;
    static constexpr uint32_t kHeight = 480;
    static constexpr uint32_t kMonoBytes = kWidth * kHeight / 8;  // 48,000
    static constexpr uint32_t kGreyBytes = kWidth * kHeight / 4;  // 96,000
    static constexpr uint32_t kGreyRowBytes = kWidth / 4;         // 200
    static constexpr uint32_t kMaxBytes = 64u * 1024u * 1024u;
    static constexpr size_t kMaxPages = 32;
    static constexpr size_t kMaxQuestionsPerPage = 2;
    static constexpr size_t kMaxQuestions = 64;
    static constexpr size_t kMaxOptions = 3;
    static constexpr uint16_t kHighlightMargin = 8;
    static constexpr uint16_t kNonLikeOptionBase = 0xFF00;
    static constexpr uint16_t kNextEnd = 0xFFFF;
    static constexpr uint16_t kNextRestart = 0x8001;
    static constexpr uint8_t kGotoRestart = 0x81;
    static constexpr uint16_t kMaxSeries = 16;

    bool Open(StoryPackFile& file);
    void Close();
    bool Ready() const { return file_ != nullptr; }

    PackFormat Format() const { return format_; }
    PackType Type() const { return PackType::Book; }
    uint16_t ContentId() const { return content_id_; }  // 0 for KPB1
    uint16_t Revision() const { return revision_; }     // 0 for KPB1
    uint16_t PageCount() const { return count_; }
    uint16_t QuestionCount() const { return question_count_; }
    // KPB2 version 2 (linear) or 3 (may branch / belong to a series); 0 for
    // KPB1.
    uint16_t Version() const { return version_; }
    bool Branching() const { return branching_; }
    // Series (v3): id 0 = standalone. index is 1-based of count; next is the
    // content id of the following book (0 = last in the series).
    uint16_t SeriesId() const { return series_id_; }
    uint16_t SeriesIndex() const { return series_index_; }
    uint16_t SeriesCount() const { return series_count_; }
    uint16_t SeriesNext() const { return series_next_; }

    bool Page(uint16_t number, ContentPage* out) const;
    // index is 0..question_count-1 of that page. Re-reads the 192-byte
    // record (already CRC-checked by Open) so the reader stays ~2 KiB.
    bool Question(uint16_t page, uint8_t index, ContentQuestion* out);

    bool ReadTitle(char* target, size_t capacity);  // KPB2 only
    // 1-bit page (KPB1 page or KPB2 fallback): 48,000 B, MonoFrame layout.
    bool ReadMono(uint16_t page, uint8_t* target, size_t capacity);
    // 4-level page: 96,000 B, 4 px/byte MSB first, 0 black .. 3 white.
    bool ReadGrey(uint16_t page, uint8_t* target, size_t capacity);
    // Bounded streaming of grey rows (200 B each) for a small SPI buffer.
    bool ReadGreyRows(uint16_t page, uint16_t first_row, uint16_t rows,
                      uint8_t* target, size_t capacity);
    // PCM reads: byte offset and length must be even (16-bit samples).
    bool ReadPagePcm(uint16_t page, uint32_t byte_offset, void* target,
                     size_t bytes);
    bool ReadPageText(uint16_t page, char* target, size_t capacity);

    bool ReadPromptPcm(const ContentQuestion& q, uint32_t byte_offset,
                       void* target, size_t bytes);
    bool ReadPromptText(const ContentQuestion& q, char* target,
                        size_t capacity);
    // option is 0-based.
    bool ReadOptionPcm(const ContentQuestion& q, uint8_t option,
                       uint32_t byte_offset, void* target, size_t bytes);
    bool ReadOptionLabel(const ContentQuestion& q, uint8_t option,
                         char* target, size_t capacity);
    bool ReadOptionIcon(const ContentQuestion& q, uint8_t option,
                        uint8_t* target, size_t capacity);
    // Paints the icon's black and white pixels into frame at its box.
    bool BlitOptionIcon(const ContentQuestion& q, uint8_t option,
                        MonoFrame& frame);

private:
    bool OpenKpb1(StoryPackFile& file, uint64_t size);
    bool OpenKpb2(StoryPackFile& file, uint64_t size);
    bool ReadRange(const ContentSegment& s, uint32_t offset, void* target,
                   size_t bytes);
    bool ReadString(const ContentSegment& s, char* target, size_t capacity);

    StoryPackFile* file_ = nullptr;
    PackFormat format_ = PackFormat::None;
    uint16_t content_id_ = 0;
    uint16_t revision_ = 0;
    uint16_t version_ = 0;
    bool branching_ = false;
    uint16_t series_id_ = 0, series_index_ = 0, series_count_ = 0, series_next_ = 0;
    uint16_t count_ = 0;
    uint16_t question_count_ = 0;
    uint32_t question_table_ = 0;
    ContentSegment title_{};
    std::array<ContentPage, kMaxPages> pages_{};
};

// Dial-pick area including the highlight ring margin: what the device
// partially refreshes when the selection moves.
PageRect HighlightRect(const ContentOption& option);

// Splits grey bytes (even count) into the two 1-bit planes a 4-grey EPD
// waveform consumes: hi = level >> 1, lo = level & 1, 8 px per byte, MSB
// first. Writes grey_bytes / 2 bytes to each plane.
bool SplitGreyPlanes(const uint8_t* grey, size_t grey_bytes, uint8_t* hi,
                     uint8_t* lo);

// ADR-012 / V5-03: the ONLY mapping from a book question to memory. True
// (and *out filled, source DialChoice) only when the child confirmed an
// option that counts as a like on a like question. Voice answers must not
// call this.
bool ChoiceMemoryEvent(const ContentQuestion& q, uint8_t option,
                       uint64_t event_id, uint32_t day, MemoryEvent* out);

// STORY_FINISHED for a KPB2 book (KPB1 has no numeric content id).
bool BookFinishedMemoryEvent(const ContentPackReader& reader,
                             uint64_t event_id, uint32_t day,
                             MemoryEvent* out);

}  // namespace kid_app
