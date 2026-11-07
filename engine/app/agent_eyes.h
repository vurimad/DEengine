#pragma once

#include "platform/file.h"
#include "tools/camera_script.h"
#include "tools/screenshot.h"

#include <cstdint>
#include <optional>

namespace de::asset {
class AssetRegistry;
}
namespace de::platform {
class Input;
class Vfs;
class Window;
} // namespace de::platform
namespace de::render {
class GpuAssets;
class RenderGraph;
struct GraphTexture;
} // namespace de::render
namespace de::rhi {
class Device;
class Uploader;
} // namespace de::rhi

namespace de::app {

struct FrameworkArgs;

// The frames a picture waits for its history to settle: measured in the test world, where after this no pixel is over
// a level from frame 2048 (engine/app/README.md). Whole periods of the 16-sample jitter. A new number means new
// baselines.
inline constexpr uint32_t kSettleFrames = 96;

// The frame clock's step in a still or a flight unless --fixed-step says otherwise.
inline constexpr double kEyesFrameS = 1.0 / 60.0;

// What the command line asks of the agent's eyes (dev builds; see engine/app/README.md). Paths are VFS paths
// ("src:/build/eyes/camp.png").
//   --camera X,Y,Z,YAW_DEG,PITCH_DEG  place the free camera (meters, degrees), and fly from there unless scripted
//   --view NAME               the same, from a named view
//   --views-file PATH         where the views are (default: camera.views in src:/config, or game:/config)
//   --still PATH              a settled, repeatable picture and exit: --screenshot at the settle frame, the fixed
//                             step of kEyesFrameS, no vsync
//   --screenshot PATH         write the frame's final image as a PNG, then exit unless --frames says go on
//   --screenshot-frame N      at this frame, the first being 1 (default: the settle frame)
//   --settle N                what the settle frame is (default kSettleFrames)
//   --fixed-step S            the frame clock advances by S seconds a frame, whatever time passes
//   --camera-path PATH        fly a path file: hold at its start for the settle frames, fly, exit at its end. A
//                             fixed step of kEyesFrameS unless --fixed-step says otherwise, no vsync
//   --path-speed M/S          over the path file's speed
//   --path-shots PREFIX       a picture every --path-every frames as PREFIX_0000.png, _0001 and so on
//   --path-every N            how many frames (default 30)
// A still, a screenshot or a path makes the run scripted: it takes no input (a stray key or a drifting stick
// would change its pictures), shows no debug UI, and starts its first frame with the scene's assets on the GPU.
struct EyesFlags {
    std::optional<tools::CameraPose> camera; // --camera
    const char* view = nullptr;
    const char* views_file = nullptr;
    const char* screenshot = nullptr;
    uint64_t screenshot_frame = 0; // the first frame is 1
    uint64_t run_frames = 0;       // --frames, for a run that goes on after its picture
    uint32_t settle_frames = kSettleFrames;
    double fixed_step_s = 0.0; // 0: the frame clock follows real time
    const char* path_file = nullptr;
    const char* path_shots = nullptr;
    uint32_t path_every = 30;
    double path_speed_mps = 0.0; // 0: the path file's
    bool no_vsync = false;       // a still or a path has no reason to wait for the screen

    bool scripted() const { return screenshot != nullptr || path_file != nullptr; }
};

// False, logged, on a malformed flag or flags that can't go together. Reads args.argc, args.argv and args.frames.
bool parse_eyes_flags(const FrameworkArgs& args, EyesFlags& out);

// The agent's eyes, in the frame loop: where the camera goes each frame, which frames become pictures, when a
// script is done. With no flags it costs a few branches a frame, and allocates and reads back nothing.
class AgentEyes {
public:
    // Reads the files the flags name, and checks the paths it'll write to, before there's a window. False (logged):
    // the run shouldn't start. default_views: where the views are without --views-file.
    bool start(const EyesFlags& flags, const platform::Vfs& vfs, const char* default_views);

    bool scripted() const { return flags_.scripted(); }
    bool takes_pictures() const { return flags_.screenshot != nullptr || flags_.path_shots != nullptr; }

    // A scripted run's first frame starts with everything already loaded, whatever the disk and the job system's
    // timing did: otherwise, assets land on a different frame each run. Records the uploads on frames of its own,
    // which present nothing and don't count. False (logged) if it isn't done in a couple of minutes.
    bool wait_for_scene(platform::Window& window, platform::Input& input, asset::AssetRegistry& assets,
                        render::GpuAssets& gpu_assets, rhi::Uploader& uploader, rhi::Device& device) const;

    // What the frame moves things by, from what the frame clock says.
    double frame_s(double clock_s) const { return flags_.fixed_step_s > 0.0 ? flags_.fixed_step_s : clock_s; }

    // At each frame's start: where the camera goes this frame, or null to leave it alone. Good until the next call.
    const tools::CameraPose* begin_frame();
    // After the frame's other passes: the pass that copies its final image, if it's due for a picture.
    void add_picture_pass(rhi::Device& device, render::RenderGraph& graph, render::GraphTexture back,
                          rhi::TextureHandle backbuffer);
    // After device.end_frame(): writes the picture, if there's one. A frame not drawn (the window minimized) fails a
    // scripted run: the history its picture rests on is gone.
    void end_frame(rhi::Device& device, const platform::Vfs& vfs, bool drew);
    // Whether the run should end: it's done what it came for, or failed.
    bool done() const { return failed_ || (last_frame_ != 0 && frame_ >= last_frame_); }
    // Once, after the last frame: lets go of the GPU's memory. False (logged) if the run failed or stopped short.
    bool finish(rhi::Device& device);

    // The plan, frame by frame (the first is 1); what the calls above follow. 0: no end of its own.
    uint64_t last_frame() const { return last_frame_; }
    // The camera in a frame: null to leave it alone.
    const tools::CameraPose* pose_at(uint64_t frame);
    // The picture's path if a frame has one.
    bool picture_at(uint64_t frame, platform::PathString& path) const;

private:
    EyesFlags flags_{};
    std::optional<tools::CameraPose> pose_; // --camera or --view; for a path, the frame's
    std::optional<tools::CameraPath> path_;
    uint64_t last_frame_ = 0;
    uint64_t frame_ = 0;
    tools::Screenshot screenshot_;
    platform::PathString picture_path_;
    bool picture_due_ = false;
    bool failed_ = false;
};

} // namespace de::app
