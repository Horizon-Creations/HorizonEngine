# Water follows the wind

2026-10-27, `release/0.7.0`. The Environment's wind (`windDirection` compass, `windSpeed` multiplier,
1 = calm; the weather presets drive both) now sets the sea.

* One definition: `HE_Core/include/Physics/WaterWaves.h` (`windWaveFactors`, `windTurnDeg`, `windTrain`).
  Direction: compass heading becomes the heading of train A (compass 0 = wave heading -90); B and C keep their
  angle to A. Strength: wavelength x wind, speed x sqrt(wind), steepness x (1 + 0.3 (wind - 1)); calm (1) with
  the wind along the material's own heading (compass 110) is the identity; storm 2.6 = ~3.4x taller.
* Material: `RenderExtractor.cpp` `windWaterParams` rewrites WaveA/B/C of `kEngineWaterMaterialId` into the
  object's param block each frame (an entity's own WaveA/B/C override wins). The wind is part of `FrameKey`.
* Physics: `WaterIndex::refresh` builds `m_waves` from the Environment; no Environment = old sea.
* Caustics: `causticsScale /= length`, `causticsSpeed *= speed`, `causticsWindTurn` (radians) rides in
  `sun.w` of the pass's constants in all five backends; the shaders turn S by it.

## Handoff
Verified: he_tests water/underwater green incl. new tests (windTrain, physics, caustics knobs); Metal shots
calm / storm / storm from another heading differ as expected (scratchpad `wind/`). Not verified: other backends'
caustic shaders at runtime, the net under a turned wind, GPU cost. Decided: exact heading of A = the wind.
Next: Environment panel tooltip for wind could mention water; companions/chop trains inside the material follow
via their knobs only.
