# Runtime configuration

Production-facing runtime configuration uses a complete set of canonical `key=value` entries.
Parsing rejects missing, duplicate, unknown, malformed, signed unsigned values, leading zeros, overflow, embedded null path bytes, trailing path separators, and unsafe cross-field relationships before resource construction.

Required fields:

- `minimum-price`: signed integer minimum price tick.
- `tick-count`: positive price-level count, at most 1,000,000, without signed price overflow.
- `max-orders`: positive fixed order capacity, at most 1,000,000.
- `max-quantity`: positive quantity fitting `uint32_t`.
- `command-queue-capacity`: positive capacity, at most 1,000,000.
- `event-queue-capacity`: at least `2 * max-orders + 1` and at most 1,000,000.
- `journal-segment-capacity`: positive record capacity, at most 1,000,000.
- `journal-prefix`: nonempty segment prefix without an embedded null, trailing separator, `.` or `..`.
- `snapshot-path`: nonempty snapshot path under the same path-shape restrictions.
- `max-lanes`: positive and no greater than command queue capacity.
- `max-notional`: positive integer notional limit.
- `max-orders-per-second`: positive per-lane logical-time rate limit.

The parsed configuration derives the gateway price collar from `minimum-price` and `tick-count`.
It does not read environment variables, apply defaults, or silently clamp values.
Secrets, credentials, personal data, authentication, and authorization are not configuration fields.

The runtime API starts at version 1.0.
Compatibility requires an equal major version and a requested minor version no newer than the library minor version.
Protocol, journal, and snapshot format versions remain independent of this API version.
