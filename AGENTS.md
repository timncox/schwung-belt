---
status: active
last_touched: 2026-07-20
deploy: scripts/deploy.sh
---

# Belt

Schwung Shadow UI vocal processor for Ableton Move. It ships as the `belt`
audio effect and `belt-in` input generator, both backed by
`src/belt_core.c`'s YIN and TD-PSOLA engine.

## Work safely

- Run `make test` after DSP, parameter, state, or UI changes.
- Run the simulator under AddressSanitizer and UBSan before release.
- Keep pitch analysis and rendering non-allocating in the audio callback.
- Treat chain, Master FX, and input-generator parameter behavior as one API.
- `make arm` stages and cross-compiles both archives with Docker.
- One tag publishes both archives. Keep the top-level `release.json` fields as
  the Custom GitHub fallback for `belt`, and keep its `modules` entries aligned
  with the `belt` and `belt-in` catalog IDs.
- `scripts/deploy.sh` writes to Move hardware. Do not run it without explicit
  deployment authorization.

See `CLAUDE.md` for the DSP architecture, parameter model, latency contract,
and on-device verification checklist.
