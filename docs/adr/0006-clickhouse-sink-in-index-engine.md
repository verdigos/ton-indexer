# Писатель в ClickHouse — sink в index-engine, общий для live и бэкфилла

Pool snapshots пишет новый sink в `index-engine`. Его подключают и живой worker (`ton-index-postgres`), и исторические прогоны по диапазонам masterchain seqno.

Отвергнуты два варианта: CDC Postgres → ClickHouse и отдельный процесс. В обоих live и бэкфилл шли бы разными путями, а в первом ещё и появлялась бы лишняя промежуточная база.
