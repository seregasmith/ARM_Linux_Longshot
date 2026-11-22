# ReShade FX presets

Open **Graphics → Post-processing** in the Insert menu. The **ReShade preset** row offers Off, Warm, Cool, User and
twenty Clarity presets adapted to Zelda's SDR gameplay scene, HDR Vibrance, Depth View and Depth Haze.
Warm and Cool are built-in SDR grades. The renderer prepares an effect after it sees the gameplay scene at its
current size. The row shows the preparation result. If an import fails, the last working preset can continue at
the same scene size until you select Off or another preset.

During gameplay, open **Compare ReShade Preset** to see the scene with a compact toolbar. **Enabled** toggles the
effect. Use Up and Down to move between rows and Left and Right to browse presets, or click the arrows. When you
reach **User**, a third row lets you browse the imported INI files. **Back** returns to Graphics and restores the
saved preset and user file.

## Clarity for Zelda

Clarity is a collection of looks from the community Breath of the Wild graphics pack. Longshot adapts the grades to
Ocarina of Time's already tone-mapped SDR image. Exposure, levels, gamma, color balance, contrast and sharpening are
tuned for that input. The Cemu profiles also affect earlier stages such as bloom and tone mapping, so their look
varies in Longshot. Clarity Base uses the published starting values for the user-defined profile.

Try **Serfrost (Recommended)** for a balanced grade, **t3nka Neutral+** for a lighter change, or **Filmic** for softer
color with a stronger midtone curve. The Serfrost display variants and the two Survival of the Wild variants offer
different brightness and color balances. All twenty use one fullscreen FX pass.

The original profiles are by the named Clarity preset authors and the Cemu graphics pack contributors. The source revision is
[`496df203`](https://github.com/cemu-project/cemu_graphic_packs/tree/496df20311147cfd22523445eea71961115602ab).
The upstream names and settings are published under [CC0 1.0](https://github.com/cemu-project/cemu_graphic_packs/blob/496df20311147cfd22523445eea71961115602ab/LICENSE.md).
The [preset list](https://github.com/cemu-project/cemu_graphic_packs/blob/496df20311147cfd22523445eea71961115602ab/src/BreathOfTheWild/Enhancements/rules.txt)
and [Clarity settings](https://github.com/cemu-project/cemu_graphic_packs/blob/496df20311147cfd22523445eea71961115602ab/src/BreathOfTheWild/Enhancements/37040a485a29d54e_00000000000003c9_ps.txt)
record the original values. Longshot supplies its own ReShade FX shader. The shader, the other built-in presets and
the TintedHighlights example are covered by Longshot's GPL-3.0-or-later license. The FX compiler is BSD-3-Clause and
the PNG decoder uses stb_image's MIT license. Their licenses and provenance are included in the builder and the
generated game's `licenses` directory. Imported presets, shaders and textures are supplied by the player.

## Import a preset

Select User and activate **Rescan ReShade presets** to create the folders beside the active `longshot_options.ini`.
Place the original INI in `reshade/presets`, FX and FXH files in `reshade/shaders`, and PNG textures in
`reshade/textures`. Rescan again and select the file on **User preset file**. The file name is saved separately so
adding a new preset preserves the selection. Rescan also reloads the selected user preset after an edit.

Longshot reads `Techniques`, `TechniqueSorting`, global and per-effect `PreprocessorDefinitions`, and numeric
uniform values under each `[shader.fx]` section. This INI selects the upstream Daltonize shader.

```ini
Techniques=Daltonize@Daltonize.fx

[Daltonize.fx]
Type=0
```

Copy `Daltonize.fx` and `ReShade.fxh` from the reshade-shaders repository into the shader folder. If shaders in different
subfolders share a file name, give the relative file path in `Techniques`. An older bare technique name can resolve
to a shader with the same name. The menu and the run log report missing FX files, includes, texture files and
unsupported pass features. [Upstream ReShade shaders](https://github.com/crosire/reshade-shaders)

Try **TintedHighlights** for an example with two passes. The first makes a half-size RGBA16F highlight mask.
The second samples the mask and blends its tint over the gameplay scene. Use the same import steps with
[TintedHighlights.ini](../../scripts/reshade/examples/TintedHighlights.ini) and [TintedHighlights.fx](../../scripts/reshade/examples/TintedHighlights.fx).

## Supported effects

ReShade FX 6.0 presets can use fullscreen graphics passes, `COLOR`, `DEPTH`, numeric scalar and vector uniforms,
PNG textures, single-level RGBA8 or RGBA16F render targets, blending and color write masks. Targets can be sampled
after an earlier pass fills them. The runtime supports up to 32 techniques, 64 passes and 24 auxiliary textures
per preset. Compute, frame-to-frame effect history, mip chains, stencil, multiple render targets in one pass and
dynamic uniform sources such as timers and input are outside the supported subset.

**Depth View** shows the gameplay depth buffer and **Depth Haze** applies a distance tint. Imported effects see
the game's normalized depth, from 0 at the near plane to 1 at the far plane. Effects that need linear depth handle
linearization in their own shader. When a matching depth attachment is unavailable, Longshot leaves the gameplay
scene untouched for that frame.

**Expanded HDR** uses SDR presets before Longshot's HDR expansion. **Native HDR** supports the included presets.
Warm, Cool, Clarity and Depth Haze apply their grade in a bounded color range and restore HDR brightness in
linear light. **HDR Vibrance** is designed specifically for Native HDR.

Imported SDR presets use Expanded HDR. For an imported effect designed for a linear HDR scene, add
`LongshotColorSpace=LinearHDR` to the global section of its INI. Native HDR uses RGBA16F scene intermediates and
allows values above 1.0. Imported Native HDR scene passes use linear samplers and output. The display and driver
must expose an HDR10 surface to Longshot for Native HDR to activate.

Effects run after the gameplay upscaler, before the HUD and before frame generation consumes the scene. The
available frame-generation modes follow Longshot's support for the platform, driver and active settings.
Menus and movies have no gameplay scene on which to run the effect.

The FX compiler rereads includes and definitions at each reload. An in-process cache reuses identical preprocessed
FX, and the Vulkan cache persists pipelines for the matching GPU and driver. Imported includes and textures resolve
within their respective folders. Paths containing a parent component or a drive name are refused.

Off performs no asset scan, shader compilation or extra GPU work. GPU resources from a previous active preset
are retired after their submissions complete.
