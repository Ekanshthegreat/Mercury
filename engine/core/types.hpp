// Mercury — core value types shared across the engine.
//
// Design notes
// ------------
// We use strong-ish integer types for prices and quantities because mixing them
// up is the single easiest way to silently corrupt a matching engine. Prices are
// integer *ticks* (never floating point) so that price arithmetic is exact and
// order-book invariants (best_bid < best_ask) are not subject to rounding.
//
// Timestamps are signed 64-bit **nanoseconds since the start of the simulation**.
// The simulator's clock is virtual (discrete-event); we never read wall time in
// the matching path. This is what makes replay deterministic.

#pragma once

#include <cstdint>
#include <limits>
#include <string_view>

namespace mercury {

/// Price expressed in integer ticks of a fixed tick size.
/// The mapping tick -> currency is a calibration/ reporting concern, kept out of
/// the matching engine.
using Price = std::int64_t;

/// Share quantity. Strictly non-negative everywhere it is meaningful.
using Quantity = std::int64_t;

/// Virtual time in nanoseconds since simulation start. Monotonic non-decreasing
/// in the event stream.
using Timestamp = std::int64_t;

/// Unique identifier for a resting or incoming order.
using OrderId = std::uint64_t;

/// Identifier for market participants / strategy tags (used for accounting only,
/// never consulted by the matching engine).
using ParticipantId = std::uint32_t;

inline constexpr Price kInvalidPrice = std::numeric_limits<Price>::min();
inline constexpr Price kNoPrice = std::numeric_limits<Price>::max();
inline constexpr OrderId kInvalidOrderId = 0;

enum class Side : std::uint8_t { Buy = 0, Sell = 1 };

[[nodiscard]] constexpr Side opposite(Side s) noexcept {
    return s == Side::Buy ? Side::Sell : Side::Buy;
}

[[nodiscard]] constexpr std::string_view to_string(Side s) noexcept {
    return s == Side::Buy ? "Buy" : "Sell";
}

/// Order type as seen by the matching engine.
enum class OrderType : std::uint8_t {
    Limit,   ///< rests if not immediately marketable
    Market,  ///< matches against available liquidity, never rests
};

/// Reason an order stopped resting in the book.
enum class OrderStatus : std::uint8_t {
    New,        ///< acknowledged, resting
    PartiallyFilled,
    Filled,     ///< completely executed
    Cancelled,  ///< cancelled by participant
    Rejected,   ///< failed validation (e.g. bad quantity)
};

/// Time-in-force. IOC = immediate-or-cancel, FOK = fill-or-kill.
enum class TimeInForce : std::uint8_t {
    GTC,  ///< good-till-cancel (rests)
    IOC,  ///< immediate-or-cancel (no resting remainder; remainder is cancelled)
    FOK,  ///< fill-or-kill (all-or-nothing)
};

}  // namespace mercury
