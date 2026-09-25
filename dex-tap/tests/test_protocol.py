from dex_tap.protocol import GapCounter, TapEvent, encode_event, parse_event


def event(session=1, seq=1):
    return TapEvent(session=session, seq=seq, mc_seqno=10, utime=1790000000, account="0:AB", last_trans_lt=5,
                    balance=123, code_hash="C0", data_hash="D0", code="Y29kZQ==", data="ZGF0YQ==", tx_count=2)


def test_worker_line_parses():
    # the exact line DexTapTest.cpp pins for the worker
    line = (b'{"v":1,"session":1,"seq":2,"mc_seqno":3,"utime":4,"account":"0:AB","last_trans_lt":5,'
            b'"balance":"6","code_hash":"C0","data_hash":"D0","code":"Y29kZQ==","data":"ZGF0YQ==","tx_count":7}')
    e = parse_event(line)
    assert (e.session, e.seq, e.mc_seqno, e.utime, e.balance, e.tx_count) == (1, 2, 3, 4, 6, 7)


def test_encode_roundtrip():
    assert parse_event(encode_event(event())) == event()


def test_gap_counter_counts_losses_within_a_session_only():
    gaps = GapCounter()
    for seq in (100, 101, 104, 105):  # listener joins at 100; 102 and 103 are lost
        gaps.observe(event(seq=seq))
    gaps.observe(event(session=2, seq=1))  # worker restarted
    gaps.observe(event(session=2, seq=2))
    assert (gaps.lost, gaps.sessions) == (2, 2)
