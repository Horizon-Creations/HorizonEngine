#pragma once

// Marks an entity as the audio listener (usually the main camera or player).
// Position/orientation is derived from the attached TransformComponent at runtime.
// Only one listener is active at a time; the first one found in the registry wins.
struct AudioListenerComponent {
    float masterVolume = 1.0f;
};
