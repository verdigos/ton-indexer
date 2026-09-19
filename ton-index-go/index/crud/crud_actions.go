package crud

import (
	"context"
	"fmt"
	"log"
	"strings"

	"github.com/jackc/pgx/v5/pgxpool"
	"github.com/toncenter/ton-indexer/ton-index-go/index/models"
	"github.com/toncenter/ton-indexer/ton-index-go/index/parse"
)

func buildActionsQuery(req models.ActionRequest, settings models.RequestSettings) (string, []any, error) {
	utime_req := req.GetUtimeParams()
	lt_req := req.GetLtParams()
	lim_req := req.GetLimitParams()

	args := []any{}
	clmn_query_default := `A.trace_id, A.action_id, A.start_lt, A.end_lt, A.start_utime, A.end_utime, 
		A.trace_end_lt, A.trace_end_utime, A.trace_mc_seqno_end, A.source, A.source_secondary,
		A.destination, A.destination_secondary, A.asset, A.asset_secondary, A.asset2, A.asset2_secondary, A.opcode, A.tx_hashes,
		A.type, A.payload, A.value, A.amount, A.success, 2 as finality,
		A.trace_external_hash, A.trace_external_hash_norm, A.value_extra_currencies,
		A.ancestor_type, ARRAY[]::text[]`
	clmn_query := clmn_query_default
	from_query := `actions as A`
	filter_list := []string{}
	filter_query := ``
	orderby_query := ``
	limit_query, err := limitQuery(lim_req, settings)
	if err != nil {
		return "", nil, err
	}

	sort_order := "desc"
	if v := lim_req.Sort; v != nil {
		sort_order, err = getSortOrder(*v)
		if err != nil {
			return "", nil, err
		}
	}
	// time
	order_by_now := false
	join_accounts := false
	if v := req.AccountAddress; v != nil {
		join_accounts = true
	}
	if v := utime_req.StartUtime; v != nil {
		field := "A.trace_end_utime"
		if join_accounts {
			field = "AA.trace_end_utime"
		}
		filter_list = append(filter_list, fmt.Sprintf("%s >= %d", field, *v))
		order_by_now = true
	}
	if v := utime_req.EndUtime; v != nil {
		field := "A.trace_end_utime"
		if join_accounts {
			field = "AA.trace_end_utime"
		}
		filter_list = append(filter_list, fmt.Sprintf("%s <= %d", field, *v))
		order_by_now = true
	}
	if v := lt_req.StartLt; v != nil {
		field := "A.trace_end_lt"
		if join_accounts {
			field = "AA.trace_end_lt"
		}
		filter_list = append(filter_list, fmt.Sprintf("%s >= %d", field, *v))
	}
	if v := lt_req.EndLt; v != nil {
		field := "A.trace_end_lt"
		if join_accounts {
			field = "AA.trace_end_lt"
		}
		filter_list = append(filter_list, fmt.Sprintf("%s <= %d", field, *v))
	}
	if v := req.AccountAddress; v != nil {
		filter_str := fmt.Sprintf("AA.account = '%s'::tonaddr", v.FilterString())
		filter_list = append(filter_list, filter_str)

		from_query = `action_accounts as AA join actions as A on A.trace_id = AA.trace_id and A.action_id = AA.action_id and A.trace_mc_seqno_end = AA.trace_mc_seqno_end`
		if order_by_now {
			clmn_query = `distinct on (AA.trace_end_utime, AA.trace_id, AA.action_end_utime, AA.action_id) ` + clmn_query_default
		} else {
			clmn_query = `distinct on (AA.trace_end_lt, AA.trace_id, AA.action_end_lt, AA.action_id) ` + clmn_query_default
		}
	}
	if v := req.TransactionHash; v != nil {
		filter_str := filterByArray("T.hash", v)
		if len(filter_str) > 0 {
			filter_list = append(filter_list, filter_str)
		}
		from_query = `actions as A join transactions as T on A.trace_id = T.trace_id and A.tx_hashes @> array[T.hash::tonhash]`
		if order_by_now {
			clmn_query = `distinct on (A.trace_end_utime, A.trace_id, A.end_utime, A.action_id) ` + clmn_query_default
		} else {
			clmn_query = `distinct on (A.trace_end_lt, A.trace_id, A.end_lt, A.action_id) ` + clmn_query_default
		}
	}
	if v := req.MessageHash; len(v) > 0 {
		filter_str := fmt.Sprintf("(%s or %s)", filterByArray("M.msg_hash", v), filterByArray("M.msg_hash_norm", v))
		filter_list = append(filter_list, filter_str)

		from_query = `actions as A join messages as M on A.trace_id = M.trace_id and array[M.tx_hash::tonhash] <@ A.tx_hashes`
		if order_by_now {
			clmn_query = `distinct on (A.trace_end_utime, A.trace_id, A.end_utime, A.action_id) ` + clmn_query_default
		} else {
			clmn_query = `distinct on (A.trace_end_lt, A.trace_id, A.end_lt, A.action_id) ` + clmn_query_default
		}
	}
	if v := req.IncludeActionTypes; len(v) > 0 {
		args = append(args, v)
		filter_list = append(filter_list, fmt.Sprintf("A.type = ANY($%d)", len(args)))
	}
	if v := req.ExcludeActionTypes; len(v) > 0 {
		args = append(args, v)
		filter_list = append(filter_list, fmt.Sprintf("NOT (A.type = ANY($%d))", len(args)))
	}
	if v := req.McSeqno; v != nil {
		filter_list = append(filter_list, `E.state = 'complete'`)
		filter_list = append(filter_list, fmt.Sprintf("E.mc_seqno_end = %d", *v))
		from_query = `actions as A join traces as E on A.trace_id = E.trace_id`
		clmn_query = clmn_query_default
	}
	if v := req.ActionId; v != nil {
		from_query = `actions as A`
		filter_str := filterByArray("A.action_id", v)
		if len(filter_str) > 0 {
			filter_list = []string{filter_str}
		}
		clmn_query = clmn_query_default
	}
	if v := req.TraceId; v != nil {
		from_query = `actions as A`
		filter_str := filterByArray("A.trace_id", v)
		if len(filter_str) > 0 {
			filter_list = []string{filter_str}
		}
	}
	if strings.Contains(from_query, "action_accounts") {
		if order_by_now {
			orderby_query = fmt.Sprintf(" order by AA.trace_end_utime %s, AA.trace_id %s, AA.action_end_utime %s, AA.action_id %s",
				sort_order, sort_order, sort_order, sort_order)
		} else {
			orderby_query = fmt.Sprintf(" order by AA.trace_end_lt %s, AA.trace_id %s, AA.action_end_lt %s, AA.action_id %s",
				sort_order, sort_order, sort_order, sort_order)
		}
	} else {
		if order_by_now {
			orderby_query = fmt.Sprintf(" order by A.trace_end_utime %s, A.trace_id %s, A.end_utime %s, A.action_id %s",
				sort_order, sort_order, sort_order, sort_order)
		} else {
			orderby_query = fmt.Sprintf(" order by A.trace_end_lt %s, A.trace_id %s, A.end_lt %s, A.action_id %s",
				sort_order, sort_order, sort_order, sort_order)
		}
	}
	filter_list = append(filter_list, "A.end_lt is not NULL")
	// build query
	if len(filter_list) > 0 {
		filter_query = ` where ` + strings.Join(filter_list, " and ")
	}
	query := `select ` + clmn_query
	query += ` from ` + from_query
	query += filter_query
	query += orderby_query
	query += limit_query
	// log.Println(query)
	return query, args, nil
}

// queryActionsAccountsImpl attaches account lists to actions[idxs], querying conn
// (the DB that owns those actions' partitions). Assigns in place.
func queryActionsAccountsImpl(actions []models.Action, idxs []int, conn *pgxpool.Conn, settings models.RequestSettings) error {
	if len(idxs) == 0 {
		return nil
	}

	args := make([]any, 0, len(idxs)*2)
	placeholders := make([]string, len(idxs))
	for j, i := range idxs {
		// pgx uses 1-based parameter numbers.
		// Every action consumes two params: trace_id then action_id.
		p1, p2 := j*2+1, j*2+2
		placeholders[j] = fmt.Sprintf("($%d,$%d)", p1, p2)

		args = append(args, actions[i].TraceId, actions[i].ActionId)
	}

	query := fmt.Sprintf(
		`SELECT trace_id, action_id, account
		   FROM action_accounts
		  WHERE (trace_id, action_id) IN (%s)`,
		strings.Join(placeholders, ","),
	)

	ctx, cancel := requestContext(settings)
	defer cancel()
	rows, err := conn.Query(ctx, query, args...)
	if err != nil {
		return models.IndexError{Code: 500, Message: err.Error()}
	}
	defer rows.Close()

	type key struct{ trace, action string }
	idx := make(map[key]*models.Action, len(idxs))
	for _, i := range idxs {
		k := key{string(*actions[i].TraceId), string(actions[i].ActionId)}
		idx[k] = &actions[i]
	}

	for rows.Next() {
		var traceID, actionID string
		var account models.AccountAddress

		if err := rows.Scan(&traceID, &actionID, &account); err != nil {
			return models.IndexError{Code: 500, Message: err.Error()}
		}
		if a := idx[key{traceID, actionID}]; a != nil {
			a.Accounts = append(a.Accounts, account)
		}
	}
	if rows.Err() != nil {
		return models.IndexError{Code: 500, Message: rows.Err().Error()}
	}

	return nil
}

func queryRawActionsImpl(query string, conn *pgxpool.Conn, settings models.RequestSettings, args ...any) ([]models.RawAction, error) {
	ctx, cancel_ctx := context.WithTimeout(context.Background(), settings.Timeout)
	defer cancel_ctx()
	rows, err := conn.Query(ctx, query, args...)
	if err != nil {
		return nil, models.IndexError{Code: 500, Message: err.Error()}
	}
	// select {
	// case <-ctx.Done():
	// 	return nil, fmt.Errorf("query timeout %v", settings.Timeout)
	// default:
	// }
	defer rows.Close()

	res := []models.RawAction{}
	for rows.Next() {
		if loc, err := parse.ScanRawAction(rows); err == nil {
			res = append(res, *loc)
		} else {
			return nil, models.IndexError{Code: 500, Message: err.Error()}
		}
	}
	if rows.Err() != nil {
		return nil, models.IndexError{Code: 500, Message: rows.Err().Error()}
	}
	return res, nil
}

func (db *DbClient) QueryActions(
	req models.ActionRequest,
	settings models.RequestSettings,
) ([]models.Action, models.AddressBook, models.Metadata, error) {
	query, args, err := buildActionsQuery(req, settings)
	if settings.DebugRequest {
		log.Println("Debug query:", query)
	}
	if err != nil {
		return nil, nil, nil, models.IndexError{Code: 500, Message: err.Error()}
	}

	// read data
	conn, releaseConn, err := acquireConnForRequest(db.Pool, settings)
	if err != nil {
		return nil, nil, nil, models.IndexError{Code: 500, Message: err.Error()}
	}
	defer releaseConn()

	// check block
	if seqno := req.McSeqno; seqno != nil {
		exists, err := queryBlockExists(*seqno, conn, settings)
		if err != nil {
			return nil, nil, nil, err
		}
		if !exists {
			return nil, nil, nil, models.IndexError{Code: 404, Message: fmt.Sprintf("masterchain block %d not found", *seqno)}
		}
	}

	raw_actions, err := queryRawActionsImpl(query, conn, settings, args...)
	if err != nil {
		return nil, nil, nil, models.IndexError{Code: 500, Message: err.Error()}
	}
	actions := []models.Action{}
	book := models.AddressBook{}
	metadata := models.Metadata{}
	addr_map := map[models.AccountAddress]bool{}
	for idx := range raw_actions {
		parse.CollectAddressesFromAction(&addr_map, &raw_actions[idx])
		action, err := parse.ParseRawAction(&raw_actions[idx])
		if err != nil {
			return nil, nil, nil, models.IndexError{Code: 500, Message: err.Error()}
		}
		actions = append(actions, *action)
	}
	if len(addr_map) > 0 {
		addr_list := []models.AccountAddress{}
		for k := range addr_map {
			addr_list = append(addr_list, k)
		}
		if db.Kvrocks != nil {
			releaseConn()
			book, metadata, err = db.queryKvrocksEnrichment(addr_list, settings)
			if err != nil {
				return nil, nil, nil, models.IndexError{Code: 500, Message: err.Error()}
			}
		} else {
			if !settings.NoAddressBook {
				book, err = QueryAddressBookImpl(addr_list, conn, settings)
				if err != nil {
					return nil, nil, nil, models.IndexError{Code: 500, Message: err.Error()}
				}
			}
			if !settings.NoMetadata {
				metadata, err = QueryMetadataImpl(addr_list, conn, settings)
				if err != nil {
					return nil, nil, nil, models.IndexError{Code: 500, Message: err.Error()}
				}
			}
		}
	}
	return actions, book, metadata, nil
}
