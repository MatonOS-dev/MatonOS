<!-- SPDX-License-Identifier: Apache-2.0 -->
# Output boost migration

The temporary global `DynamicsProcessing` effect used in build-76 has been
removed. PipeWire's AIDL HAL now owns output gain per discovered sink, with a
0.5×–1.5× multiplier defaulting to 1×. MatonOS Settings stores a multiplier
for each stable PipeWire device identity through `IChannel/audio`; the HAL
persists the values under `/data/vendor/maton-audio/device-boosts.conf` and
applies them to the active PipeWire stream. No limiter is applied.

See [audio/README.md](../audio/README.md) for the identity key, IPC and
verification steps. Fresh-image runtime verification is coordinator-owned.
