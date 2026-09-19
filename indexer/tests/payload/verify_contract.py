#!/usr/bin/env python3
"""Verify mapping coverage, reversibility, golden shapes, and hash IDs. No DB I/O."""
import base64
import hashlib
import json
import re
from pathlib import Path
import yaml  # Existing project dependency; only needed to read existing goldens.

HERE = Path(__file__).parent
ROOT = Path(__file__).resolve().parents[3]
C = json.loads((HERE/'mapping.json').read_text())
INV = json.loads((HERE/'source-inventory.json').read_text())

def transform(value, spec, reverse=False, producer=False, path=''):
    if value is None:
        return None
    kind = spec['kind']
    if kind == 'object':
        assert isinstance(value,dict), (path,'expected object')
        fields = spec['fields']
        lookup = {key if reverse else child.get('legacy_name',key):(key,child) for key,child in fields.items()}
        normalized = dict(value)
        # Explicit producer-only alias for the historically dropped outbid comment.
        if producer and path == 'ton_transfer_data' and 'comment' in normalized and 'content' in lookup:
            if 'content' in normalized and normalized['content'] != normalized['comment']:
                raise AssertionError('Conflicting content/comment alias')
            normalized['content'] = normalized.pop('comment')
        assert not set(normalized)-set(lookup), (path,'unknown fields', sorted(set(normalized)-set(lookup)))
        out = {}
        for key,val in normalized.items():
            name,child = lookup[key]
            target = child.get('legacy_name',name) if reverse else name
            assert target not in out, (path,target,'target collision')
            out[target] = transform(val,child,reverse,producer,path+'.'+key)
        return out
    if kind == 'array':
        assert isinstance(value,list), (path,'expected array')
        return [transform(v,spec['items'],reverse,producer,path+'[]') for v in value]
    if kind == 'integer_string':
        assert isinstance(value,(int,str)) and not isinstance(value,bool), (path,'integer without float hop',value)
        text = str(value)
        assert re.fullmatch(r'0|-?[1-9][0-9]*',text), (path,'noncanonical integer',text)
        return text
    if kind == 'boolean':
        assert isinstance(value,bool), (path,'expected bool',value)
    else:
        assert isinstance(value,str), (path,'expected string',value)
    return value

def convert(action, legacy, producer=False):
    groups = C['actions'][action]['groups']
    allowed = {g['legacy_column'] for g in groups.values()}
    unexpected = {key for key in set(legacy)-allowed if key not in C['legacy_detail_columns'] or legacy[key] is not None}
    assert not unexpected, (action,'unexpected populated column',unexpected)
    return {name:transform(legacy[g['legacy_column']],g['schema'],producer=producer and action=='auction_outbid',path=g['legacy_column']) for name,g in groups.items() if g['legacy_column'] in legacy}

def restore(action,payload):
    groups = C['actions'][action]['groups']
    assert not set(payload)-set(groups), 'Unknown payload group'
    return {groups[name]['legacy_column']:transform(value,groups[name]['schema'],reverse=True,path=name) for name,value in payload.items()}

counter = 0
def specimen(spec):
    global counter
    counter += 1
    k=spec['kind']
    if k=='object':return {v.get('legacy_name',n):specimen(v) for n,v in spec['fields'].items()}
    if k=='array':return [specimen(spec['items']),None,specimen(spec['items'])]
    if k=='integer_string':return str(2**200+counter)
    if k=='boolean':return bool(counter%2)
    if k=='ton_address':return '-1:'+f'{counter:064X}'
    if k=='hash_base64':return base64.b64encode(counter.to_bytes(32,'big')).decode()
    if k=='bytes_base64':return base64.b64encode(bytes([0,255,34,92])+str(counter).encode()).decode()
    return f'{counter}: quote=" slash=\\ unicode=тест'

def reject(fn):
    try:fn()
    except AssertionError:return
    raise AssertionError('Expected rejection')

def main():
    all_columns=set()
    synthetic=0
    for action,entry in C['actions'].items():
        source={g['legacy_column']:specimen(g['schema']) for g in entry['groups'].values()}
        all_columns.update(source)
        for test in (source,{},dict.fromkeys(source),{g['legacy_column']:{} for g in entry['groups'].values()}):
            assert restore(action,convert(action,test))==test, action
            synthetic+=1
        sql_columns = dict.fromkeys(C['legacy_detail_columns'])
        sql_columns.update(source)
        assert restore(action,convert(action,sql_columns)) == source, action
        synthetic+=1
        # Shape-level distinctions are part of reversibility.
        for group,info in entry['groups'].items():
            source={info['legacy_column']:{child.get('legacy_name',name):None for name,child in info['schema']['fields'].items()}}
            assert restore(action,convert(action,source))==source
            synthetic+=1
        reject(lambda:convert(action,{'unregistered_detail_data':{}}))

    assert all_columns-{'extra'}==set(C['legacy_detail_columns'])
    for pg_type in C['legacy_detail_columns'].values():
        assert pg_type in INV['composite_types']

    # Independent Luna inventory confirms all persisted types after fill-time renames.
    luna=json.loads((HERE/'action-inventory-luna.json').read_text())
    assert set(C['actions'])==set(luna['by_emitted_type'])|{'unknown','tgbtc_mint_fallback','tgbtc_burn_fallback','tgbtc_new_key_fallback'}
    for action,item in luna['by_emitted_type'].items():
        declared={g['legacy_column'] for g in C['actions'][action]['groups'].values()}
        actual={k for k in item['detail_columns'] if '.' not in k}
        assert actual <= declared, (action,actual-declared)

    goldens=sorted((ROOT/'ton-index-worker/ton-mch-engine/goldens/actions').glob('**/*.yaml'))
    golden_actions=0
    types=set()
    present_fields=set()
    for path in goldens:
        document=yaml.safe_load(path.read_text())
        for row in document.get('actions',[]):
            action=row['type']
            assert action in C['actions'],(str(path),action)
            legacy={key:val for key,val in row.items() if key in C['legacy_detail_columns'] or key=='extra'}
            payload=convert(action,legacy,producer=True)
            restored=restore(action,payload)
            # Compare after only documented alias and exact integer normalization.
            expected={}
            for group,g in C['actions'][action]['groups'].items():
                key=g['legacy_column']
                if key in legacy:
                    expected[key]=transform(transform(legacy[key],g['schema'],producer=True,path=key),g['schema'],reverse=True)
            assert restored==expected,(str(path),action)
            for key,val in legacy.items():
                if isinstance(val,dict):
                    present_fields.update((key,k) for k in val)
            golden_actions+=1
            types.add(action)

    reject(lambda:transform('1.5',{'kind':'integer_string'}))
    reject(lambda:transform(1.0,{'kind':'integer_string'}))
    reject(lambda:transform('01',{'kind':'integer_string'}))
    reject(lambda:transform('-0',{'kind':'integer_string'}))
    reject(lambda:transform('1e3',{'kind':'integer_string'}))
    assert transform('-887272',{'kind':'integer_string'})=='-887272'
    assert transform(str(2**256-1),{'kind':'integer_string'})==str(2**256-1)
    reject(lambda:convert('auction_outbid',{'ton_transfer_data':{'content':'a','comment':'b'}},producer=True))
    assert convert('auction_outbid',{'ton_transfer_data':{'comment':'refund'}},producer=True)=={'comment_data':{'comment':'refund'}}
    reject(lambda:convert('ton_transfer',{'ton_transfer_data':{'comment':'not a registered alias'}},producer=True))
    reject(lambda:convert('ton_transfer',{'ton_transfer_data':{'unknown_future_field':'x'}}))

    keys=C['key_dictionary']
    assert len({v['stable_id'] for v in keys.values()})==len(keys)
    assert all(isinstance(v['stable_id'],int) and v['stable_id']>0 for v in keys.values())
    assert len({v['sha256_24_hex'] for v in keys.values()})==len(keys)
    assert all(hashlib.sha256(k.encode()).hexdigest()[:6]==v['sha256_24_hex'] for k,v in keys.items())
    targetpaths=set()
    for row in C['field_mappings']:
        identity=(row['action_type'],row['payload_path'])
        assert identity not in targetpaths,identity
        targetpaths.add(identity)
    results={'status':'passed','action_types':len(C['actions']),'detail_columns_covered':len(C['legacy_detail_columns']), 'synthetic_roundtrips':synthetic,'golden_files':len(goldens),'golden_actions':golden_actions,'golden_action_types':len(types),'types_without_goldens':sorted(set(C['actions'])-types),'distinct_json_keys':len(keys),'mapping_entries':len(C['field_mappings']),'golden_scope':'Existing golden field shapes and reversible mapping after documented integer/alias normalization; cellhash placeholders are not real BOCs and were not validated as base64. No PostgreSQL or production-row roundtrip was performed.'}
    (HERE/'verification.json').write_text(json.dumps(results,indent=2)+'\n')
    print(json.dumps(results,indent=2))


if __name__ == "__main__":
    main()
