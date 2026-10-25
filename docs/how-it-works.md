# How it works

## Where FSR 4 hooks in

BeamNG's `fsrCustom` upscaler records one block of compute passes per frame, all with their resources in
descriptor set 0. The layer recognises the passes by their shader debug names
(`shaders/common/postFx/upscale/…`) and an FNV-1a hash of their SPIR-V:

| Pass                                                          | While FSR 4 is active                           |
|---------------------------------------------------------------|-------------------------------------------------|
| `dynamicDisocclusion` (the engine's reactive mask)            | skipped                                         |
| `fsr3/fsr3Clear` ×8, `LumaPyramid`, `ShadingChange*`, `PrepareReactivity`, `LumaInstability` | skipped |
| `upscalerExposure`                                            | runs                                            |
| `fsr3/fsr3PrepareInputs`                                      | inputs captured, dispatch skipped               |
| `fsr3/fsr3Accumulate`                                         | constant buffer copied back, dispatch skipped   |
| `fsr3/fsr3Rcas`                                               | replaced by FSR 4.1.1, writing the same image   |

Passes are only skipped when the previous frame was rendered by FSR 4. Nothing outside `upscale/fsr3/`
reads FSR 3's intermediates.

The inputs FSR 4 reads from the bound descriptors:

- color: `r_input_color_jittered` (PrepareInputs binding 49), RGBA16F, render size
- depth: `r_input_depth` (48), D32S8, reversed Z
- motion: `r_input_motion_vectors` (47), RG16F in UV units, so `motion_scale` is the render size
- output: `rw_upscaled_output` (RCAS binding 31), RGBA16F, display size, layout `GENERAL`

## Device setup

At `vkCreateDevice` the layer adds `shaderInt8`, `shaderIntegerDotProduct`, `subgroupSizeControl` (when
the GPU allows wave32 and wave64 for compute) and `VK_VALVE_shader_mixed_float_dot_product` (when
available) to the game's request. If the game's request lacks Vulkan 1.2/1.3 feature structs, push
descriptors or compute shader derivatives, or if the driver rejects the modified request, the device is
created exactly as the game asked and the layer only passes calls through.

The FSR 4 backend (`fsr411/`) records into the game's command buffer through the next layer's dispatch
table, so its own calls don't go back through these hooks. Afterwards the layer re-binds the game's RCAS
pipeline and descriptor set, so the original dispatch still works as a fallback.

## Jitter

FSR 4 needs the current frame's sub-pixel jitter while the commands are being recorded, but BeamNG only
writes it to a GPU constant buffer. The layer therefore copies that buffer back every frame. The copy
lands a few frames late. From it the layer learns the jitter table (`jitterSeqLen` entries, Halton 2,3)
and the offset between the game's `frameIdx` and its own frame counter, and uses them to predict the
current frame's jitter. Every prediction is later checked against the copy that comes back, and
mismatches are logged.

A change in render size, output size or sequence length, or a `frameIdx` reset, invalidates the table.
FSR 3 renders until the table has been relearned, which takes 17–32 frames. If the FSR 3 passes for that
frame were already skipped, FSR 4 renders it with a history reset instead, since FSR 3 has no result to
show.

Sharpness follows the game's slider. It is read back the same way from RCAS's constant buffer, where
`x = exp2(-(2 − 2·sharpness))`.

## Verification (upstream, RX 6700 XT)

- AMD's signed `amd_fidelityfx_upscaler_dx12.dll` (FidelityFX SDK 2.3.0, provider 4.1.1), run under
  vkd3d-proton, and the Vulkan port gave byte-identical output in all 7 test cases, for both models and up to
  3840×2160.
- All 29 SPIR-V programs created on BeamNG's device were byte-identical to the asset files. The model
  initializer read back from GPU memory matched `initializer.bin`.
- Replaying captured live frames through the port offline reproduced them byte for byte: 33 of 33 frames,
  and 53 of 53 with the FSR 3 passes skipped.
