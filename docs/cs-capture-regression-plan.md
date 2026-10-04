# Community Shaders capture regression: historical evidence

## Original fixes

`dea38d4` (preserve fades and support Community Shaders) introduced separate-UI
handling: capture the world from `kFRAMEBUFFER.SRV` when Scaleform is redirected,
and composite into the scene so the downstream CS presentation path carries it.

`21e1d0e` moved capture/composition after the chained CS image-space callback.
The post-load fade waits for that callback to resume and samples `frozenFrameView`.
The commit explains that this avoids processing a retained image with live DLSS
motion data. It also suppresses generated frames during transition callbacks.

`b7b2e22` (finalize transition compatibility for 1.0.7.0) contains the explicit
lighting fix. Its source says the final swap-chain capture includes render-extension
lighting applied after the native scene target. It:

- Keeps `sceneFrame` separate from the post-CS `frozenFrame` used for the fade.
- Updates the scene fallback at Scaleform begin, then captures swap-chain buffer 0
  into `sceneFrame` at Present when the CS post-processing hook is active.
- Marks that capture with `sceneFrameContainsFinalOutput` and prefers it for loading
  presentation when its descriptor matches the output target.
- Retains the floating-point CS HDR target and refills it during loading, so CS
  applies its normal output transform even while image-space rendering is suspended.
- Locks both rolling captures at the native door fade request, before a door callback
  can detach or disable a carried/dynamic light.

These mechanisms remain in `8deeaff`, the parent of the stabilization commit.

## Regression introduced by stabilization

`ced96af` removes `sceneFrame`, the final-output marker, and retained CS HDR target.
It replaces the separate capture resources with one persistent transition image.
It also flips the Present capture gate from `compositeAfterPostProcessing` to
`!compositeAfterPostProcessing`, so that path no longer captures final output with
the CS hook active. The early door lock remains.

This is direct evidence that the earlier lighting compatibility mechanisms were
removed. It does not establish which CS configuration the user's latest test used.

## Failed follow-up and correction

The follow-up restored only pre-Scaleform capture and removed the post-CS copy.
That did not restore the final-output capture/HDR replay mechanisms, and changed
the texture feeding the fade. The user reported unchanged lighting loss and a
broken fade-in. The follow-up source change was backed out before this correction.

The correction restores separate final-output and post-CS rolling captures, then
freezes both into independent plugin-owned snapshots. Loading presentation prefers
the final output only when it matches the destination descriptor. The destination
crossfade continues sampling the post-CS snapshot; its timing and handoff gates
are unchanged. The CS HDR destination is retained and refilled during loading as in
`b7b2e22`. Proxy interop output is captured after downstream Present; native DXGI
output is captured before Present. A pre-UI pass cannot overwrite a completed CS
output with an earlier scene representation. The existing early door lock remains.

## In-game acceptance (pending)

Compare lighting immediately before a door load with the retained loading image,
including a carried/dynamic light. Check the color fade into loading, the hold, and
the fade into the destination. Repeat cold loads, in-game save loads, and main-menu
save loads with CS HDR/Linear Lighting and frame generation off/on where supported.
Check a run without CS for HUD capture and transition regressions. Record the CS
version and configuration; compilation does not verify visual or color-space fidelity.
