# Fragment's telemint minter keys: the gifts collection (Eternal Roses) and
# usernames / numbers. A collection stores its key in the account data.
export fn is_fragment_authorised(key) = key in [
  "be3a1bc943496314139a36e2ab85f53fefb5e32c098fe6cef85094d6cdebc898",
  "71de902f194a53293a0aa982070ca71acdbda490056d0ee8e821cc088451cda9"
]

# Marketplace label for a telemint collection lookup record. An unresolvable
# collection leaves the venue unknown, which keeps "fragment"; only a known
# foreign key drops the label.
export fn fragment_marketplace(coll) = "fragment" if coll == null or is_fragment_authorised(coll.public_key) else null
