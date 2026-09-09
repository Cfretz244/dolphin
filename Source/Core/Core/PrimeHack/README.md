# Experimental Trilogy Prime 1 controls

This is a focused port of PrimeHack's NTSC-U Trilogy Prime 1 FPS-control logic,
not the complete PrimeHack distribution. Reference: [shiiion/dolphin at
53f53e0f5bad27ad62a807cb93b136d84f68777f](https://github.com/shiiion/dolphin/tree/53f53e0f5bad27ad62a807cb93b136d84f68777f/Source/Core/Core/PrimeHack).
The patch addresses/instructions, object offsets, camera rotation, lock-on pitch,
and reticle behavior derive from `Mods/FpsControls.cpp`, `AddressDBInit.cpp`, and
`Transform.cpp`. The input adapter and expected-original-word checks are local.
Source remains under GPL-2.0-or-later, as part of Dolphin.

Build with `ENABLE_PRIMEHACK=ON` (the unified stack does this for macOS/iOS).
Runtime is off by default: `-C Dolphin.PrimeHack.Enabled=True` opts in. The Delta
bridge opts in automatically, but the runtime gates itself to Wii `R3ME01`, rev 0.
The current port supports MP1 NTSC-U only. Netplay and movie sessions disable it:
camera input is host-side state and is not yet serialized into those protocols.

Every frame at PatchEngine's CPU safe point, check the MP1 discriminator and all
eight original/replacement instructions before writing any patch. Unknown or
partially loaded code is left alone. Changed instructions go through Dolphin's
instruction-cache invalidation, including AOT guard generation invalidation.
Turning the option off restores the known originals when the same image is
present. Reloaded original code is patched again; no persistent image activation
is trusted.

For native patched blocks, build the CFG with `--dol-images --primehack`.
`Patches.h` is shared by the runtime and offline image builder. The builder
requires R3ME01 revision 0, the MP1 discriminator, and all eight original words;
it derives four patch-mode images from those verified disc bytes. Their identities
include the complete original DOL hash and exact patch recipe. Translation checks
those identities again. Only changed blocks get extra native candidates; common
code remains shared with the original DOL. Existing instruction-content guards
select the matching candidate on mode changes, including stale direct entries.
Unknown modifications still fall back to the interpreter. No trace snapshot is
accepted as executable code, and no instruction-validation checks are removed.

Wiimote 1 input is adapted from `Pad::GetStatus(0)` before normal Wiimote reports
are built. The desired-state variant attaches a Nunchuk automatically. On desktop,
configure GameCube controller 1 using Dolphin's normal controller settings; on
Delta, the existing external pad provider supplies the same layout. Wiimote 1
must use the emulated source. No physical Wii Remote is required.

If the emulated Wii Remote disconnects after inactivity, reconnect the physical
controller and press A. The Bluetooth activation poll reads GameCube pad 1 even
while normal Wii input reports are stopped. Dolphin's Connect Wii Remote 1
shortcut (Alt/Option + F5 by default) can also request reconnection.

| Pad input | Wii input / action |
| --- | --- |
| Left stick | Nunchuk movement |
| Right stick | FPS camera; relative IR pointer in menus |
| A or R/right trigger | Wii A: fire/charge, confirm |
| B | Wii B: jump/back |
| L/left trigger | Nunchuk Z: lock-on |
| X | Nunchuk C: morph ball |
| Y or D-pad down | Wii D-pad down: missiles |
| Z | Wii minus: original visor menu |
| Start | Wii plus: original beam menu |
| L + Start | Wii 1: map |
| Remaining D-pad directions | Wii D-pad |

The original beam/visor wheels remain enabled. Hold minus/plus and push the
right stick toward the desired screen quadrant, then release minus/plus. Wheel
input selects a direction directly rather than integrating pointer velocity;
it retains the last selection through stick release. A deliberate deflection is
required to change direction, preventing weak center noise from changing it.
During gameplay the emulated IR pointer stays centered, so it cannot compete
with host camera rotation. Reticle-forcing patches are restored to original
instructions while a selection wheel or pause menu owns the pointer.
Direct weapon/visor shortcuts, springball, MP2/MP3 support, custom guest opcodes,
and PrimeHack's desktop UI are not ported.

Freelook uses an 8% circular deadzone followed by a linear turn-rate response,
with no time-based acceleration or smoothing. Combined diagonal turn rate is
bounded to the same maximum as axial movement. Nunchuk and wheel input retain
their existing mappings. At each camera update, input is freshly sampled through
the pad provider rather than reusing the preceding Wii report.

Pitch/yaw targets accumulate while freelook owns the camera, as in upstream
PrimeHack; they are not reconstructed from a potentially game-modified transform
each frame. Ownership is relinquished for lock-on, wheels, pause, morph/cutscenes,
disconnect, invalid player/code, or disabled controls. Resuming freelook initializes
the target from guest state. Loading a savestate resets host control state without
changing the savestate format. Save-state transitions and full gameplay still
need interactive validation; RAM patch tests do not establish end-to-end latency.
