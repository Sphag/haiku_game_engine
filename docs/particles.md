# GPU particles (US-051)

GPU particles run emission, simulation, and drawing on the device. The CPU keeps only the spawn accumulator and the parameters of each emitter.

## Pieces

| Piece | Where | Role |
|---|---|---|
| `render::ParticleSystem` | `engine/render/include/engine/render/particles.hpp`, `engine/render/src/particles.cpp` | Emitters, the budget, the per-frame records (`prepare`, `simulate`, `draw`), and the lifecycle |
| `EmitterDesc`, `ParticleCurve` | `particles.hpp` | The emitter parameters and the colour and size curves (up to 8 keys) |
| `validate_emitter`, `validate_curve` | `particles.cpp` | The checks that run before any resource exists |
| `advance_spawn`, `evaluate_curve`, `pack_particle_parameters` | `particles.cpp` | The CPU spawn accumulator, the CPU curve (the reference of the GPU curve), and the parameter block |
| `particles.hlsl` | `engine/render/shaders/particles.hlsl` | `CSParticleSimulate`, `CSParticleArgs`, `VSParticle`, `PSParticle`; the parameter layout is in its header |

## Frame

1. `prepare(frame)` advances each unpaused emitter's spawn accumulator by `spawn_rate * dt` and uploads its parameter block. A frame's spawns are clamped to the emitter's capacity.
2. `simulate(list)` records, per prepared emitter, the following passes:
   - the live counter is zeroed from a zero source;
   - one thread per ring slot spawns the slots in this frame's range, or integrates a live slot (gravity, lifetime, bounds), and appends each live slot to the draw list through an atomic counter;
   - one thread writes the indirect record (six indices, the live count).
3. `draw(list, targets)` records one render pass that draws every simulated emitter with `DrawIndexedIndirect`. Each quad is a camera-facing billboard that samples the caller's texture, is tinted by the colour curve, and is scaled by the size curve. The pipelines test against the scene depth and do not write it.

## Rules

- **Budget.** The capacity of an emitter is its live-particle budget and the size of its ring. The system holds at most `ParticleBudget::max_particles` slots over all emitters, and `max_emitters` emitters. An addition that exceeds either is refused with `OutOfRange`.
- **Overflow.** A frame that asks for more particles than the capacity spawns the capacity and counts the rest as `dropped`. The ring overwrites the oldest slots, so the live count never exceeds the capacity.
- **Reproducibility.** Every spawn draws its random values from a hash of (seed, spawn serial). One backend reproduces the same particle bytes from the same inputs; the GPU test checks this on two devices. Different backends may differ in the last bit of a float, so the test compares within one backend.
- **Pause** stops spawning and integration. The emitter keeps drawing its last state. **Reset** kills every particle on the next `simulate` and restarts the spawn serial and the accumulator. **Removal** destroys the emitter's five buffers and three descriptor sets.
- **Blend.** `ParticleBlend::Alpha` blends by source alpha. `ParticleBlend::Additive` adds the premultiplied colour (`PipelineDesc::additive_blend`), and it keeps the target's alpha.
- **Texture.** The billboards read the texture through `set_texture`. It must be in ShaderRead when `draw` records. The texture is read with `Load`, because the RHI has no sampler descriptor.

## Limits

- The reference device cannot host the system: the counter zeroing and the index buffer are uploads, and the CPU reference device keeps no buffer contents. The lifecycle and budget are tested on the GPU (`render_particles_gpu_tests.cpp`).
- Particles do not collide, sort, or cast shadows. The draw is a single pass with no sorting, so overlapping alpha billboards are blended in ring order.
- `engine_bench --suite acceptance` has no `render_stress` or `all_features` workload yet (US-074, US-076); the particle counts in the PRD's benchmark table are not measured here.
