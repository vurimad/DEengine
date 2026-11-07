#include "core/log.h"
#include "image_diff/image_diff.h"
#include "platform/file.h"
#include "support/sandbox.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstring>
#include <initializer_list>
#include <string_view>

using de::image_diff::compare;
using de::image_diff::decode_png;
using de::image_diff::encode_png;
using de::image_diff::Image;
using de::image_diff::kExitDifferent;
using de::image_diff::kExitError;
using de::image_diff::kExitSame;
using de::image_diff::Report;
using de::image_diff::run;

namespace {

struct Pixel {
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;
    uint8_t a = 255;
};

Pixel rgb(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) {
    return {.r = r, .g = g, .b = b, .a = a};
}

Image picture(uint32_t width, uint32_t height, Pixel fill = {}) {
    Image image;
    image.width = width;
    image.height = height;
    image.rgba.resize(size_t{width} * height * 4);
    for (size_t i = 0; i < size_t{width} * height; ++i) {
        image.rgba[i * 4] = fill.r;
        image.rgba[(i * 4) + 1] = fill.g;
        image.rgba[(i * 4) + 2] = fill.b;
        image.rgba[(i * 4) + 3] = fill.a;
    }
    return image;
}

void set_pixel(Image& image, uint32_t x, uint32_t y, Pixel pixel) {
    const size_t at = (size_t{y} * image.width + x) * 4;
    image.rgba[at] = pixel.r;
    image.rgba[at + 1] = pixel.g;
    image.rgba[at + 2] = pixel.b;
    image.rgba[at + 3] = pixel.a;
}

Pixel get_pixel(const Image& image, uint32_t x, uint32_t y) {
    const size_t at = (size_t{y} * image.width + x) * 4;
    return rgb(image.rgba[at], image.rgba[at + 1], image.rgba[at + 2], image.rgba[at + 3]);
}

struct QuietLog {
    de::core::LogLevel saved = de::core::log_level();
    QuietLog() { de::core::set_log_level(de::core::LogLevel::Fatal); }
    ~QuietLog() { de::core::set_log_level(saved); }
    QuietLog(const QuietLog&) = delete;
    QuietLog& operator=(const QuietLog&) = delete;
    QuietLog(QuietLog&&) = delete;
    QuietLog& operator=(QuietLog&&) = delete;
};

// The last line logged, for what the tool says.
class LastLine final : public de::core::LogSink {
public:
    LastLine() { de::core::add_log_sink(*this); }
    ~LastLine() { de::core::remove_log_sink(*this); }
    LastLine(const LastLine&) = delete;
    LastLine& operator=(const LastLine&) = delete;
    LastLine(LastLine&&) = delete;
    LastLine& operator=(LastLine&&) = delete;
    void write(const de::core::LogMessage& message) override {
        std::string_view(message.text).copy(text, sizeof(text) - 1);
        text[std::min(std::strlen(message.text), sizeof(text) - 1)] = '\0';
    }
    char text[256] = {};
};

// The command line, against files in a sandbox.
class Files {
public:
    void put(const char* name, const Image& image, uint32_t channels = 3) const {
        const auto png = encode_png(image, channels);
        REQUIRE(png.ok());
        REQUIRE(de::platform::write_file_atomic(path(name).c_str(), png->span()).ok());
    }
    void put_bytes(const char* name, std::string_view bytes) const {
        REQUIRE(de::platform::write_file_atomic(path(name).c_str(), de::test::bytes_of(bytes)).ok());
    }
    de::platform::PathString path(const char* name) const { return sandbox_.folder(name); }

    // Words starting with @ are files here.
    int run_with(std::initializer_list<const char*> words) const {
        de::platform::PathString owned[12];
        char* argv[12] = {};
        int argc = 0;
        owned[argc] = de::platform::PathString("image_diff");
        argv[argc] = const_cast<char*>(owned[argc].c_str());
        ++argc;
        REQUIRE(words.size() < 11);
        for (const char* word : words) {
            owned[argc] = word[0] == '@' ? path(word + 1) : de::platform::PathString(word);
            argv[argc] = const_cast<char*>(owned[argc].c_str());
            ++argc;
        }
        return run(argc, argv);
    }

private:
    de::test::Sandbox sandbox_;
};

} // namespace

TEST_CASE("two pictures the same differ in nothing") {
    const Report report = compare(picture(3, 2, rgb(10, 20, 30)), picture(3, 2, rgb(10, 20, 30)), nullptr, 0, nullptr);
    CHECK(report.compared == 6);
    CHECK(report.differing == 0);
    CHECK(report.max_difference == 0);
    CHECK(report.mean_difference == 0.0);
}

TEST_CASE("a pixel's difference is its largest channel's, and it says where the biggest is") {
    const Image a = picture(4, 2, rgb(100, 100, 100));
    Image b = picture(4, 2, rgb(100, 100, 100));
    set_pixel(b, 1, 0, rgb(103, 100, 100));
    set_pixel(b, 2, 1, rgb(90, 95, 100));   // red is the largest: 10
    set_pixel(b, 3, 1, rgb(100, 100, 110)); // as large, later: the first stays
    const Report report = compare(a, b, nullptr, 0, nullptr);
    CHECK(report.compared == 8);
    CHECK(report.differing == 3);
    CHECK(report.max_difference == 10);
    CHECK(report.max_x == 2);
    CHECK(report.max_y == 1);
    CHECK(report.mean_difference == doctest::Approx((3.0 + 10.0 + 10.0) / 8.0));
}

TEST_CASE("alpha is a channel like the others") {
    const Report report =
        compare(picture(1, 1, rgb(0, 0, 0, 255)), picture(1, 1, rgb(0, 0, 0, 250)), nullptr, 0, nullptr);
    CHECK(report.differing == 1);
    CHECK(report.max_difference == 5);
}

TEST_CASE("a pixel differs only past the threshold") {
    const Image a = picture(1, 1, rgb(100, 100, 100));
    const Image b = picture(1, 1, rgb(102, 100, 100));
    CHECK(compare(a, b, nullptr, 1, nullptr).differing == 1);
    CHECK(compare(a, b, nullptr, 2, nullptr).differing == 0);
    // The maximum and the mean don't depend on it.
    CHECK(compare(a, b, nullptr, 2, nullptr).max_difference == 2);
    CHECK(compare(a, b, nullptr, 2, nullptr).mean_difference == 2.0);
}

TEST_CASE("what the mask blacks out isn't compared") {
    const Image a = picture(3, 1, rgb(0, 0, 0));
    Image b = picture(3, 1, rgb(0, 0, 0));
    set_pixel(b, 0, 0, rgb(50, 0, 0));
    set_pixel(b, 1, 0, rgb(80, 0, 0));
    set_pixel(b, 2, 0, rgb(20, 0, 0));
    Image mask = picture(3, 1, rgb(255, 255, 255));
    set_pixel(mask, 1, 0, rgb(127, 127, 127)); // below the middle in every channel: out
    set_pixel(mask, 2, 0, rgb(0, 128, 0));     // any channel in the upper half: in
    const Report report = compare(a, b, &mask, 0, nullptr);
    CHECK(report.compared == 2);
    CHECK(report.differing == 2);
    CHECK(report.max_difference == 50);
    CHECK(report.max_x == 0);
    CHECK(report.mean_difference == 35.0);
}

TEST_CASE("the picture of the differences shows what differs, red on grey, and blacks out the mask") {
    const Image a = picture(4, 1, rgb(40, 80, 120));
    Image b = picture(4, 1, rgb(40, 80, 120));
    set_pixel(b, 1, 0, rgb(41, 80, 120));
    set_pixel(b, 2, 0, rgb(255, 80, 120));
    set_pixel(b, 3, 0, rgb(255, 80, 120));
    Image mask = picture(4, 1, rgb(255, 255, 255));
    set_pixel(mask, 3, 0, rgb(0, 0, 0));
    Image out;
    CHECK(compare(a, b, &mask, 0, &out).differing == 2);
    REQUIRE(out.rgba.size() == a.rgba.size());
    const uint8_t grey = 80 / 4; // a's grey, at a quarter
    CHECK(get_pixel(out, 0, 0).r == grey);
    CHECK(get_pixel(out, 0, 0).g == grey);
    CHECK(get_pixel(out, 0, 0).b == grey);
    CHECK(get_pixel(out, 1, 0).r == 128); // the least that differs still shows
    CHECK(get_pixel(out, 1, 0).g == 0);
    CHECK(get_pixel(out, 2, 0).r == 128 + (215 / 2));
    CHECK(get_pixel(out, 2, 0).b == 0);
    CHECK(get_pixel(out, 3, 0).r == 0); // masked out
    CHECK(get_pixel(out, 3, 0).g == 0);
    CHECK(get_pixel(out, 3, 0).a == 255);
    // Within the threshold, it's the grey.
    Image within;
    static_cast<void>(compare(a, b, nullptr, 255, &within));
    CHECK(get_pixel(within, 2, 0).r == grey);
}

TEST_CASE("PNGs come back as they went in") {
    Image image = picture(3, 2, rgb(1, 2, 3, 200));
    set_pixel(image, 2, 1, rgb(250, 251, 252, 40));
    const auto with_alpha = encode_png(image, 4);
    REQUIRE(with_alpha.ok());
    const auto kept = decode_png(with_alpha->span());
    REQUIRE(kept.ok());
    CHECK(kept->width == 3);
    CHECK(kept->height == 2);
    CHECK(get_pixel(*kept, 0, 0).b == 3);
    CHECK(get_pixel(*kept, 0, 0).a == 200);
    CHECK(get_pixel(*kept, 2, 1).r == 250);
    CHECK(get_pixel(*kept, 2, 1).a == 40);
    // Without alpha: opaque.
    const auto without_alpha = encode_png(image);
    REQUIRE(without_alpha.ok());
    const auto opaque = decode_png(without_alpha->span());
    REQUIRE(opaque.ok());
    CHECK(get_pixel(*opaque, 2, 1).g == 251);
    CHECK(get_pixel(*opaque, 2, 1).a == 255);
}

TEST_CASE("what isn't a PNG isn't read") {
    const QuietLog quiet;
    CHECK_FALSE(decode_png(de::test::bytes_of("not a png")).ok());
    CHECK_FALSE(decode_png({}).ok());
    const auto png = encode_png(picture(8, 8, rgb(1, 2, 3)));
    REQUIRE(png.ok());
    CHECK_FALSE(decode_png(png->span().first(png->size() / 2)).ok()); // cut short
    CHECK(decode_png(png->span()).ok());
    CHECK_FALSE(encode_png(Image{}).ok());
    CHECK_FALSE(encode_png(picture(1, 1), 2).ok());
}

TEST_CASE("the exit code says same, different or couldn't compare") {
    const Files files;
    files.put("a.png", picture(4, 4, rgb(100, 100, 100)));
    files.put("a2.png", picture(4, 4, rgb(100, 100, 100)));
    Image other = picture(4, 4, rgb(100, 100, 100));
    set_pixel(other, 1, 1, rgb(105, 100, 100));
    set_pixel(other, 2, 2, rgb(103, 100, 100));
    files.put("b.png", other);
    files.put("small.png", picture(2, 2, rgb(100, 100, 100)));
    Image holes = picture(4, 4, rgb(255, 255, 255));
    set_pixel(holes, 1, 1, rgb(0, 0, 0));
    files.put("mask.png", holes);
    set_pixel(holes, 2, 2, rgb(0, 0, 0));
    files.put("mask2.png", holes);
    files.put("mask_small.png", picture(2, 2, rgb(255, 255, 255)));
    files.put("mask_all.png", picture(4, 4, rgb(0, 0, 0)));
    files.put_bytes("junk.png", "not a png");

    const QuietLog quiet;
    CHECK(files.run_with({"@a.png", "@a2.png"}) == kExitSame);
    CHECK(files.run_with({"@a.png", "@b.png"}) == kExitDifferent);
    CHECK(files.run_with({"@b.png", "@a.png"}) == kExitDifferent);
    // The threshold: past what differs most, and just short of it.
    CHECK(files.run_with({"@a.png", "@b.png", "--threshold", "5"}) == kExitSame);
    CHECK(files.run_with({"@a.png", "@b.png", "--threshold", "4"}) == kExitDifferent);
    // How many may differ: two do.
    CHECK(files.run_with({"@a.png", "@b.png", "--max-pixels", "2"}) == kExitSame);
    CHECK(files.run_with({"@a.png", "@b.png", "--max-pixels", "1"}) == kExitDifferent);
    // The one left past a threshold of 4 is masked out, and past one of 2 another is left.
    CHECK(files.run_with({"@a.png", "@b.png", "--threshold", "4", "--mask", "@mask.png"}) == kExitSame);
    CHECK(files.run_with({"@a.png", "@b.png", "--threshold", "2", "--mask", "@mask.png"}) == kExitDifferent);
    CHECK(files.run_with({"@a.png", "@b.png", "--mask", "@mask2.png"}) == kExitSame);

    // Couldn't compare.
    CHECK(files.run_with({}) == kExitError);
    CHECK(files.run_with({"@a.png"}) == kExitError);
    CHECK(files.run_with({"@a.png", "@a2.png", "@b.png"}) == kExitError);
    CHECK(files.run_with({"@a.png", "@a2.png", "--wat", "1"}) == kExitError);
    CHECK(files.run_with({"@a.png", "@a2.png", "--threshold"}) == kExitError);
    CHECK(files.run_with({"@a.png", "@a2.png", "--threshold", "256"}) == kExitError);
    CHECK(files.run_with({"@a.png", "@a2.png", "--threshold", "-1"}) == kExitError);
    CHECK(files.run_with({"@a.png", "@a2.png", "--threshold", "1x"}) == kExitError);
    CHECK(files.run_with({"@a.png", "@a2.png", "--threshold", "255"}) == kExitSame); // the limit itself is fine
    CHECK(files.run_with({"@a.png", "@a2.png", "--max-pixels", "many"}) == kExitError);
    CHECK(files.run_with({"@a.png", "@gone.png"}) == kExitError);
    CHECK(files.run_with({"@gone.png", "@a.png"}) == kExitError);
    CHECK(files.run_with({"@a.png", "@junk.png"}) == kExitError);
    CHECK(files.run_with({"@junk.png", "@a.png"}) == kExitError);
    CHECK(files.run_with({"@a.png", "@small.png"}) == kExitError);
    CHECK(files.run_with({"@a.png", "@a2.png", "--mask", "@gone.png"}) == kExitError);
    CHECK(files.run_with({"@a.png", "@a2.png", "--mask", "@mask_small.png"}) == kExitError);
    CHECK(files.run_with({"@a.png", "@a2.png", "--mask", "@mask_all.png"}) == kExitError);
}

TEST_CASE("the tool says its numbers, and writes the picture of the differences, passing or not") {
    const Files files;
    files.put("a.png", picture(2, 2, rgb(100, 100, 100)));
    Image other = picture(2, 2, rgb(100, 100, 100));
    set_pixel(other, 1, 0, rgb(107, 100, 100));
    files.put("b.png", other);

    {
        const LastLine said;
        CHECK(files.run_with({"@a.png", "@b.png"}) == kExitDifferent);
        CHECK((std::string_view(said.text) ==
               "FAIL: 1 of 4 pixels differ by over 0 (0 allowed); max 7 at 1,0; mean 1.750000"));
    }
    {
        const LastLine said;
        CHECK(files.run_with({"@a.png", "@b.png", "--threshold", "7", "--out", "@sub/folder/diff.png"}) == kExitSame);
        CHECK((std::string_view(said.text) ==
               "PASS: 0 of 4 pixels differ by over 7 (0 allowed); max 7 at 1,0; mean 1.750000"));
    }
    const auto within = de::platform::File::open_read(files.path("sub/folder/diff.png").c_str());
    REQUIRE(within.ok());
    const QuietLog quiet;
    CHECK(files.run_with({"@a.png", "@b.png", "--out", "@diff.png"}) == kExitDifferent);
    auto file = de::platform::File::open_read(files.path("diff.png").c_str());
    REQUIRE(file.ok());
    de::core::Array<std::byte> bytes(de::core::heap_allocator(de::core::MemoryTag::Tools));
    bytes.resize(static_cast<size_t>(*file->size()));
    REQUIRE(file->read_at(0, bytes.span()).ok());
    const auto difference = decode_png(bytes.span());
    REQUIRE(difference.ok());
    CHECK(get_pixel(*difference, 1, 0).r == 128 + 3);
    CHECK(get_pixel(*difference, 0, 0).r == 100 / 4);
}
