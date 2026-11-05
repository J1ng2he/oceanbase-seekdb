"""Verify the AI vector column.

The check replays the official `ai_column_vector_index.test`:

* the worker computes the embedding of the rows,
* the vector index sees the computed vectors,
* an invalidation removes the old vector from the index,
* the regenerated vector is visible to the index,
* the rollback restores the vector and the index.

Usage:
    python tools/ai_gateway/checks/ai_vector_check.py [--port 2881] [--mock-port 19123]
"""
import argparse
import json
import os
import subprocess
import sys
import time

import pymysql

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
# The mock of the model.  It sits next to the gateway, the tools directory of
# the workspace is accepted as well.
MOCK_CANDIDATES = [
    os.path.join(ROOT, 'ai_column_mock.py'),
    os.path.join(HERE, 'ai_column_mock.py'),
    os.path.join(ROOT, 'tools', 'ai_column_mock.py'),
]
MOCK = next((path for path in MOCK_CANDIDATES if os.path.exists(path)),
            MOCK_CANDIDATES[0])

DB = 'ai_check_vector'
MODEL = 'p0_check_vector_embed'
ENDPOINT = 'p0_check_vector_embed_ep'

# the table of the tasks is shared by every database of the tenant, the
# queries of the check are scoped to the database of the check
TASK_FROM = ("from oceanbase.__all_ai_column_task task "
             "join oceanbase.__all_table t on t.table_id = task.table_id "
             "join oceanbase.__all_database d on d.database_id = t.database_id "
             "where d.database_name = %s")


def one(cur, sql, args=()):
    cur.execute(sql, args)
    rows = cur.fetchall()
    return rows[0][0] if rows else None


def wait_for(cur, sql, expect, timeout, args=(), interval=0.1):
    deadline = time.time() + timeout
    value = None
    while time.time() < deadline:
        value = one(cur, sql, args)
        if value == expect:
            return value
        time.sleep(interval)
    return value


def mock(mode, mock_port, assertion=None):
    args = [sys.executable, MOCK, mode, str(mock_port)]
    if assertion:
        args.append(assertion)
    proc = subprocess.run(args, capture_output=True, text=True)
    if proc.returncode != 0:
        raise RuntimeError('the mock failed: {}\n{}'.format(proc.stdout, proc.stderr))
    return proc.stdout.strip()


def drop_endpoint(cur, name):
    try:
        cur.execute("call DBMS_AI_SERVICE.DROP_AI_MODEL_ENDPOINT(%s)", (name,))
    except pymysql.err.MySQLError:
        pass


def drop_model(cur, name):
    try:
        cur.execute("call DBMS_AI_SERVICE.DROP_AI_MODEL(%s)", (name,))
    except pymysql.err.MySQLError:
        pass


class Checker:
    def __init__(self):
        self.passed = 0
        self.failed = 0

    def check(self, label, got, expect):
        ok = (got == expect)
        if ok:
            self.passed += 1
        else:
            self.failed += 1
        print('[{}] {:<46} got={!r} expect={!r}'.format(
            'PASS' if ok else 'FAIL', label, got, expect))
        return ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--port', type=int, default=2881)
    ap.add_argument('--mock-port', type=int, default=19123)
    args = ap.parse_args()

    mock('start', args.mock_port)
    c = Checker()
    conn = pymysql.connect(host='127.0.0.1', port=args.port, user='root', password='',
                           charset='utf8mb4', autocommit=False)
    try:
        with conn.cursor() as cur:
            cur.execute("drop database if exists " + DB)
            cur.execute("create database " + DB)
            conn.commit()
            drop_endpoint(cur, ENDPOINT)
            drop_model(cur, MODEL)
            cur.execute("call DBMS_AI_SERVICE.CREATE_AI_MODEL(%s, %s)",
                        (MODEL, json.dumps({'type': 'dense_embedding', 'model_name': 'mock-embed'})))
            cur.execute("call DBMS_AI_SERVICE.CREATE_AI_MODEL_ENDPOINT(%s, %s)",
                        (ENDPOINT, json.dumps({
                            'ai_model_name': MODEL,
                            'url': 'http://127.0.0.1:{}/v1/embeddings'.format(args.mock_port),
                            'access_key': 'p0-vector-key',
                            'provider': 'openai',
                            'request_model_name': 'mock-embed'})))
            conn.commit()

            cur.execute("create table " + DB + ".docs ("
                        "id bigint primary key, "
                        "content varchar(200), "
                        "emb vector(4) AI COLUMN (AI_EMBED('" + MODEL + "', content)) "
                        "WITH (sync_mode='async', max_retries=2))")
            cur.execute("insert into " + DB + ".docs(id, content) "
                        "values(1, 'vec-zero'), (2, 'vec-ten'), (3, 'vec-twenty')")
            conn.commit()

            # 1. the worker computes the embeddings
            ready = wait_for(cur,
                             "select count(*) = 3 from " + DB + ".docs where emb is not null",
                             1, 30)
            c.check('the embeddings are ready', ready, 1)
            c.check('the first embedding is the answer of the model',
                    one(cur, "select emb = [0,0,0,0] from " + DB + ".docs where id = 1"), 1)
            c.check('the second embedding is the answer of the model',
                    one(cur, "select emb = [10,0,0,0] from " + DB + ".docs where id = 2"), 1)
            c.check('the third embedding is the answer of the model',
                    one(cur, "select emb = [20,0,0,0] from " + DB + ".docs where id = 3"), 1)
            c.check('the tasks are removed',
                    one(cur, "select count(*) " + TASK_FROM, (DB,)), 0)
            c.check('the embedding service was called',
                    mock('assert', args.mock_port, 'embedding'), 'mock_embedding=ok')

            # 2. the vector index sees the computed vectors
            cur.execute("create vector index idx_p0_ai_emb on " + DB + ".docs(emb) "
                        "with(distance=l2, type=hnsw, lib=vsag, m=10, "
                        "ef_construction=12, ef_search=40)")
            c.check('the index returns the three rows',
                    one(cur, "select count(*) from (select id from " + DB + ".docs "
                             "order by l2_distance(emb, [0,0,0,0]) approximate limit 3) ann"), 3)

            # 3. an invalidation removes the old vector from the index
            mock('reset', args.mock_port)
            cur.execute("update " + DB + ".docs set content = 'vec-zero' where id = 2")
            c.check('the vector is hidden during the regeneration',
                    one(cur, "select content = 'vec-zero' and emb is null from "
                             + DB + ".docs where id = 2"), 1)
            c.check('the update creates one task',
                    one(cur, "select count(*) " + TASK_FROM
                             + " and task.rowkey_predicate = '`id` = 2'", (DB,)), 1)
            c.check('the old vector is removed from the index',
                    one(cur, "select count(*) = 2 and sum(id = 2) = 0 from ("
                             "select id from " + DB + ".docs "
                             "order by l2_distance(emb, [0,0,0,0]) approximate limit 3) ann"), 1)
            conn.commit()
            ready = wait_for(cur,
                             "select emb = [0,0,0,0] from " + DB + ".docs where id = 2",
                             1, 30)
            c.check('the vector is regenerated', ready, 1)
            c.check('the task of the regenerated vector is removed',
                    one(cur, "select count(*) " + TASK_FROM, (DB,)), 0)
            c.check('the regenerated vector is visible to the index',
                    one(cur, "select count(*) = 2 and sum(id = 1) = 1 and sum(id = 2) = 1 from ("
                             "select id from " + DB + ".docs "
                             "order by l2_distance(emb, [0,0,0,0]) approximate limit 2) ann"), 1)

            # 4. the rollback restores the vector and the index
            mock('reset', args.mock_port)
            cur.execute("update " + DB + ".docs set content = 'vec-ten' where id = 3")
            c.check('the transaction sees the invalidation',
                    one(cur, "select content = 'vec-ten' and emb is null from "
                             + DB + ".docs where id = 3"), 1)
            c.check('the transaction sees the task',
                    one(cur, "select count(*) " + TASK_FROM
                             + " and task.rowkey_predicate = '`id` = 3'", (DB,)), 1)
            conn.rollback()
            c.check('the rollback restores the vector',
                    one(cur, "select content = 'vec-twenty' and emb = [20,0,0,0] from "
                             + DB + ".docs where id = 3"), 1)
            c.check('the rollback removes the task',
                    one(cur, "select count(*) " + TASK_FROM, (DB,)), 0)
            time.sleep(2)
            c.check('the model was not called for the rolled back value',
                    mock('assert', args.mock_port, 'no-requests'), 'mock_no_requests=ok')
            c.check('the restored vector is visible to the index',
                    one(cur, "select count(*) = 1 and sum(id = 3) = 1 from ("
                             "select id from " + DB + ".docs "
                             "order by l2_distance(emb, [20,0,0,0]) approximate limit 1) ann"), 1)

            cur.execute("drop database " + DB)
            conn.commit()
    finally:
        mock('stop', args.mock_port)
        conn.close()

    print('[RESULT] {}/{} passed'.format(c.passed, c.passed + c.failed))
    return 0 if c.failed == 0 else 1


if __name__ == '__main__':
    sys.exit(main())
