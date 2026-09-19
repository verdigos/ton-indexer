# Whether the address carries a detected TelemintContract interface.
export fn is_teleitem(addr) = lookup telemint_item(addr) != null

# The smaller of two values.
export fn min(a, b) = a if a < b else b

# Whether the transaction aborted, defaulting to false when missing.
export fn tx_failed(x) = tx_aborted(x) ?? false
