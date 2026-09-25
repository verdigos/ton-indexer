# This file holds helpers shared by the matcher specs.
export fn is_teleitem(addr) = lookup telemint_item(addr) != null

export fn min(a, b) = a if a < b else b

export fn tx_failed(x) = tx_aborted(x) ?? false

# The block failure flag or an aborted transaction; tx_failed alone reads only the abort.
export fn any_failed(x) = x.failed or tx_failed(x)
