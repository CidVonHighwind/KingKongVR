// Screen aspect (kingkong9d.exe, from disassembly and runtime traces).
//
// The engine fits every view into its target with the function at 0x96a2f0.
// It takes a height/width ratio from a table at 0xaebf50, indexed by a view
// mode ([ecx+0x1c], 1..3):
//
//   0xaebf50  1.0        (index 0, unused: the code only reads 1..3)
//   0xaebf54  1.0        index 1, also the fallback when flag bit 0 is clear
//   0xaebf58  0.5625     index 2: 9/16
//   0xaebf5c  0.562745   index 3: ~9/16
//
//   0x96a30a  fld dword ptr [0xaebf54]           D9 05 54 BF AE 00
//   0x96a31e  fld dword ptr [eax*4 + 0xaebf50]   D9 04 85 50 BF AE 00
//
// With the 9/16 entries the game letterboxes on any non-16:9 backbuffer: at
// 1920x1440 the scene goes into a 1920x1080 viewport at y=180, the post
// effects composite only that band, and the projection stays 16:9. Writing
// the backbuffer's own height/width into both 9/16 entries makes the engine
// lay out the view, projection and post effects for the real aspect.
#pragma once

namespace kkvr {

// Set the engine's widescreen view ratio to height/width. Checks the table
// and the two instructions that read it first; another build of the game is
// left alone. Returns true if patched. Calling it again (after Reset) with a
// new size is fine.
bool PatchScreenAspect(unsigned width, unsigned height);

}  // namespace kkvr
