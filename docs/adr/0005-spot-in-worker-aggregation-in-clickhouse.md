# Spot price пула считает worker, агрегацию по пулам — ClickHouse

Worker считает Spot price каждого пула (включая итерации Ньютона для stable Curve) и пишет Pool snapshot в ClickHouse. Агрегацию в Price jetton'а, конвертацию в USDT и Candles делает ClickHouse: materialized views и периодическая задача.

Вариант с графом цен в памяти worker'а отвергнут. Worker остаётся без состояния по ценам, правила агрегации можно менять и пересчитывать без переиндексации, а бэкфилл идёт тем же путём.
