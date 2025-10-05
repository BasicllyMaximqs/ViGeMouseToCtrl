# ViGeMouseToCtrl
Maps inputs from keyboard/mouse to joystick using ViGEmClient (cpp wrapper).
I made this to play around with low-level tools (https://github.com/nefarius/ViGEmBus/).

### How it works

Tested with THE FINALS in attempt to abuse the controller aim assist feature.
Since THE FINALS paired with EasyAntiCheat (the anti-cheat officially tested with this project) uses DirectInput for the Mouse, the mouse grabbed controller and mouse inputs at the same time.
Given these circumstances, a UDP server (listening on port 12345) waits for mouse input and maps it to the Right Joystick for mouse movement.
The interception driver (https://github.com/oblitum/Interception) is flagged with EasyAntiCheat by default and therefor disables Auto-Aim even when interception initializes in runtime.

As in hotfix 1.4.1 (https://www.reachthefinals.com/patchnotes/141) - "Clients running key re-mapping programs on PC will not have access to aim assist." the interception driver has been picked up on.

I tried Gyroscopic Input using the DS4 class using ViGEmBus (as the original DualShock 4 controller has gyro to a certain extent), however DirectInput/whatever method THE FINALS uses only picks up on that with newer generation DS5 controllers. Gyro Input would be clamped to a extent where it wont make a large enough effect.

### Networking

The networking consists of a linux-version of mouse locking and sending the deltas to the main server (see udp_mouse_sender.cpp) via UDP for minimum latency.

### Important Notes

You MAY be banned using this, even though ViGEmBus/Client has legitimate use as EasyAntiCheat most likely has detection vectors set up for similar "loopholes".
Weird input is expected, as:

- Velocity capping (meaning as per XINPUT standard - theres only 0-36727) meaning you can barely flick with the mapper
- Joystick Inertia making it impossible to have a true pixel-stop like on a mouse
- Low velocity jitter (moving the mouse at slow speeds) barely works with joystick.

(atleast for FPS games).

If you're not comftorable with using UDP because you're scared packet loss will occur since UDP is send-n-forget (even though you should only use this on local internet)
you can set rewrite the portion to have a TCP server instead, or if you're planning on making a sender client for the mouse on windows, make sure to follow this struct:

``
struct MousePayload {
    LONG deltaX;
    LONG deltaY;
    bool mLMB;
    bool mRMB;
};
``
