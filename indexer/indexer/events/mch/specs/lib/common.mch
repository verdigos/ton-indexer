# Whether the item belongs to a Fragment collection: Telegram Usernames,
# Anonymous Telegram Numbers, or Eternal Roses.
export fn is_teleitem(n) = n != null and n.collection_address in [
  "0:80D78A35F955A14B679FAA887FF4CD5BFC0F43B4A4EEA2A7E6927F3701B273C2",
  "0:0E41DC1DC3C9067ED24248580E12B3359818D83DEE0304FABCF80845EAFAFDB2",
  "0:EE9B2DDB9D96335786D258C492664997AB1E745EE1F977D4655FFC848B614D96"
]

# The smaller of two values.
export fn min(a, b) = a if a < b else b

# Whether the transaction aborted, defaulting to false when missing.
export fn tx_failed(x) = tx_aborted(x) ?? false
