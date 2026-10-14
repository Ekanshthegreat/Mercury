#pragma once
#include <filesystem>

#include "engine/execution/execution.hpp"
namespace mercury {
// Canonical text with an accidental-corruption checksum, not an authentication signature.
void write_tape(const MarketTape& tape, const std::filesystem::path& path);
MarketTape read_tape(const std::filesystem::path& path);
}  // namespace mercury
