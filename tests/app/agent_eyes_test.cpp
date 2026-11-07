#include "app/agent_eyes.h"
#include "app/framework.h"
#include "core/log.h"
#include "core/memory_stats.h"
#include "image_diff/image_diff.h"
#include "platform/vfs.h"
#include "render/graph/render_graph.h"
#include "support/gpu.h"
#include "support/sandbox.h"

#include <doctest/doctest.h>
#include <glm/trigonometric.hpp>

#include <cstdlib>
#include <initializer_list>
#include <string_view>

using de::app::AgentEyes;
using de::app::EyesFlags;
using de::app::kEyesFrameS;
using de::app::kSettleFrames;
using de::platform::PathString;

namespace {

struct QuietLog {
    de::core::LogLevel saved = de::core::log_level();
    QuietLog() { de::core::set_log_level(de::core::LogLevel::Fatal); }
    ~QuietLog() { de::core::set_log_level(saved); }
    QuietLog(const QuietLog&) = delete;
    QuietLog& operator=(const QuietLog&) = delete;
    QuietLog(QuietLog&&) = delete;
    QuietLog& operator=(QuietLog&&) = delete;
};

// A command line that outlives what's parsed from it, as main's does. The flags are read the same in every build:
// only parse_args leaves them out of ship (the last test here).
struct CommandLine {
    char* argv[16] = {};
    de::app::FrameworkArgs args;

    explicit CommandLine(std::initializer_list<const char*> flags) {
        REQUIRE(flags.size() < 16);
        argv[args.argc++] = const_cast<char*>("deengine");
        for (const char* flag : flags) {
            argv[args.argc++] = const_cast<char*>(flag);
        }
        args.argv = argv;
        if (const char* frames = de::app::flag_value(args, "--frames")) {
            args.frames = std::strtoull(frames, nullptr, 10);
        }
    }

    bool parse(EyesFlags& out) const { return de::app::parse_eyes_flags(args, out); }
};

EyesFlags parsed(std::initializer_list<const char*> flags) {
    const CommandLine line(flags);
    EyesFlags out;
    REQUIRE(line.parse(out));
    return out;
}

constexpr const char* kViews = "src:/config/camera.views";

// A Vfs with a views file and a path file.
class Scene {
public:
    Scene() : vfs_(sandbox_.make_vfs()) {
        write(kViews, "camp 10 20 30 90 -10\nfar 16000.123456789 2 -16000.5 0 0\n");
        write("src:/ride.path", "# 13 m a second, by default\n0 0 0 0 0\n13 0 0 0 0\n");
    }
    void write(const char* path, std::string_view text) const {
        REQUIRE(vfs_.write_file(path, de::test::bytes_of(text)).ok());
    }
    bool start(AgentEyes& eyes, std::initializer_list<const char*> flags) const {
        const CommandLine line(flags);
        EyesFlags out;
        REQUIRE(line.parse(out));
        return eyes.start(out, vfs_, kViews);
    }
    const de::platform::Vfs& vfs() const { return vfs_; }

private:
    de::test::Sandbox sandbox_;
    de::platform::Vfs vfs_;
};

} // namespace

TEST_CASE("with no flags, the eyes do nothing") {
    const EyesFlags none = parsed({"--frames", "120", "--world", "00000000000000ab"});
    CHECK_FALSE(none.scripted());
    CHECK_FALSE(none.no_vsync);
    CHECK(none.fixed_step_s == 0.0);
    CHECK_FALSE(none.camera.has_value());

    const Scene scene;
    AgentEyes eyes;
    REQUIRE(scene.start(eyes, {"--frames", "120"}));
    CHECK_FALSE(eyes.scripted());
    CHECK_FALSE(eyes.takes_pictures());
    CHECK(eyes.last_frame() == 0);
    CHECK(eyes.frame_s(0.123) == 0.123); // the frame clock's
    for (int i = 0; i < 100; ++i) {
        CHECK(eyes.begin_frame() == nullptr);
    }
    CHECK_FALSE(eyes.done());
}

TEST_CASE("a camera on the command line is placed once, and flown from there") {
    const EyesFlags flags = parsed({"--camera", "16000.123456789,2,-3,90,-10"});
    CHECK(flags.camera.value_or(de::tools::CameraPose{}).position.x == 16000.123456789); // kept in a double
    CHECK_FALSE(flags.scripted());
    CHECK(flags.fixed_step_s == 0.0);

    const Scene scene;
    AgentEyes eyes;
    REQUIRE(scene.start(eyes, {"--camera", "16000.123456789,2,-3,90,-10"}));
    const de::tools::CameraPose* first = eyes.begin_frame();
    REQUIRE(first != nullptr);
    CHECK(first->position.x == 16000.123456789);
    CHECK(first->yaw_rad == doctest::Approx(glm::radians(90.0f)));
    CHECK(first->pitch_rad == doctest::Approx(glm::radians(-10.0f)));
    CHECK(eyes.begin_frame() == nullptr); // the input has it now
}

TEST_CASE("a named view comes from the views file, the default one or another") {
    const Scene scene;
    {
        AgentEyes eyes;
        REQUIRE(scene.start(eyes, {"--view", "far"}));
        const de::tools::CameraPose* pose = eyes.pose_at(1);
        REQUIRE(pose != nullptr);
        CHECK(pose->position.x == 16000.123456789);
        CHECK(pose->position.z == -16000.5);
    }
    {
        scene.write("user:/mine.views", "far 1 2 3 0 0\n");
        AgentEyes eyes;
        REQUIRE(scene.start(eyes, {"--view", "far", "--views-file", "user:/mine.views"}));
        const de::tools::CameraPose* pose = eyes.pose_at(1);
        REQUIRE(pose != nullptr);
        CHECK(pose->position.x == 1.0);
    }
    const QuietLog quiet;
    AgentEyes missing_view;
    CHECK_FALSE(scene.start(missing_view, {"--view", "nowhere"}));
    AgentEyes missing_file;
    CHECK_FALSE(scene.start(missing_file, {"--view", "far", "--views-file", "user:/gone.views"}));
    scene.write("user:/broken.views", "far 1 2 3 0 0\nbroken 1 2\n");
    AgentEyes broken_file;
    CHECK_FALSE(scene.start(broken_file, {"--view", "far", "--views-file", "user:/broken.views"}));
}

TEST_CASE("a still is a repeatable picture at the settle frame, and then the end") {
    const EyesFlags flags = parsed({"--still", "user:/a.png"});
    CHECK(flags.scripted());
    CHECK(flags.no_vsync);
    CHECK(flags.fixed_step_s == kEyesFrameS);
    CHECK(flags.screenshot_frame == kSettleFrames);
    CHECK(parsed({"--still", "user:/a.png", "--settle", "32"}).screenshot_frame == 32);
    CHECK(parsed({"--still", "user:/a.png", "--fixed-step", "0.25"}).fixed_step_s == 0.25);

    const Scene scene;
    AgentEyes eyes;
    REQUIRE(scene.start(eyes, {"--view", "camp", "--still", "user:/a.png", "--settle", "4"}));
    CHECK(eyes.scripted());
    CHECK(eyes.takes_pictures());
    CHECK(eyes.last_frame() == 4);
    CHECK(eyes.frame_s(0.123) == kEyesFrameS); // whatever the clock says
    PathString path;
    uint64_t frames = 0;
    while (!eyes.done()) {
        const de::tools::CameraPose* pose = eyes.begin_frame();
        REQUIRE(pose != nullptr);
        CHECK(pose->position.x == 10.0); // held there, every frame
        CHECK(pose->position.z == 30.0);
        ++frames;
        REQUIRE(frames < 100);
    }
    CHECK(frames == 4);
    CHECK_FALSE(eyes.picture_at(3, path));
    CHECK(eyes.picture_at(4, path));
    CHECK((path.view() == "user:/a.png"));
    CHECK_FALSE(eyes.picture_at(5, path));
}

TEST_CASE("a screenshot is at the frame it says, on the clock it's given, and the run can go on after") {
    const EyesFlags flags = parsed({"--screenshot", "user:/a.png"});
    CHECK(flags.scripted());
    CHECK_FALSE(flags.no_vsync); // not asked for
    CHECK(flags.fixed_step_s == 0.0);
    CHECK(flags.screenshot_frame == kSettleFrames);

    const Scene scene;
    AgentEyes eyes;
    REQUIRE(scene.start(eyes, {"--screenshot", "user:/a.png", "--screenshot-frame", "7", "--frames", "20"}));
    CHECK(eyes.frame_s(0.123) == 0.123);
    CHECK(eyes.last_frame() == 20);
    PathString path;
    CHECK(eyes.picture_at(7, path));
    CHECK_FALSE(eyes.picture_at(20, path));

    AgentEyes held;
    REQUIRE(scene.start(held, {"--screenshot", "user:/a.png", "--screenshot-frame", "3"}));
    CHECK(held.last_frame() == 3);
    CHECK(held.pose_at(1) == nullptr); // no camera asked for: the world's start stands
}

TEST_CASE("a path holds at its start to settle, flies at its speed, takes numbered pictures, and ends") {
    const Scene scene;
    AgentEyes eyes;
    REQUIRE(scene.start(eyes, {"--camera-path", "src:/ride.path", "--settle", "8", "--path-shots", "user:/shots/ride",
                               "--path-every", "20"}));
    CHECK(eyes.scripted());
    CHECK(eyes.takes_pictures());
    CHECK(eyes.frame_s(0.123) == kEyesFrameS);
    CHECK(eyes.last_frame() == 8 + 61); // 13 m at 13 m/s: a second, at 60 frames and the one it starts on
    for (uint64_t frame = 1; frame <= 9; ++frame) {
        const de::tools::CameraPose* pose = eyes.pose_at(frame);
        REQUIRE(pose != nullptr);
        CHECK(pose->position.x == 0.0);
    }
    const de::tools::CameraPose* moving = eyes.pose_at(10);
    REQUIRE(moving != nullptr);
    CHECK(moving->position.x == doctest::Approx(13.0 / 60.0)); // a gallop's frame
    const de::tools::CameraPose* last = eyes.pose_at(eyes.last_frame());
    REQUIRE(last != nullptr);
    CHECK(last->position.x == 13.0);

    // Pictures: from the frame the flight starts, one every 20.
    PathString path;
    CHECK_FALSE(eyes.picture_at(8, path));
    REQUIRE(eyes.picture_at(9, path));
    CHECK((path.view() == "user:/shots/ride_0000.png"));
    CHECK_FALSE(eyes.picture_at(10, path));
    CHECK_FALSE(eyes.picture_at(28, path));
    REQUIRE(eyes.picture_at(29, path));
    CHECK((path.view() == "user:/shots/ride_0001.png"));
    REQUIRE(eyes.picture_at(69, path));
    CHECK((path.view() == "user:/shots/ride_0003.png"));
    CHECK_FALSE(eyes.picture_at(68, path));

    uint64_t frames = 0;
    while (!eyes.done()) {
        static_cast<void>(eyes.begin_frame());
        ++frames;
        REQUIRE(frames < 1000);
    }
    CHECK(frames == eyes.last_frame());
}

TEST_CASE("a path's speed comes from its file, or from the flag over it") {
    const Scene scene;
    scene.write("src:/slow.path", "speed 1.3\n0 0 0 0 0\n13 0 0 0 0\n");
    AgentEyes from_file;
    REQUIRE(scene.start(from_file, {"--camera-path", "src:/slow.path", "--settle", "1"}));
    CHECK(from_file.last_frame() == 1 + 601);
    AgentEyes from_flag;
    REQUIRE(scene.start(from_flag, {"--camera-path", "src:/slow.path", "--settle", "1", "--path-speed", "26"}));
    CHECK(from_flag.last_frame() == 1 + 31);
    AgentEyes other_step;
    REQUIRE(scene.start(other_step, {"--camera-path", "src:/ride.path", "--settle", "1", "--fixed-step", "0.5"}));
    CHECK(other_step.frame_s(0.123) == 0.5);
    CHECK(other_step.last_frame() == 1 + 3);
    // A path with no pictures still flies, and takes none.
    PathString path;
    CHECK_FALSE(other_step.takes_pictures());
    CHECK_FALSE(other_step.picture_at(2, path));
}

TEST_CASE("the flags are refused if they make no sense, or none together") {
    // Each of these is what its rejections below differ from.
    for (const std::initializer_list<const char*> good :
         {std::initializer_list<const char*>{"--camera", "1,2,3,4,5"},
          {"--view", "a", "--views-file", "user:/v"},
          {"--still", "user:/a.png", "--settle", "1"},
          {"--still", "user:/a.png", "--settle", "8", "--frames", "8"},
          {"--screenshot", "user:/a.png", "--screenshot-frame", "1"},
          {"--screenshot", "user:/a.png", "--settle", "1"},
          {"--camera-path", "user:/p", "--settle", "1"},
          {"--screenshot", "user:/a.png", "--screenshot-frame", "2", "--frames", "2"},
          {"--fixed-step", "0.001"},
          {"--fixed-step", "1"},
          {"--camera-path", "user:/p", "--path-speed", "1", "--path-shots", "user:/s", "--path-every", "1"}}) {
        const CommandLine line(good);
        EyesFlags out;
        CHECK(line.parse(out));
    }
    const QuietLog quiet;
    for (const std::initializer_list<const char*> bad :
         {std::initializer_list<const char*>{"--camera"},
          {"--camera", "1,2,3,4"},
          {"--camera", "1,2,3,4,89"},
          {"--view"},
          {"--still"},
          {"--screenshot"},
          {"--camera-path"},
          {"--still", "user:/a.png", "--settle", "0"},
          {"--still", "user:/a.png", "--settle", "x"},
          {"--still", "user:/a.png", "--settle", "-1"},
          {"--still", "user:/a.png", "--settle", "1x"},
          {"--screenshot", "user:/a.png", "--screenshot-frame", "0"},
          {"--fixed-step", "0"},
          {"--fixed-step", "-1"},
          {"--fixed-step", "nan"},
          {"--fixed-step", "inf"},
          {"--fixed-step", "1.5"},
          {"--fixed-step", "0.1x"},
          {"--fixed-step"},
          {"--camera-path", "user:/p", "--path-speed", "0"},
          {"--camera-path", "user:/p", "--path-speed", "nan"},
          {"--camera-path", "user:/p", "--path-shots", "user:/s", "--path-every", "0"},
          // Together:
          {"--camera", "1,2,3,4,5", "--view", "a"},
          {"--still", "user:/a.png", "--screenshot", "user:/b.png"},
          {"--still", "user:/a.png", "--screenshot-frame", "1"},
          {"--camera-path", "user:/p", "--camera", "1,2,3,4,5"},
          {"--camera-path", "user:/p", "--view", "a"},
          {"--camera-path", "user:/p", "--still", "user:/a.png"},
          {"--camera-path", "user:/p", "--screenshot", "user:/a.png"},
          {"--camera-path", "user:/p", "--frames", "10"},
          // Means nothing without:
          {"--settle", "5"},
          {"--views-file", "user:/v"},
          {"--screenshot-frame", "1"},
          {"--path-shots", "user:/s"},
          {"--path-speed", "1"},
          {"--path-every", "1"},
          {"--camera-path", "user:/p", "--path-every", "1"},
          // A run too short for its picture:
          {"--still", "user:/a.png", "--settle", "8", "--frames", "7"},
          {"--screenshot", "user:/a.png", "--screenshot-frame", "3", "--frames", "2"}}) {
        const CommandLine line(bad);
        EyesFlags out;
        CHECK_MESSAGE(!line.parse(out), *bad.begin());
    }
}

TEST_CASE("what can't work is found before there's a window") {
    const Scene scene;
    scene.write("src:/one.path", "0 0 0 0 0\n");
    {
        AgentEyes eyes;
        REQUIRE(scene.start(eyes, {"--still", "user:/a.png"}));
        AgentEyes shots;
        REQUIRE(scene.start(shots, {"--camera-path", "src:/ride.path", "--path-shots", "user:/s"}));
    }
    const QuietLog quiet;
    for (const std::initializer_list<const char*> bad :
         {std::initializer_list<const char*>{"--still", "game:/a.png"},       // read only
          {"--still", "user:/a.jpg"},                                         // writes a PNG
          {"--still", "user:/Capital.png"},                                   // the path rules
          {"--still", "nowhere:/a.png"},                                      //
          {"--still", "a.png"},                                               //
          {"--screenshot", "user:/../a.png"},                                 //
          {"--camera-path", "src:/ride.path", "--path-shots", "game:/s"},     //
          {"--camera-path", "src:/ride.path", "--path-shots", "user:/S"},     //
          {"--camera-path", "src:/gone.path"},                                //
          {"--camera-path", "src:/one.path"},                                 // not a path
          {"--camera-path", "src:/ride.path", "--fixed-step", "0.000000001"}, // would take years
          {"--camera-path", "src:/ride.path", "--path-speed", "0.0001"}}) {
        AgentEyes eyes;
        CHECK_MESSAGE(!scene.start(eyes, bad), *(bad.begin() + 1));
    }
}

namespace {

// The eyes' part of the frame loop, on a texture standing in for the backbuffer (there's no window here).
class FrameLoop {
public:
    FrameLoop(de::rhi::Device& gpu, de::render::RenderGraph& frame_graph, de::rhi::TextureUsage usage)
        : device_(gpu), graph_(frame_graph) {
        const auto made = device_.create_texture(
            {.width = kWidth, .height = kHeight, .format = de::rhi::Format::BGRA8Srgb, .usage = usage});
        REQUIRE(made.ok());
        texture_ = *made;
    }
    ~FrameLoop() { device_.destroy(texture_); }
    FrameLoop(const FrameLoop&) = delete;
    FrameLoop& operator=(const FrameLoop&) = delete;
    FrameLoop(FrameLoop&&) = delete;
    FrameLoop& operator=(FrameLoop&&) = delete;

    // Blue, green, red, alpha.
    void fill(uint8_t blue, uint8_t green, uint8_t red) {
        de::core::Array<std::byte> texels(de::core::heap_allocator(de::core::MemoryTag::Tools));
        texels.resize(size_t{kWidth} * kHeight * 4);
        for (size_t i = 0; i < size_t{kWidth} * kHeight; ++i) {
            texels[i * 4] = static_cast<std::byte>(blue);
            texels[(i * 4) + 1] = static_cast<std::byte>(green);
            texels[(i * 4) + 2] = static_cast<std::byte>(red);
            texels[(i * 4) + 3] = std::byte{7}; // alpha: not in the picture
        }
        de::test::upload_texture(device_, texture_, texels.span(), de::rhi::ResourceState::RenderTarget);
    }

    void frame(AgentEyes& eyes, const de::platform::Vfs& vfs, bool drew = true) {
        static_cast<void>(eyes.begin_frame());
        device_.begin_frame();
        graph_.begin(kWidth, kHeight);
        const de::render::GraphTexture back =
            graph_.import_texture({.texture = texture_,
                                   .name = "picture",
                                   .state = de::rhi::ResourceState::RenderTarget,
                                   .final_state = de::rhi::ResourceState::RenderTarget});
        if (drew) {
            eyes.add_picture_pass(device_, graph_, back, texture_);
        }
        graph_.execute();
        device_.end_frame();
        eyes.end_frame(device_, vfs, drew);
    }

    static constexpr uint32_t kWidth = 8;
    static constexpr uint32_t kHeight = 4;

private:
    de::rhi::Device& device_;
    de::render::RenderGraph& graph_;
    de::rhi::TextureHandle texture_;
};

constexpr de::rhi::TextureUsage kCopyable =
    de::rhi::TextureUsage::RenderTarget | de::rhi::TextureUsage::CopySrc | de::rhi::TextureUsage::CopyDst;

} // namespace

TEST_CASE("a screenshot is written on its frame, of what's there then, and the run ends there") {
    auto device = de::test::make_device();
    if (!device) {
        return;
    }
    auto graph = de::render::RenderGraph::create(*device);
    REQUIRE(graph.ok());
    const Scene scene;
    AgentEyes eyes;
    REQUIRE(scene.start(eyes, {"--screenshot", "user:/shots/a.png", "--screenshot-frame", "3"}));
    {
        FrameLoop loop(*device, *graph, kCopyable);
        loop.fill(10, 20, 30);
        loop.frame(eyes, scene.vfs());
        loop.frame(eyes, scene.vfs());
        CHECK_FALSE(scene.vfs().exists("user:/shots/a.png"));
        CHECK_FALSE(eyes.done());
        CHECK(device->memory_stats().category_count(de::rhi::GpuMemoryCategory::ReadbackBuffers) == 0);
        loop.fill(40, 50, 60); // what the frame shows, not what an earlier one did
        loop.frame(eyes, scene.vfs());
        REQUIRE(scene.vfs().exists("user:/shots/a.png"));
        CHECK(device->memory_stats().category_count(de::rhi::GpuMemoryCategory::ReadbackBuffers) == 1);
        CHECK(eyes.done());
    }
    CHECK(eyes.finish(*device));
    const auto file =
        scene.vfs().read_whole_file("user:/shots/a.png", de::core::heap_allocator(de::core::MemoryTag::Tools));
    REQUIRE(file.ok());
    const auto image = de::image_diff::decode_png(file->span());
    REQUIRE(image.ok());
    CHECK(image->width == FrameLoop::kWidth);
    CHECK(image->height == FrameLoop::kHeight);
    CHECK(image->rgba[0] == 60); // red, kept third
    CHECK(image->rgba[1] == 50);
    CHECK(image->rgba[2] == 40);
    CHECK(image->rgba[3] == 255);
    device->wait_idle();
    de::test::check_clean(*device);
}

TEST_CASE("a flight writes its numbered pictures, and ends") {
    auto device = de::test::make_device();
    if (!device) {
        return;
    }
    auto graph = de::render::RenderGraph::create(*device);
    REQUIRE(graph.ok());
    const Scene scene;
    AgentEyes eyes;
    // 13 m at 13 m/s and a quarter of a second a frame: frames 2 to 6 fly, and every other one is a picture.
    REQUIRE(scene.start(eyes, {"--camera-path", "src:/ride.path", "--settle", "1", "--fixed-step", "0.25",
                               "--path-shots", "user:/ride/s", "--path-every", "2"}));
    {
        FrameLoop loop(*device, *graph, kCopyable);
        loop.fill(1, 2, 3);
        uint64_t frames = 0;
        while (!eyes.done()) {
            loop.frame(eyes, scene.vfs());
            ++frames;
            REQUIRE(frames < 100);
        }
        CHECK(frames == 6);
    }
    CHECK(eyes.finish(*device));
    CHECK(scene.vfs().exists("user:/ride/s_0000.png"));
    CHECK(scene.vfs().exists("user:/ride/s_0001.png"));
    CHECK(scene.vfs().exists("user:/ride/s_0002.png"));
    CHECK_FALSE(scene.vfs().exists("user:/ride/s_0003.png"));
    device->wait_idle();
    de::test::check_clean(*device);
}

TEST_CASE("a script fails if it can't take its picture, loses a frame or stops short") {
    auto device = de::test::make_device();
    if (!device) {
        return;
    }
    auto graph = de::render::RenderGraph::create(*device);
    REQUIRE(graph.ok());
    const Scene scene;
    const auto one_frame = [&](AgentEyes& eyes, de::rhi::TextureUsage usage, bool drew) {
        FrameLoop loop(*device, *graph, usage);
        loop.fill(1, 2, 3);
        loop.frame(eyes, scene.vfs(), drew);
    };
    constexpr de::rhi::TextureUsage kNotCopyable = de::rhi::TextureUsage::RenderTarget | de::rhi::TextureUsage::CopyDst;
    {
        AgentEyes eyes; // the one all the failures differ from
        REQUIRE(scene.start(eyes, {"--screenshot", "user:/ok.png", "--screenshot-frame", "1"}));
        one_frame(eyes, kCopyable, true);
        CHECK(eyes.done());
        CHECK(eyes.finish(*device));
    }
    const QuietLog quiet;
    {
        AgentEyes eyes;
        REQUIRE(scene.start(eyes, {"--screenshot", "user:/ok.png", "--screenshot-frame", "1"}));
        one_frame(eyes, kNotCopyable, true); // a swapchain the display won't let be copied
        CHECK(eyes.done());
        CHECK_FALSE(eyes.finish(*device));
    }
    {
        AgentEyes eyes;
        REQUIRE(scene.start(eyes, {"--screenshot", "user:/ok.png", "--screenshot-frame", "2"}));
        one_frame(eyes, kCopyable, false); // the window minimized: its history is gone
        CHECK(eyes.done());
        CHECK_FALSE(eyes.finish(*device));
    }
    {
        scene.write("user:/blocked", "a file, where the picture wants a folder");
        AgentEyes eyes;
        REQUIRE(scene.start(eyes, {"--screenshot", "user:/blocked/a.png", "--screenshot-frame", "1"}));
        one_frame(eyes, kCopyable, true);
        CHECK(eyes.done());
        CHECK_FALSE(eyes.finish(*device));
    }
    {
        AgentEyes eyes;
        REQUIRE(scene.start(eyes, {"--screenshot", "user:/late.png", "--screenshot-frame", "5"}));
        one_frame(eyes, kCopyable, true);
        CHECK_FALSE(eyes.done());
        CHECK_FALSE(eyes.finish(*device)); // the window closed before its picture
        CHECK_FALSE(scene.vfs().exists("user:/late.png"));
    }
    {
        AgentEyes eyes; // no script: nothing to lose, and no end of its own
        REQUIRE(scene.start(eyes, {"--frames", "120"}));
        one_frame(eyes, kCopyable, false);
        CHECK_FALSE(eyes.done());
        CHECK(eyes.finish(*device));
    }
    device->wait_idle();
}

TEST_CASE("the eyes cost a frame no allocation and no readback, off or between pictures") {
    auto device = de::test::make_device();
    if (!device) {
        return;
    }
    auto graph = de::render::RenderGraph::create(*device);
    REQUIRE(graph.ok());
    const Scene scene;
    AgentEyes off;
    REQUIRE(scene.start(off, {}));
    AgentEyes between;
    REQUIRE(scene.start(between, {"--camera-path", "src:/ride.path", "--settle", "200", "--path-shots", "user:/s"}));
    for (AgentEyes* const eyes : {&off, &between}) {
        for (int i = 0; i < 100; ++i) {
            device->begin_frame();
            graph->begin(FrameLoop::kWidth, FrameLoop::kHeight);
            const uint64_t before = de::core::thread_heap_allocation_count();
            static_cast<void>(eyes->frame_s(0.1));
            static_cast<void>(eyes->begin_frame());
            // With no picture due it doesn't look at what it's given.
            eyes->add_picture_pass(*device, *graph, de::render::GraphTexture{}, de::rhi::TextureHandle{});
            CHECK(de::core::thread_heap_allocation_count() == before);
            graph->execute();
            CHECK(graph->stats().passes == 0);
            device->end_frame();
            const uint64_t after_frame = de::core::thread_heap_allocation_count();
            eyes->end_frame(*device, scene.vfs(), true);
            static_cast<void>(eyes->done());
            CHECK(de::core::thread_heap_allocation_count() == after_frame);
        }
    }
    CHECK(device->memory_stats().category_count(de::rhi::GpuMemoryCategory::ReadbackBuffers) == 0);
    device->wait_idle();
    CHECK(off.finish(*device));
}

TEST_CASE("the eyes are dev builds' only") {
    const CommandLine line({"--still", "user:/a.png", "--camera", "not a camera", "--settle", "0"});
    de::app::FrameworkArgs args;
#if DE_BUILD_DEV
    const QuietLog quiet;
    CHECK_FALSE(de::app::parse_args(line.args.argc, line.args.argv, args));
    const CommandLine good({"--still", "user:/a.png"});
    REQUIRE(de::app::parse_args(good.args.argc, good.args.argv, args));
    CHECK(args.eyes.scripted());
    CHECK_FALSE(args.vsync);
    const CommandLine keeps({"--screenshot", "user:/a.png"});
    de::app::FrameworkArgs kept;
    REQUIRE(de::app::parse_args(keeps.args.argc, keeps.args.argv, kept));
    CHECK(kept.vsync);
#else
    // Not read at all: nothing to refuse.
    REQUIRE(de::app::parse_args(line.args.argc, line.args.argv, args));
    CHECK(args.vsync);
#endif
}
