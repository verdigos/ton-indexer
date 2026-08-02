# Message parsing

The parser registry combines hand-written parsers for irregular message shapes
with generated ABI parsers. Duplicate names are poisoned in the merged map and
reported by the startup consistency check, so dispatch fails closed instead of
choosing a parser arbitrarily.

Hand-written parsers preserve the existing message-value conventions. They skip
the opcode without validating it, turn `addr_none` into null, represent external
addresses as strings, serialize optional cell payloads as CRC32C BOCs, and keep
conditionally assigned attributes conditionally present. Any underflow, missing
reference, or invalid address fails the whole parse so the caller can try the
next soft-parse alternative.

The underlying slice adapter intentionally preserves several observable parser
behaviors. Copying a slice resets reference consumption. Anycast bits are
consumed without rewriting the address prefix. A zero-length coin value is zero,
and payload sum-type hexadecimal strings are lowercase without zero padding.

Generated ABI rows validate their authored prefixes except for explicitly
prefixless payloads. A consumer must therefore guard a parse by the relevant
opcode or tolerate a null result in the surrounding lazy expression.
