import argparse
import asyncio
import logging
import multiprocessing as mp
import sys
import time

from sqlalchemy import update, select
from sqlalchemy.ext.asyncio import AsyncSession
from sqlalchemy.orm import sessionmaker, contains_eager

from indexer.core import redis
from indexer.core.database import engine, Trace, Transaction, Message, Action
from indexer.core.settings import Settings
from indexer.events import context
from indexer.events.blocks.utils.block_tree_serializer import block_to_action
from indexer.events.blocks.utils.event_deserializer import deserialize_event
from indexer.events.event_processing import process_event_async
from indexer.events.interface_repository import EmulatedTransactionsInterfaceRepository, gather_interfaces, \
    RedisInterfaceRepository

async_session = sessionmaker(engine, expire_on_commit=False, class_=AsyncSession)
logging.basicConfig(stream=sys.stdout, level=logging.INFO)
logger = logging.getLogger(__name__)
settings = Settings()


async def start_processing_events_from_db(args: argparse.Namespace):
    logger.info(f"Creating pool of {args.pool_size} workers")
    with mp.Pool(args.pool_size, initializer=init_pool) as pool:
        while True:
            async with async_session() as session:
                batch = await fetch_events_for_processing(args.fetch_size)
                if len(batch) == 0:
                    await asyncio.sleep(2)
                    continue
                ids = []
                has_traces_to_process = False
                for (trace_id, nodes) in batch:
                    if nodes > 500 or nodes == 0:
                        await session.execute(update(Trace)
                                              .where(Trace.trace_id == trace_id)
                                              .values(classification_state='ok'))
                        has_traces_to_process = True
                    else:
                        has_traces_to_process = True
                        ids.append(trace_id)
                await session.commit()
                if has_traces_to_process:
                    pool.map(process_event_batch, split_into_batches(ids, args.batch_size))
                else:
                    await asyncio.sleep(2)


def init_pool():
    asyncio.set_event_loop(asyncio.new_event_loop())


def split_into_batches(data, batch_size):
    for i in range(0, len(data), batch_size):
        yield data[i:i + batch_size]


async def start_emulated_traces_processing():
    pubsub = redis.client.pubsub()
    await pubsub.subscribe(settings.emulated_traces_reddit_channel)
    while True:

        message = await pubsub.get_message()
        if message is not None and message['type'] == 'message':
            trace_id = message['data'].decode('utf-8')
            try:
                start = time.time()
                res = await process_emulated_trace(trace_id)
            except Exception as e:
                logger.error(f"Failed to process emulated trace {trace_id}: {e}")
        else:
            await asyncio.sleep(1)


async def process_emulated_trace(trace_id):
    trace_map = await redis.client.hgetall(trace_id)
    trace_map = dict((str(key, encoding='utf-8'), value) for key, value in trace_map.items())
    trace = deserialize_event(trace_id, trace_map)
    context.interface_repository.set(EmulatedTransactionsInterfaceRepository(trace_map))
    return await process_event_async(trace)


async def fetch_events_for_processing(size: int):
    async with async_session() as session:
        query = select(Trace.trace_id, Trace.nodes_) \
            .filter(Trace.state == 'complete') \
            .filter(Trace.classification_state == 'unclassified') \
            .order_by(Trace.start_lt.asc()) \
            .limit(size)
        traces = await session.execute(query)
        return traces.all()


def process_event_batch(ids: list[str]):
    asyncio.get_event_loop().run_until_complete(process_trace_batch_async(ids))
    return None


async def process_trace_batch_async(ids: list[str]):
    async with async_session() as session:
        query = select(Trace) \
            .join(Trace.transactions) \
            .join(Trace.edges, isouter=True) \
            .join(Transaction.messages, isouter=True) \
            .join(Message.message_content, isouter=True) \
            .options(contains_eager(Trace.transactions, Transaction.messages, Message.message_content),
                     contains_eager(Trace.edges)) \
            .filter(Trace.trace_id.in_(ids))
        result = await session.execute(query)
        events = result.scalars().unique().all()
        # Gather interfaces for each account
        accounts = set()
        for trace in events:
            for tx in trace.transactions:
                accounts.add(tx.account)
        interfaces = await gather_interfaces(accounts, session)
        repository = RedisInterfaceRepository(redis.client)
        await repository.put_interfaces(interfaces)
        context.interface_repository.set(repository)
        # Process traces and save actions
        results = await asyncio.gather(*(process_trace(event) for event in events))
        ok_traces = []
        failed_traces = []
        for trace_id, success, actions in results:
            if success:
                session.add_all(actions)
                ok_traces.append(trace_id)
            else:
                failed_traces.append(trace_id)
        stmt = update(Trace).where(Trace.trace_id.in_(ok_traces)).values(classification_state='ok')
        await session.execute(stmt)
        stmt = update(Trace).where(Trace.trace_id.in_(failed_traces)).values(classification_state='failed')
        await session.execute(stmt)
        await session.commit()


async def process_trace(trace: Trace) -> tuple[str, bool, list[Action]]:
    if len(trace.transactions) == 1 and trace.transactions[0].descr == 'tick_tock' and len(trace.edges) == 0:
        return trace.trace_id, True, []
    try:
        result = await process_event_async(trace)
        actions = []
        for block in result.bfs_iter():
            if block.btype != 'root':
                if block.btype == 'call_contract' and block.event_nodes[0].message.destination is None:
                    continue
                action = block_to_action(block, trace.trace_id)
                actions.append(action)
        return trace.trace_id, True, actions
    except Exception as e:
        logger.error("Marking trace as failed " + trace.trace_id + " - " + str(e))
        return trace.trace_id, False, []


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--fetch-size',
                        help='Number of traces to fetch from db in one batch',
                        type=int,
                        default=10000)
    parser.add_argument('--batch-size',
                        help='Number of traces to process in one batch',
                        type=int,
                        default=1000)
    parser.add_argument('--pool-size',
                        help='Number of workers to process traces',
                        type=int,
                        default=4)
    args = parser.parse_args()
    if settings.emulated_traces:
        logger.info("Starting processing emulated traces")
        asyncio.run(start_emulated_traces_processing())
    else:
        logger.info("Starting processing events from db")
        asyncio.run(start_processing_events_from_db(args))
