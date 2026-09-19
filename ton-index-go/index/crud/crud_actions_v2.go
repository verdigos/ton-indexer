package crud

import (
	"context"
	"fmt"
	"log"
	"strings"

	"github.com/toncenter/ton-indexer/ton-index-go/index/models"
	"github.com/toncenter/ton-indexer/ton-index-go/index/parse"

	"github.com/jackc/pgx/v5/pgxpool"
)

func (db *DbClient) QueryActionsV2(
	req models.ActionRequest,
	settings models.RequestSettings,
) ([]models.Action, models.AddressBook, models.Metadata, error) {
	if len(req.SupportedActionTypes) == 0 {
		req.SupportedActionTypes = []string{"latest"}
	}
	req.SupportedActionTypes = models.ExpandActionTypeShortcuts(req.SupportedActionTypes)

	lim_req := req.GetLimitParams()
	sortOrder := "desc"
	if v := lim_req.Sort; v != nil {
		var serr error
		sortOrder, serr = getSortOrder(*v)
		if serr != nil {
			return nil, nil, nil, serr
		}
	}
	offset := 0
	if lim_req.Offset != nil {
		offset = int(max(0, *lim_req.Offset))
	}
	limit := int32(settings.DefaultLimit)
	if lim_req.Limit != nil {
		limit = max(1, *lim_req.Limit)
		if limit > int32(settings.MaxLimit) {
			return nil, nil, nil, models.IndexError{Code: 422, Message: fmt.Sprintf("limit is not allowed: %d > %d", limit, settings.MaxLimit)}
		}
	}

	parts := actionsQueryPartsV2(req, sortOrder)

	fc, release, err := db.acquireFedForRequest(settings)
	if err != nil {
		return nil, nil, nil, models.IndexError{Code: 500, Message: err.Error()}
	}
	defer release()

	fetch := func(query string, conn *pgxpool.Conn) ([]models.RawAction, error) {
		if settings.DebugRequest {
			log.Println("Debug query:", query, "Args:", parts.args)
		}
		return queryRawActionsImplV2(query, parts.args, conn, settings)
	}

	var raw_actions []models.RawAction
	// Router-served pages enrich from the same DB that served the page.
	routed := false
	servedCold := false
	if req.McSeqno != nil {
		// a masterchain round's actions live wholly in one partition
		conn, cerr := fc.connForSeqno(uint64(*req.McSeqno))
		if cerr != nil {
			return nil, nil, nil, models.IndexError{Code: 500, Message: cerr.Error()}
		}
		exists, eerr := queryBlockExists(*req.McSeqno, conn, settings)
		if eerr != nil {
			return nil, nil, nil, eerr
		}
		if !exists {
			return nil, nil, nil, models.IndexError{Code: 404, Message: fmt.Sprintf("masterchain block %d not found", *req.McSeqno)}
		}
		raw_actions, err = fetch(buildActionsOffsetQueryV2(parts, offset, int(limit)), conn)
	} else {
		routed = true
		raw_actions, servedCold, err = queryActionsRouted(fc, req, parts, sortOrder, offset, int(limit), fetch)
	}
	if err != nil {
		return nil, nil, nil, models.IndexError{Code: 500, Message: err.Error()}
	}

	actions := []models.Action{}
	book := models.AddressBook{}
	metadata := models.Metadata{}
	addr_map := map[models.AccountAddress]bool{}
	for idx := range raw_actions {
		parse.CollectAddressesFromAction(&addr_map, &raw_actions[idx])
		action, perr := parse.ParseRawAction(&raw_actions[idx])
		if perr != nil {
			return nil, nil, nil, models.IndexError{Code: 500, Message: perr.Error()}
		}
		actions = append(actions, *action)
	}
	if req.IncludeAccounts != nil && *req.IncludeAccounts {
		var eerr error
		if routed {
			eerr = enrichActionsAccountsRouted(fc, actions, servedCold, settings)
		} else {
			eerr = enrichActionsAccounts(fc, actions, settings)
		}
		if eerr != nil {
			return nil, nil, nil, models.IndexError{Code: 500, Message: eerr.Error()}
		}
	}
	if req.IncludeTransactions != nil && *req.IncludeTransactions {
		actions, err = queryActionsTransactionsImpl(fc, release, actions, settings, db.Kvrocks)
		if err != nil {
			return nil, nil, nil, models.IndexError{Code: 500, Message: err.Error()}
		}
	}
	if len(addr_map) > 0 {
		addr_list := []models.AccountAddress{}
		for k := range addr_map {
			addr_list = append(addr_list, k)
		}
		if db.Kvrocks != nil {
			release()
			book, metadata, err = db.queryKvrocksEnrichment(addr_list, settings)
			if err != nil {
				return nil, nil, nil, models.IndexError{Code: 500, Message: err.Error()}
			}
		} else if !settings.NoAddressBook || !settings.NoMetadata {
			coldConn, cerr := fc.cold()
			if cerr != nil {
				return nil, nil, nil, models.IndexError{Code: 500, Message: cerr.Error()}
			}
			if !settings.NoAddressBook {
				book, err = QueryAddressBookImpl(addr_list, coldConn, settings)
				if err != nil {
					return nil, nil, nil, models.IndexError{Code: 500, Message: err.Error()}
				}
			}
			if !settings.NoMetadata {
				metadata, err = QueryMetadataImpl(addr_list, coldConn, settings)
				if err != nil {
					return nil, nil, nil, models.IndexError{Code: 500, Message: err.Error()}
				}
			}
		}
	}
	return actions, book, metadata, nil
}

// enrichActionsAccounts batches account enrichment by the action's owning DB.
func enrichActionsAccounts(fc *fedConns, actions []models.Action, settings models.RequestSettings) error {
	if len(actions) == 0 {
		return nil
	}
	hotIdx, coldIdx := []int{}, []int{}
	for i := range actions {
		if fc.coldForSeqno(uint64(actions[i].TraceMcSeqnoEnd)) {
			coldIdx = append(coldIdx, i)
		} else {
			hotIdx = append(hotIdx, i)
		}
	}
	if len(hotIdx) > 0 {
		conn, err := fc.hot()
		if err != nil {
			return err
		}
		if err := queryActionsAccountsImpl(actions, hotIdx, conn, settings); err != nil {
			return err
		}
	}
	if len(coldIdx) > 0 {
		conn, err := fc.cold()
		if err != nil {
			return err
		}
		if err := queryActionsAccountsImpl(actions, coldIdx, conn, settings); err != nil {
			return err
		}
	}
	return nil
}

// enrichActionsAccountsRouted uses the one DB that served the routed page.
func enrichActionsAccountsRouted(fc *fedConns, actions []models.Action, servedCold bool, settings models.RequestSettings) error {
	if len(actions) == 0 {
		return nil
	}
	idxs := make([]int, len(actions))
	for i := range actions {
		idxs[i] = i
	}
	conn, err := fc.conn(servedCold)
	if err != nil {
		return err
	}
	return queryActionsAccountsImpl(actions, idxs, conn, settings)
}

// actionOrderKey returns the row's router sort key; action keys are non-null.
func actionOrderKey(orderByNow bool) func(*models.RawAction) *uint64 {
	if orderByNow {
		return func(a *models.RawAction) *uint64 {
			v := uint64(a.TraceEndUtime)
			return &v
		}
	}
	return func(a *models.RawAction) *uint64 {
		v := uint64(a.TraceEndLt)
		return &v
	}
}

// queryActionsRouted returns one router-served page plus the side it came from.
func queryActionsRouted(
	fc *fedConns,
	req models.ActionRequest,
	parts actionsQueryParts,
	sortOrder string,
	offset, limit int,
	fetch func(query string, conn *pgxpool.Conn) ([]models.RawAction, error),
) ([]models.RawAction, bool, error) {
	query := buildActionsOffsetQueryV2(parts, offset, limit)

	// Match the sort axis selected by actionsQueryPartsV2.
	orderByNow := parts.orderKey == "utime"

	// Single pool: read cold directly, no classification.
	dec := routeCold
	var floor uint64
	if fc.federated {
		w := routeWindow{
			startLt:    req.StartLt,
			endLt:      req.EndLt,
			startUtime: (*uint64)(req.StartUtime),
			endUtime:   (*uint64)(req.EndUtime),
			orderByNow: orderByNow,
			sortDesc:   sortOrder == "desc",
		}
		dec = classifyRoute(w, fc.split, fc.utimeMargin)

		// Verify against the same floor classifyRoute used.
		floor = fc.split.Lt
		if orderByNow {
			floor = fc.split.Utime + fc.utimeMargin
		}
	}

	return routedPage(fc, dec,
		func(conn *pgxpool.Conn) ([]models.RawAction, error) { return fetch(query, conn) },
		actionOrderKey(orderByNow), limit, floor)
}

func actionsQueryPartsV2(req models.ActionRequest, sort_order string) actionsQueryParts {
	utime_req := req.GetUtimeParams()
	lt_req := req.GetLtParams()

	clmn_query_default := `A.trace_id, A.action_id, A.start_lt, A.end_lt, A.start_utime, A.end_utime, 
		A.trace_end_lt, A.trace_end_utime, A.trace_mc_seqno_end, A.source, A.source_secondary,
		A.destination, A.destination_secondary, A.asset, A.asset_secondary, A.asset2, A.asset2_secondary, A.opcode, A.tx_hashes,
		A.type, A.payload, A.value, A.amount, A.success, 2 as finality,
		A.trace_external_hash, A.trace_external_hash_norm, A.value_extra_currencies,
		A.ancestor_type, ARRAY[]::text[]`
	clmn_query := clmn_query_default
	from_query := `actions as A`
	filter_list := []string{}
	orderby_query := ``
	args := []any{req.SupportedActionTypes}
	// time
	order_by_now := false
	join_accounts := false
	if v := req.AccountAddress; v != nil && len(*v) > 0 {
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
	if v := req.AccountAddress; join_accounts && v != nil {
		filter_str := fmt.Sprintf("AA.account = '%s'::tonaddr", v.FilterString())
		filter_list = append(filter_list, filter_str)

		action_accounts_from := `action_accounts as AA`
		if trace_filter := filterByArray("trace_id", req.TraceId); len(trace_filter) > 0 {
			// Limit action_accounts before the lateral join for account+trace_id.
			action_accounts_from = fmt.Sprintf("(select * from action_accounts where %s) as AA", trace_filter)
		}
		from_query = action_accounts_from + ` join lateral (
				select *
				from actions as A
				where A.trace_id = AA.trace_id
					and A.action_id = AA.action_id
					and A.trace_mc_seqno_end = AA.trace_mc_seqno_end
				limit 1
			) as A on true`
		if order_by_now {
			clmn_query = `distinct on (AA.account, AA.trace_end_utime, AA.trace_id, AA.action_end_utime, AA.action_id) ` + clmn_query_default
		} else {
			clmn_query = `distinct on (AA.account, AA.trace_end_lt, AA.trace_id, AA.action_end_lt, AA.action_id) ` + clmn_query_default
		}
	}
	if v := req.TransactionHash; v != nil {
		filter_str := filterByArray("T.hash", v)
		if len(filter_str) > 0 {
			filter_list = append(filter_list, filter_str)
			from_query += ` join transactions as T on A.trace_id = T.trace_id and A.tx_hashes @> array[T.hash::tonhash]`
		}
		if len(filter_str) > 0 && !join_accounts {
			if order_by_now {
				clmn_query = `distinct on (A.trace_end_utime, A.trace_id, A.end_utime, A.action_id) ` + clmn_query_default
			} else {
				clmn_query = `distinct on (A.trace_end_lt, A.trace_id, A.end_lt, A.action_id) ` + clmn_query_default
			}
		}
	}
	if v := req.MessageHash; len(v) > 0 {
		filter_str := fmt.Sprintf("(%s or %s)", filterByArray("M.msg_hash", v), filterByArray("M.msg_hash_norm", v))
		filter_list = append(filter_list, filter_str)
		from_query += ` join messages as M on A.trace_id = M.trace_id and array[M.tx_hash::tonhash] <@ A.tx_hashes`
		if !join_accounts {
			if order_by_now {
				clmn_query = `distinct on (A.trace_end_utime, A.trace_id, A.end_utime, A.action_id) ` + clmn_query_default
			} else {
				clmn_query = `distinct on (A.trace_end_lt, A.trace_id, A.end_lt, A.action_id) ` + clmn_query_default
			}
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
		filter_list = append(filter_list, fmt.Sprintf("A.trace_mc_seqno_end = %d", *v))
	}
	if v := req.ActionId; v != nil {
		filter_str := filterByArray("A.action_id", v)
		if len(filter_str) > 0 {
			filter_list = append(filter_list, filter_str)
		}
	}
	if v := req.TraceId; v != nil {
		filter_str := filterByArray("A.trace_id", v)
		if len(filter_str) > 0 {
			filter_list = append(filter_list, filter_str)
		}
	}
	if strings.Contains(from_query, "action_accounts") {
		if order_by_now {
			orderby_query = fmt.Sprintf(" order by AA.account %s, AA.trace_end_utime %s, AA.trace_id %s, AA.action_end_utime %s, AA.action_id %s",
				sort_order, sort_order, sort_order, sort_order, sort_order)
		} else {
			orderby_query = fmt.Sprintf(" order by AA.account %s, AA.trace_end_lt %s, AA.trace_id %s, AA.action_end_lt %s, AA.action_id %s",
				sort_order, sort_order, sort_order, sort_order, sort_order)
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
	if v := req.IncludeActionTypes; len(v) == 0 {
		filter_list = append(filter_list, "A.type = ANY($1)")
	}
	filter_list = append(filter_list, "NOT(A.ancestor_type && $1::varchar[])")

	// trace_end_lt/utime is the sort axis the router classifies on.
	orderKey := "lt"
	if order_by_now {
		orderKey = "utime"
	}

	return actionsQueryParts{
		clmnQuery:  clmn_query,
		fromQuery:  from_query,
		filterList: filter_list,
		args:       args,
		orderBy:    orderby_query,
		orderKey:   orderKey,
	}
}

// actionsQueryParts holds the shared pieces of an actions listing; orderKey names
// the sort axis ("lt" or "utime") the router classifies the request window on.
type actionsQueryParts struct {
	clmnQuery  string
	fromQuery  string
	filterList []string
	args       []any
	orderBy    string
	orderKey   string
}

func buildActionsOffsetQueryV2(p actionsQueryParts, offset, limit int) string {
	filter_query := ``
	if len(p.filterList) > 0 {
		filter_query = ` where ` + strings.Join(p.filterList, " and ")
	}
	limit_query := fmt.Sprintf(" limit %d offset %d", max(1, limit), max(0, offset))
	return `select ` + p.clmnQuery + ` from ` + p.fromQuery + filter_query + p.orderBy + limit_query
}

func queryRawActionsImplV2(query string, args []any, conn *pgxpool.Conn, settings models.RequestSettings) ([]models.RawAction, error) {
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

func queryActionsTransactionsImpl(fc *fedConns, release func(), actions []models.Action, settings models.RequestSettings, store *KvrocksStore) ([]models.Action, error) {
	if len(actions) == 0 {
		return actions, nil
	}

	// Collect all unique transaction hashes from all actions
	txHashSet := make(map[models.HashType]bool)
	for _, action := range actions {
		for _, txHash := range action.TxHashes {
			txHashSet[txHash] = true
		}
	}

	if len(txHashSet) == 0 {
		return actions, nil
	}

	// Convert set to slice
	txHashes := make([]models.HashType, 0, len(txHashSet))
	for hash := range txHashSet {
		txHashes = append(txHashes, hash)
	}

	// Build query using buildTransactionsQuery with multiple hashes
	// Only Hash is relevant here; the rest of TransactionRequest stays zero.
	tx_req := models.TransactionsRequest{Hash: txHashes}
	query, queryArgs, err := buildTransactionsQuery(tx_req, settings)
	if err != nil {
		return nil, models.IndexError{Code: 500, Message: err.Error()}
	}

	// tx hashes across a page of actions can be hot- or cold-only -> consult both
	txs, err := hotThenCold(fc, len(txHashes),
		func(conn *pgxpool.Conn) ([]models.Transaction, error) {
			return queryTransactionsImpl(query, conn, settings, store, queryArgs...)
		},
		func(t *models.Transaction) models.HashType { return t.Hash })
	if err != nil {
		return nil, models.IndexError{Code: 500, Message: err.Error()}
	}
	if store != nil {
		release()
		if err := finalizeTransactionSliceFromKvrocks(txs, nil, store, settings); err != nil {
			return nil, models.IndexError{Code: 500, Message: err.Error()}
		}
	}

	// Create a map of transactions by hash
	txMap := make(map[models.HashType]*models.Transaction)
	for i := range txs {
		txMap[txs[i].Hash] = &txs[i]
	}

	// Assign transactions to actions
	for i := range actions {
		actions[i].Transactions = make([]*models.Transaction, 0, len(actions[i].TxHashes))
		for _, txHash := range actions[i].TxHashes {
			if tx, ok := txMap[txHash]; ok {
				actions[i].Transactions = append(actions[i].Transactions, tx)
			}
		}
	}

	return actions, nil
}
