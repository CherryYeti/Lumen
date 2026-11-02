#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <mutex>

#include <netinet/in.h>
#include <thread>

namespace lumen {

// Head orientation in OpenVR's frame: right-handed, +Y up, -Z forward.
struct Orientation {
    float w = 1.f, x = 0.f, y = 0.f, z = 0.f;
};

// Buttons carried in "LMN4" pose packets (bitmask).
enum PoseButtons : uint32_t {
    kButtonTrigger = 1 << 0,      // pointer trigger (Gear VR: touchpad tap)
    kButtonTrackpadTouch = 1 << 1, // pointer trackpad touched (Gear VR: finger on the touchpad)
    kButtonSystem = 1 << 2,        // pointer system button: toggles the SteamVR dashboard (Gear VR: back)
};

// Gamepad buttons carried in "LMN4" packets (bitmask), Xbox layout.
enum GamepadButtons : uint32_t {
    kPadA = 1 << 0,
    kPadB = 1 << 1,
    kPadX = 1 << 2,
    kPadY = 1 << 3,
    kPadLB = 1 << 4,
    kPadRB = 1 << 5,
    kPadLS = 1 << 6, // left stick click
    kPadRS = 1 << 7, // right stick click
    kPadStart = 1 << 8,
    kPadSelect = 1 << 9,
    kPadGuide = 1 << 10,
    kPadConnected = 1u << 31, // a gamepad is connected to the phone
};

// Gamepad state (LMN4). Sticks -1..1 with +y up; triggers 0..1.
struct GamepadState {
    uint32_t buttons = 0; // GamepadButtons
    float lx = 0, ly = 0, rx = 0, ry = 0, lt = 0, rt = 0;
    bool connected() const { return buttons & kPadConnected; }
};

// Controller input that rides along with the head pose.
struct PoseInput {
    uint32_t buttons = 0;  // PoseButtons
    float padX = 0, padY = 0; // trackpad position, -1..1, +y up
    GamepadState gamepad;
};

// Wire format of one pose packet (little-endian):
//   char[4]  magic    "LMN1" (24 bytes, head pose only; the phone app) or
//                     "LMN4" (64, adds controller input; the Gear VR app)
//   uint32   sequence (packets with a lower sequence than the last are dropped)
//   float32  w, x, y, z  (unit quaternion, OpenVR frame)
//   uint32   buttons     (LMN4; PoseButtons, the current state)
//   float32  padX, padY  (LMN4; trackpad position -1..1, +y up)
//   uint32   gamepad buttons (LMN4; GamepadButtons)
//   float32  lx, ly, rx, ry (LMN4; sticks -1..1, +y up), lt, rt (triggers 0..1)
// Every packet carries the full button state, so a lost packet only delays a
// press or release by one pose interval.
struct PosePacket {
    char magic[4];
    uint32_t sequence;
    float w, x, y, z;
    uint32_t buttons;
    float padX, padY;
    uint32_t padButtons;
    float lx, ly, rx, ry, lt, rt;
};
static_assert(sizeof(PosePacket) == 64, "PosePacket must be packed");

// Battery packet, same port, sent every few seconds (little-endian, 12 bytes):
//   char[4]  magic   "LMB1"
//   float32  level   0..1
//   uint32   flags   bit 0 = charging
struct BatteryPacket {
    char magic[4];
    float level;
    uint32_t flags;
};
static_assert(sizeof(BatteryPacket) == 12, "BatteryPacket must be packed");

// Rumble, PC -> phone, sent from the pose port to wherever poses come from
// (little-endian, 20 bytes). One packet per haptic event a game triggers:
//   char[4]  magic     "LMH1"
//   uint32   hand      0 = left, 1 = right (gamepad Touch controller)
//   float32  duration  seconds (0: one short pulse)
//   float32  frequency Hz (a gamepad can't follow it; informational)
//   float32  amplitude 0..1
struct HapticPacket {
    char magic[4];
    uint32_t hand;
    float duration, frequency, amplitude;
};
static_assert(sizeof(HapticPacket) == 20, "HapticPacket must be packed");

// What the phone would like SteamVR to render, same port, every few seconds
// (little-endian, 16 bytes). The driver adopts it (render size = eye / 1.5,
// SteamVR's direct-mode output scale) and asks for a SteamVR restart when it
// differs from what's running:
//   char[4]  magic       "LMD1"
//   uint32   eyeWidth    per-eye output size the phone wants, px
//   uint32   eyeHeight
//   float32  fovDegrees  horizontal per-eye field of view
struct DisplayPacket {
    char magic[4];
    uint32_t eyeWidth, eyeHeight;
    float fovDegrees;
};
static_assert(sizeof(DisplayPacket) == 16, "DisplayPacket must be packed");

// Phone log line, same port, about once a second: "LML1" + UTF-8 text (no
// terminator). The driver writes it to SteamVR's log, since adb isn't
// reachable with the phone in a headset.

// Listens for pose packets on a UDP port and calls onPose for each valid one
// (and onBattery for battery packets).
class PoseReceiver {
public:
    using Callback = std::function<void(const Orientation &, const PoseInput &)>;
    using BatteryCallback = std::function<void(float level, bool charging)>;
    using LogCallback = std::function<void(const std::string &line)>;
    using DisplayCallback = std::function<void(uint32_t eyeWidth, uint32_t eyeHeight, float fovDegrees)>;

    bool Start(uint16_t port, Callback onPose, BatteryCallback onBattery = nullptr, LogCallback onLog = nullptr,
               DisplayCallback onDisplay = nullptr);
    void Stop();
    // Sends to the address the latest pose came from; false if none yet.
    bool SendToPhone(const void *data, size_t size);

private:
    void Loop();

    int socket_ = -1;
    std::atomic<bool> running_{false};
    std::thread thread_;
    Callback onPose_;
    BatteryCallback onBattery_;
    LogCallback onLog_;
    DisplayCallback onDisplay_;
    uint32_t lastSequence_ = 0;
    bool haveSequence_ = false;
    std::chrono::steady_clock::time_point lastPacketTime_;
    std::mutex phoneMutex_;
    sockaddr_in phone_{}; // guarded by phoneMutex_; sin_family 0 = unknown
};

} // namespace lumen
