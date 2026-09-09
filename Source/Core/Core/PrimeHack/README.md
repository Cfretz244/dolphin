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
is trusted. AOT interprets modified blocks until patched native variants exist.

Wiimote 1 input is adapted from `Pad::GetStatus(0)` before normal Wiimote reports
are built. The desired-state variant attaches a Nunchuk automatically. On desktop,
configure GameCube controller 1 using Dolphin's normal controller settings; on
Delta, the existing external pad provider supplies the same layout. Wiimote 1
must use the emulated source. No physical Wii Remote is required.

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
and PrimeHack's desktop UI are not ported. Pitch/yaw derive from guest state each
frame rather than a host angle accumulator. Host-side pointer position is reset
between sessions. Save-state transitions and full gameplay still need interactive
validation; RAM patch unit tests alone do not establish playability.
