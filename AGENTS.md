# REA workflow for the Black recomp

This project reconstructs and recompiles *Black* for PS2. Use the installed
REA skill and `rea` MCP tools whenever an answer depends on the original ISO,
ELF, native code, runtime behavior, graphics captures, or comparison with
PCSX2. For ordinary questions answerable from this source tree, use the source
directly.

## Evidence and investigation

- Start from the exact artifact the user supplied or the current project
  artifact. Record its path and SHA-256 when comparing captures or binaries.
- Separate direct observations from inferences and unknowns. Preserve the
  REA Evidence IDs and the limits of each capture or analysis in project notes.
- For texture investigations, compare uploaded bytes and hashes across captures;
  GS VRAM addresses are reused and relocation alone does not identify an asset.
- For camera and geometry investigations, compare the same scene and frame,
  including initial GS state such as XYOFFSET and SCISSOR. Do not infer a camera
  transform from one screen image alone.
- Correlate findings with the translated EE/VU code, DMA/VIF/GIF events, and GS
  frontend only when each source supports the link. A matching upload does not
  by itself prove which object appeared on screen.
- Prefer a focused REA overview followed by targeted evidence inspection. Do
  not repeat an identical analysis when its result is already available.

## Runtime and graphics constraints

- The user requires GS graphics work to execute through the SDL_GPU backend and
  forbids CPU graphics fallbacks. Keep CPU work for guest state, scheduling,
  command preparation, and data management separate from actual GS rendering.
- Do not present fallback paths or unsupported operations routed to CPU as GPU
  progress. Report unsupported GS behavior explicitly and trace it to evidence.
- When launching the recomp for a requested capture, use the project's
  auto-closing runner and keep the capture tied to the exact build and run log.
  Do not leave the game open after an automated run.

