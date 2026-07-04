# Vesting send-message matcher — ported from blocks/vesting.py
# VestingSendMessageBlockMatcher. Linear two-node chain (M1 shape): the
# owner->vesting request plus its required response callback. The response is
# consumed (legacy child_matcher optional=False) but only the request feeds the
# build. VestingAddWhiteListBlockMatcher stays hand-written (out of scope).

opcode VestingSendMessage         = 0xA7733ACD
opcode VestingSendMessageResponse = 0xF7733ACD

matcher vesting_send_message {
    produces VestingSendMessageBlock
    build    build_vesting_send_message
    entry    @send

    op VestingSendMessage @send -> op VestingSendMessageResponse @response
}
