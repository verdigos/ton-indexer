# TON Indexer

Indexes the TON blockchain and derives higher-level data from it: actions, jetton and NFT state, and DEX prices.

## Language

### Assets

**TON**:
The native coin of the TON network. Wrapped forms that DEXes use inside pools (STON.fi pTON, DeDust native vault) are the same asset.
_Avoid_: Gram, GRAM, Toncoin, native token

**Jetton**:
A fungible token issued on TON through a jetton master contract.
_Avoid_: token, жетон

**Asset**:
Either TON or a Jetton.

### DEX prices

**DEX**:
A decentralized exchange protocol at a specific version, e.g. STON.fi v2 or DeDust v1.
_Avoid_: exchange, AMM

**Pair**:
An unordered pair of Assets, independent of any DEX. One Pair can have many Pools.
_Avoid_: market, pool

**Pool**:
A DEX contract that holds Reserves of one Pair and trades them along one Curve. Identified by its address.
_Avoid_: pair, liquidity pool contract

**Curve**:
The invariant a Pool trades along: _volatile_ (constant product) or _stable_.
_Avoid_: pool type, formula

**Reserves**:
The amounts of both Assets a Pool holds at a moment; together with the Curve they determine the Spot price.

**Spot price**:
The marginal price of one Asset in terms of the other, derived from a Pool's Reserves and Curve at a moment.
_Avoid_: rate, execution price

**Execution price**:
The ratio of amounts actually exchanged in one swap. Not used as a price; it includes fees, slippage and routing.

**Price**:
The Spot price of an Asset aggregated across its Pools, weighted by liquidity, expressed in a Quote asset.

**Quote asset**:
The unit a Price is expressed in. TON is the primary Quote asset; USD is derived from it.

**Anchor pools**:
The configured set of USDT/TON Pools whose Spot prices define the TON/USD Price, assuming 1 USDT = 1 USD.
_Avoid_: base pool, reference pool, hardcoded pool
