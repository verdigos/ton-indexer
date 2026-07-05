# Coffee liquidity/vault matchers — ported from blocks/liquidity.py.
# Currently: CoffeeCreateVaultMatcher.
#
# A vault creation request deploys the vault, which then discovers its jetton
# wallet via the TEP-89 pair (provide_wallet_address / take_wallet_address —
# the raw 0x2c76b973 / 0xd1735400 literals in the legacy matcher). Both
# discovery hops are optional (TON/extra-currency vaults skip them).
#
# Fully declarative build (M4 §9): sender/amount from the anchor envelope, asset
# from its parsed body, vault from the captured CoffeeDeploy call's envelope.
# The produced block is the canonical generic block (btype + data map).

opcode CoffeeCreateVault    = 0xc0ffee06
opcode CoffeeDeploy         = 0xc0ffee22
opcode ProvideWalletAddress = 0x2c76b973
opcode TakeWalletAddress    = 0xd1735400

matcher coffee_create_vault {
    produces CoffeeCreateVaultBlock
    entry    @create

    op CoffeeCreateVault @create
      -> op CoffeeDeploy @deploy
      -> maybe op ProvideWalletAddress
      -> maybe op TakeWalletAddress

    parse create as CoffeeCreateVault
    out {
        sender: account(create.msg.source)
        vault:  account(deploy.msg.destination)
        asset:  create.body.asset
        amount: amount(create.msg.value)
    }
}
