# This file holds helpers shared by the matcher specs.
export fn is_teleitem(addr) = lookup telemint_item(addr) != null

export fn min(a, b) = a if a < b else b

export fn tx_failed(x) = tx_aborted(x) ?? false
