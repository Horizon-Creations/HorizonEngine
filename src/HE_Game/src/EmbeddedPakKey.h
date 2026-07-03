#pragma once
#include <cstdint>

// Patchable pak-key block compiled into the game executable. The built binary
// carries hasKey = 0; when the project exports with encryption enabled, the
// ProjectExporter locates this block in the copied executable by its magic and
// patches hasKey + key in place (re-signing on macOS), so no key ships in
// project.hcfg and the game developer never handles key material.
//
// Same trust model as before: the key lives inside the shipped artifact either
// way — this is obfuscation against casual pak ripping, not a security boundary.
//
// LAYOUT IS ABI: the exporter patches by byte offset (magic 0..23, hasKey 24,
// pad 25..31, key 32..63). Changing it requires bumping the magic version.
struct EmbeddedPakKeyBlock
{
	char    magic[24];
	uint8_t hasKey;   // 0 in the built binary; 1 once patched by the exporter
	uint8_t pad[7];
	uint8_t key[32];
};
static_assert(sizeof(EmbeddedPakKeyBlock) == 64, "exporter patches by offset");

// Deliberately non-const: keeps the block in a writable data section so neither
// constant folding nor section merging can break the byte-pattern patching.
extern "C" EmbeddedPakKeyBlock g_hePakKeyBlock;
