#!/usr/bin/env python3
"""Build the reviewable contract from a pinned source tree; no database access."""
import ast
import hashlib
import json
import re
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
OUT = Path(__file__).resolve().parent
CPP_PATH = 'ton-index-worker/ton-mch-engine/src/ActionBuild.cpp'
DDL_PATH = 'ton-index-worker/ton-index-postgres/src/migrate.cpp'
PY_PATH = 'indexer/indexer/events/blocks/utils/block_tree_serializer.py'
cpp = (ROOT / CPP_PATH).read_text()
ddl = (ROOT / DDL_PATH).read_text()

def dump(name, value):
    (OUT / name).write_text(json.dumps(value, indent=2, ensure_ascii=False) + '\n')

def line_at(text, offset):
    return text[:offset].count('\n') + 1

def strip_comments(text):
    return re.sub(r'//[^\n]*|/\*[\s\S]*?\*/', lambda m: ''.join('\n' if c == '\n' else ' ' for c in m[0]), text)

def parse_functions(text):
    clean = strip_comments(text)
    funcs = {}
    for m in re.finditer(r'^(?:void|bool|Value|Action)\s+(\w+)\([^;]*?\)\s*\{', clean, re.M):
        start = m.end()
        # Ignore braces inside string literals when finding the function end.
        tail = re.sub(r'"(?:\\.|[^"\\])*"', lambda s: ' ' * len(s[0]), clean[start:])
        depth = 1
        for i, char in enumerate(tail):
            depth += (char == '{') - (char == '}')
            if depth == 0:
                funcs[m[1]] = {'body': clean[start:start+i], 'line': line_at(text, m.start())}
                break
    return funcs

funcs = parse_functions(cpp)
def closure(name, seen=None):
    seen = set() if seen is None else seen
    if name in seen:
        return seen
    seen.add(name)
    for called in re.findall(r'\b(fill_\w+)\(', funcs[name]['body']):
        if called in funcs:
            closure(called, seen)
    return seen

composites = {}
for m in re.finditer(r'create type (\w+) as\s*\((.*?)\);', ddl, re.I | re.S):
    fields = {}
    for fm in re.finditer(r'^\s*(\w+)\s+(\w+(?:\[\])?)\s*,?\s*$', m[2], re.M):
        fields[fm[1]] = {'pg_type': fm[2].lower(), 'source': f'{DDL_PATH}:{line_at(ddl, m.start(2)+fm.start())}'}
    if fields:
        composites[m[1]] = fields
table = re.search(r'create table if not exists actions\s*\((.*?)\) partition', ddl, re.S)[1]
columns = dict(re.findall(r'^\s*(\w+)\s+(\w+(?:\[\])?)', table, re.M))
detail_cols = {key: val for key, val in columns.items() if val in composites}
assert len(detail_cols) == 39, detail_cols

inventory = defaultdict(lambda: {'input_block_types': [], 'fill_functions': [], 'detail_columns': set(), 'sources': []})
dispatch = funcs['fill_action']['body']
for match in re.finditer(r'(?:\bif|else if)\s*\((.*?)\)\s*(?:\{\s*)?(fill_\w+)\(', dispatch, re.S):
    inputs = re.findall(r't == "([^"]+)"', match[1])
    if not inputs:
        continue
    fill = match[2]
    names = closure(fill)
    body = '\n'.join(funcs[n]['body'] for n in names)
    renamed = set(re.findall(r'a\.type\s*=\s*"([^"]+)"', body))
    outputs = renamed or set(inputs)
    used = set(re.findall(r'a\.(\w+_data)\s*=', body))
    assert used <= set(detail_cols), used-set(detail_cols)
    for output in outputs:
        item = inventory[output]
        item['input_block_types'] += inputs
        item['fill_functions'] += sorted(names)
        item['detail_columns'].update(used)
        item['sources'] += [f'{CPP_PATH}:{funcs[n]["line"]}' for n in sorted(names)]
inventory['unknown']['sources'] = [f'{CPP_PATH}:{funcs["create_unknown_action"]["line"]}']
# Historical names still accepted by the API; the current producer rewrites them.
for legacy in ('tgbtc_mint_fallback', 'tgbtc_burn_fallback', 'tgbtc_new_key_fallback'):
    inventory[legacy]['sources'] = ['ton-index-go/index/parse/actions.go:765']

# Resolve Python dispatch and function calls independently, including output type rewrites.
pytree = ast.parse((ROOT / PY_PATH).read_text())
pyfuncs = {n.name: n for n in pytree.body if isinstance(n, (ast.FunctionDef, ast.AsyncFunctionDef))}
def pyclosure(name, seen=None):
    seen = set() if seen is None else seen
    if name in seen or name not in pyfuncs:
        return seen
    seen.add(name)
    for n in ast.walk(pyfuncs[name]):
        if isinstance(n, ast.Call) and isinstance(n.func, ast.Name) and n.func.id.startswith('_fill_'):
            pyclosure(n.func.id, seen)
    return seen

py_inventory = defaultdict(set)
for node in ast.walk(pyfuncs['block_to_action']):
    if not isinstance(node, ast.Match):
        continue
    for case in node.cases:
        inputs = [n.value for n in ast.walk(case.pattern) if isinstance(n, ast.Constant) and isinstance(n.value, str)]
        fills = set()
        for stmt in case.body:
            for n in ast.walk(stmt):
                if isinstance(n, ast.Call) and isinstance(n.func, ast.Name) and n.func.id.startswith('_fill_'):
                    fills |= pyclosure(n.func.id)
        assigned_types, used = set(), set()
        for fn in fills:
            for n in ast.walk(pyfuncs[fn]):
                if isinstance(n, ast.Assign):
                    for t in n.targets:
                        if isinstance(t, ast.Attribute) and isinstance(t.value, ast.Name) and t.value.id == 'action':
                            if t.attr == 'type' and isinstance(n.value, ast.Constant):
                                assigned_types.add(n.value.value)
                            if t.attr.endswith('_data'):
                                used.add(t.attr)
        for output in assigned_types or set(inputs):
            py_inventory[output] |= used
            if output not in inventory:
                inventory[output]['sources'] = [f'{PY_PATH}:{pyfuncs[fn].lineno}' for fn in sorted(fills)]
                inventory[output]['input_block_types'] = inputs
            inventory[output]['detail_columns'].update(used & set(detail_cols))

RENAMES = {
    'ton_transfer_details': {'content': 'comment', 'encrypted': 'comment_encrypted'},
    'jetton_transfer_details': {'is_encrypted_comment': 'comment_encrypted'},
    'nft_transfer_details': {'nft_item_index': 'nft_index'},
    'nft_listing_details': {'nft_item_index': 'nft_index'},
    'nft_mint_details': {'nft_item_index': 'nft_index'},
    'dex_withdraw_liquidity_details': {'asset1_out': 'asset1', 'asset2_out': 'asset2', 'burned_nft_index': 'nft_index', 'burned_nft_address': 'nft_address'},
    'coffee_create_pool_details': {'amount_1': 'amount1', 'amount_2': 'amount2'},
    'coffee_staking_deposit_details': {'minted_item_index': 'nft_index', 'minted_item_address': 'nft_address'},
    'tonco_deploy_pool_details': {'jetton0_minter': 'asset1', 'jetton1_minter': 'asset2', 'jetton0_router_wallet': 'dex_jetton_wallet_1', 'jetton1_router_wallet': 'dex_jetton_wallet_2'},
    'change_dns_record_details': {'value': 'record_value'},
}

WIRE = {'numeric': 'integer_string', 'integer': 'integer_string', 'bigint': 'integer_string', 'boolean': 'boolean', 'varchar': 'string', 'text': 'string', 'tonaddr': 'ton_address', 'tonhash': 'hash_base64', 'tonbytes': 'bytes_base64'}

def schema_for(pgtype):
    if pgtype.endswith('[]'):
        return {'kind': 'array', 'nullable': True, 'items': schema_for(pgtype[:-2])}
    if pgtype in composites:
        return {'kind': 'object', 'nullable': True, 'fields': {RENAMES.get(pgtype, {}).get(k,k): dict(schema_for(v['pg_type']), legacy_name=k, source=v['source']) for k,v in composites[pgtype].items()}}
    assert pgtype in WIRE, pgtype
    return {'kind': WIRE[pgtype], 'nullable': True}

def group_for(col):
    return {'nft_listing_data': 'listing', 'layerzero_packet_data': 'packet', 'ton_transfer_data': 'comment_data'}.get(col, 'data')

actions = {}
for action, inv in sorted(inventory.items()):
    groups = {}
    for col in sorted(inv['detail_columns']):
        name = group_for(col)
        if col == 'ton_transfer_data' and len(inv['detail_columns']) == 1:
            name = 'data'
        assert name not in groups, (action, col, groups)
        groups[name] = {'legacy_column': col, 'legacy_type': detail_cols[col], 'schema': schema_for(detail_cols[col])}
    extra = {'parent_gasless_action': {'kind': 'hash_base64', 'nullable': True, 'meaning': 'Action ID of the immediate gasless-request parent; not the consumption-tree parent.', 'source': f'{CPP_PATH}:1712'}}
    if action in ('tgbtc_mint', 'tgbtc_mint_fallback'):
        extra['btc_txid'] = {'kind': 'string', 'nullable': True, 'meaning': 'Bitcoin transaction ID; preserve current textual representation.', 'source': f'{CPP_PATH}:925'}
    if action in ('tgbtc_new_key', 'tgbtc_new_key_fallback', 'tgbtc_dkg_log_fallback'):
        extra['pubkey'] = {'kind': 'string', 'nullable': True, 'meaning': 'TgBTC public key; preserve current textual representation.', 'source': f'{CPP_PATH}:957'}
    groups['extra'] = {'legacy_column': 'extra', 'legacy_type': 'jsonb', 'schema': {'kind': 'object', 'nullable': True, 'fields': extra}}
    actions[action] = {'groups': groups, 'input_block_types': sorted(set(inv['input_block_types'])), 'fill_functions': sorted(set(inv['fill_functions'])), 'sources': sorted(set(inv['sources'])), 'legacy_only': not bool(inv['fill_functions']) and action != 'unknown'}

contract = {
    'contract': 'mch-action-payload', 'version': 1, 'code_commit': 'a9c4c893',
    'scope': 'Replace 39 detail composites and the existing extra column. All other actions columns and action_accounts remain unchanged.',
    'identity': ['payload_ver', 'actions.type', 'object_path', 'field_name'],
    'envelope_columns': sorted(set(columns)-set(detail_cols)-{'extra','primary'}),
    'wire_types': {
        'integer_string': 'Canonical signed base-10 integer JSON string, no exponent, leading plus, leading zeros or negative zero. Current producer values are integral, including raw fee factors and Q64.96. Legacy fractional/non-finite numeric values fail migration validation; never round them.',
        'ton_address': 'Canonical signed-workchain:64 uppercase hex digits; null remains null. No conversion through int8 workchain.',
        'hash_base64': 'RFC4648 standard padded base64 of exactly32 bytes.',
        'bytes_base64': 'RFC4648 standard padded base64 of original bytes. For BOCs preserve bytes; never reserialize during backfill.',
        'string': 'Preserve textual value exactly, including empty string. Field-specific representation follows the mapped legacy field.',
        'boolean': 'JSON true or false, not numeric or string.',
    },
    'renames_by_legacy_type': RENAMES, 'actions': actions,
    'source_only_fields': [
        {'action_types':['auction_outbid'], 'source':'ton_transfer_data.comment', 'target':'comment_data.comment', 'policy':'producer alias only; SQL source is content. Existing lost comments cannot be recovered by backfill.'},
        {'action_types':['dex_withdraw_liquidity','dedust_v2_claim_fees','dedust_v2_claim_reward'], 'source':'dex_withdraw_liquidity_data.is_refund', 'target':'data.is_refund', 'policy':'Retain when supplied by the new producer. No inferred value for old SQL rows: this field was not persisted in their composite.'},
        {'action_types':['evaa_withdraw'], 'source':'evaa_withdraw_data.is_ton', 'target':'data.is_ton', 'policy':'Retain when supplied by the new producer. No inferred value for old SQL rows: this field was not persisted in their composite.'},
    ],
}
# These two known producer fields have explicit, closed names even though absent from legacy DDL.
for entry in contract['source_only_fields'][1:]:
    for action in entry['action_types']:
        if action in actions and 'data' in actions[action]['groups']:
            actions[action]['groups']['data']['schema']['fields'][entry['target'].split('.')[-1]] = {'kind':'boolean','nullable':True,'producer_only':True,'source':entry['source'],'meaning':entry['policy']}

field_index = defaultdict(list)
mappings = []
def walk(action, group, spec, src, dst):
    kind = spec['kind']
    if kind == 'object':
        for key, child in spec['fields'].items():
            field_index[key].append({'action_type':action,'path':dst+'.'+key,'kind':child['kind']})
            walk(action, group, child, src+'.'+child.get('legacy_name',key), dst+'.'+key)
    elif kind == 'array':
        mappings.append({'action_type':action,'legacy_path':src,'payload_path':dst,'kind':'array','source':spec.get('source')})
        walk(action, group, spec['items'], src+'[]',dst+'[]')
    else:
        mappings.append({'action_type':action,'legacy_path':src,'payload_path':dst,'kind':kind,'producer_only':spec.get('producer_only',False),'source':spec.get('source')})
for action, entry in actions.items():
    for group, info in entry['groups'].items():
        field_index[group].append({'action_type':action,'path':group,'kind':'object'})
        walk(action, group, info['schema'], info['legacy_column'], group)
contract['field_mappings'] = mappings
contract['legacy_detail_columns'] = detail_cols
keys = sorted(field_index)
# Keep existing IDs when this reviewed registry is extended. Never renumber or recycle.
previous_path = OUT / 'mapping.json'
previous_keys = json.loads(previous_path.read_text()).get('key_dictionary', {}) if previous_path.exists() else {}
stable_ids = {key: entry['stable_id'] for key,entry in previous_keys.items() if 'stable_id' in entry}
assert set(stable_ids) <= set(keys), 'Removed keys must remain reserved in the registry; review before generation.'
next_id = max(stable_ids.values(), default=0) + 1
for key in keys:
    if key not in stable_ids:
        stable_ids[key] = next_id
        next_id += 1
assert len(set(stable_ids.values())) == len(stable_ids)
contract['key_dictionary'] = {key:{'stable_id':stable_ids[key], 'sha256_24_hex':hashlib.sha256(key.encode('utf8')).hexdigest()[:6], 'uses':field_index[key]} for key in keys}
contract['source_sha256'] = {path:hashlib.sha256((ROOT/path).read_bytes()).hexdigest() for path in (CPP_PATH, DDL_PATH, PY_PATH, 'ton-index-go/index/parse/actions.go')}
dump('mapping.json', contract)
dump('source-inventory.json', {'actions':{k:{**v,'detail_columns':sorted(v['detail_columns'])} for k,v in sorted(inventory.items())}, 'python_output_types':sorted(py_inventory), 'cpp_functions':len(funcs), 'composite_types':composites})

def json_schema(spec):
    k = spec['kind']
    if k == 'object':
        s = {'type':'object','properties':{name:json_schema(child) for name,child in spec['fields'].items()},'additionalProperties':False}
    elif k == 'array':
        s = {'type':'array','items':json_schema(spec['items'])}
    elif k == 'boolean':
        s = {'type':'boolean'}
    else:
        s = {'type':'string'}
        if k == 'integer_string': s['pattern'] = r'^(?:0|-?[1-9][0-9]*)$'
        if k == 'ton_address': s['pattern'] = r'^(?:0|-?[1-9][0-9]*):[0-9A-F]{64}$'
        if k == 'hash_base64': s['pattern'] = r'^[A-Za-z0-9+/]{43}=$'
        if k == 'bytes_base64': s['pattern'] = r'^(?:[A-Za-z0-9+/]{4})*(?:[A-Za-z0-9+/]{2}==|[A-Za-z0-9+/]{3}=)?$'
    if spec.get('nullable'): s['type'] = [s['type'],'null']
    return s

schema = {'$schema':'https://json-schema.org/draft/2020-12/schema','$id':'urn:mch:action-payload:1','title':'Writer-side validation for type + payload_ver + payload; scalar envelope not represented here.','oneOf':[]}
for action,entry in actions.items():
    schema['oneOf'].append({'type':'object','required':['type','payload_ver','payload'],'additionalProperties':False,'properties':{'type':{'const':action},'payload_ver':{'const':1},'payload':{'type':'object','additionalProperties':False,'properties':{g:json_schema(info['schema']) for g,info in entry['groups'].items()}}}})
dump('payload.schema.json',schema)

lines=['# Payload fields by action type','', 'Generated from mapping.json by build_contract.py. Paths are relative to payload. Every group/field is optional; explicit null is preserved. Array elements retain order and may be null. See CONTRACT.md for source-presence rules.','']
for action,entry in actions.items():
    lines += [f'## {action}', '', '| Payload path | JSON value type | Legacy source |','|---|---|---|']
    for row in mappings:
        if row['action_type']==action:
            lines.append(f'| `{row["payload_path"]}` | `{row["kind"]}` | `{row["legacy_path"]}`'+('; producer-only' if row.get('producer_only') else '')+' |')
    lines += ['']
(OUT/'FIELDS.md').write_text('\n'.join(lines)+'\n')

dictionary_lines=['# Closed JSON key dictionary','', 'A key identifies a member within an object. Its meaning is determined by action type and full path; shared spelling does not imply shared units. Numeric IDs and hashes are future binary encoding candidates, not JSON aliases. Stable IDs are append-only: never renumber, reuse, or derive them again from alphabetical position.','', '| Key | Stable ID | SHA-256 first 3 bytes, hex | JSON kinds across uses | Action types using it |','|---|---:|---|---|---:|']
for key in keys:
    dictionary_lines.append(f'| `{key}` | {stable_ids[key]} | `{contract["key_dictionary"][key]["sha256_24_hex"]}` | '+', '.join(sorted({v['kind'] for v in field_index[key]}))+f' | {len({v["action_type"] for v in field_index[key]})} |')
(OUT/'KEYS.md').write_text('\n'.join(dictionary_lines)+'\n')

hashes = {}
for width in (1,2,3):
    buckets = defaultdict(list)
    for key in keys: buckets[hashlib.sha256(key.encode()).digest()[:width].hex()].append(key)
    hashes[str(width)] = {h:names for h,names in buckets.items() if len(names)>1}
assert not hashes['3'], hashes['3']
dump('hash-check.json',{'algorithm':'SHA-256 over UTF-8 field name, leading N bytes; same spelling shares an ID at all object paths','key_count':len(keys),'collisions_by_width_bytes':hashes,'production_encoding_enabled':False})
print(json.dumps({'actions':len(actions),'legacy_detail_columns':len(detail_cols),'mapped_entries':len(mappings),'distinct_json_keys':len(keys),'collision_groups':{k:len(v) for k,v in hashes.items()}},indent=2))

if __name__ == '__main__':
    pass
