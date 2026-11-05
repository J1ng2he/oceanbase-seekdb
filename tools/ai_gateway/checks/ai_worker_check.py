"""Verify the AI column worker.

The check replays the official `ai_column_worker.test`:

* the AI columns of one row share the request id of the invalidation,
* the values of the AI columns of one row become visible together,
* the prompts are typed, the type of the value is part of the request,
* a failed task is retried and the retry is visible,
* a task that can not be computed is removed and the value stays null,
* the result of an old request never overwrites the value of a newer one.

Usage:
    python tools/ai_gateway/checks/ai_worker_check.py [--port 2881] [--mock-port 19122]
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

DB = 'ai_check_worker'
MODEL = 'p0_check_complete'
ENDPOINT = 'p0_check_complete_ep'
FAIL_MODEL = 'p0_check_fail'
FAIL_ENDPOINT = 'p0_check_fail_ep'

# the table of the tasks is shared by every database of the tenant, the
# queries of the check are scoped to the database of the check
TASK_FROM = ("from oceanbase.__all_ai_column_task task "
             "join oceanbase.__all_table t on t.table_id = task.table_id "
             "join oceanbase.__all_database d on d.database_id = t.database_id "
             "where d.database_name = %s")


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


def one(cur, sql, args=()):
    cur.execute(sql, args)
    rows = cur.fetchall()
    return rows[0][0] if rows else None


def wait_for(cur, sql, expect, timeout, args=(), interval=0.1):
    """Wait until the statement returns the expected value."""
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
    """The endpoint may not exist, the error of the procedure is ignored."""
    try:
        cur.execute("call DBMS_AI_SERVICE.DROP_AI_MODEL_ENDPOINT(%s)", (name,))
    except pymysql.err.MySQLError:
        pass


def drop_model(cur, name):
    """The model may not exist, the error of the procedure is ignored."""
    try:
        cur.execute("call DBMS_AI_SERVICE.DROP_AI_MODEL(%s)", (name,))
    except pymysql.err.MySQLError:
        pass


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--port', type=int, default=2881)
    ap.add_argument('--mock-port', type=int, default=19122)
    args = ap.parse_args()

    mock('start', args.mock_port)
    c = Checker()
    conn = pymysql.connect(host='127.0.0.1', port=args.port, user='root', password='',
                           charset='utf8mb4', autocommit=True)
    try:
        with conn.cursor() as cur:
            cur.execute("drop database if exists " + DB)
            cur.execute("create database " + DB)
            for name in (ENDPOINT, FAIL_ENDPOINT):
                drop_endpoint(cur, name)
            for name in (MODEL, FAIL_MODEL):
                drop_model(cur, name)
            cur.execute("call DBMS_AI_SERVICE.CREATE_AI_MODEL(%s, %s)",
                        (MODEL, json.dumps({'type': 'completion', 'model_name': 'mock-complete'})))
            cur.execute("call DBMS_AI_SERVICE.CREATE_AI_MODEL(%s, %s)",
                        (FAIL_MODEL, json.dumps({'type': 'completion', 'model_name': 'mock-fail'})))
            cur.execute("call DBMS_AI_SERVICE.CREATE_AI_MODEL_ENDPOINT(%s, %s)",
                        (ENDPOINT, json.dumps({
                            'ai_model_name': MODEL,
                            'url': 'http://127.0.0.1:{}/v1/chat/completions'.format(args.mock_port),
                            'access_key': 'p0-check-key',
                            'provider': 'openai',
                            'request_model_name': 'mock-complete'})))
            cur.execute("call DBMS_AI_SERVICE.CREATE_AI_MODEL_ENDPOINT(%s, %s)",
                        (FAIL_ENDPOINT, json.dumps({
                            'ai_model_name': FAIL_MODEL,
                            'url': 'http://127.0.0.1:{}/v1/always-fail/completions'.format(args.mock_port),
                            'access_key': 'p0-check-key',
                            'provider': 'openai',
                            'request_model_name': 'mock-fail'})))

            # 1. the two AI columns of the row share one request id
            cur.execute(
                "create table " + DB + ".docs ("
                "id bigint primary key, "
                "content varchar(200), "
                "tags json AI COLUMN (AI_COMPLETE('" + MODEL + "', concat('Classify: ', content))) "
                "WITH (sync_mode='async', max_retries=2), "
                "score bigint AI COLUMN (AI_COMPLETE('" + MODEL + "', concat('Score: ', content))) "
                "WITH (sync_mode='async', max_retries=2))")
            cur.execute("insert into " + DB + ".docs(id, content) values(1, 'typed-prompts')")
            c.check('the AI columns are hidden at first',
                    one(cur, "select count(*) = 1 from " + DB + ".docs "
                             "where id = 1 and tags is null and score is null"), 1)
            c.check('one request id for the two AI columns',
                    one(cur, "select count(*) = 2 and count(distinct task.request_id) = 1 "
                             + TASK_FROM, (DB,)), 1)

            # 2. the values of the AI columns become visible together
            visible = wait_for(cur,
                               "select ifnull((tags is not null) + (score is not null), 0) "
                               "from " + DB + ".docs where id = 1",
                               2, 20)
            c.check('the values are visible', visible, 2)
            c.check('the value of the json column is the answer of the model',
                    one(cur, "select json_unquote(json_extract(tags, '$.input')) = 'Classify: typed-prompts' "
                             "from " + DB + ".docs where id = 1"), 1)
            c.check('the value of the integer column is the answer of the model',
                    one(cur, "select score = 7 from " + DB + ".docs where id = 1"), 1)
            c.check('the tasks are removed',
                    one(cur, "select count(*) " + TASK_FROM, (DB,)), 0)
            c.check('the prompts are typed', mock('assert', args.mock_port, 'typed-prompts'),
                    'mock_typed_prompts=ok')

            # 3. a failed task is retried and the retry is visible
            cur.execute(
                "create table " + DB + ".retry_docs ("
                "id bigint primary key, "
                "content varchar(200), "
                "result varchar(400) AI COLUMN (AI_COMPLETE('" + MODEL + "', content)) "
                "WITH (sync_mode='async', max_retries=2))")
            cur.execute("insert into " + DB + ".retry_docs(id, content) values(2, 'retry-once')")
            retry_seen = wait_for(cur,
                                  "select count(*) > 0 " + TASK_FROM
                                  + " and task.state = 3 and task.retry_count = 1",
                                  1, 20, (DB,))
            c.check('the retry of the task is visible', retry_seen, 1)
            done = wait_for(cur,
                            "select ifnull(result = 'mock:retry-once', 0) "
                            "from " + DB + ".retry_docs where id = 2", 1, 30)
            c.check('the task is done after the retry', done, 1)
            c.check('the retried task is removed',
                    one(cur, "select count(*) " + TASK_FROM
                             + " and task.rowkey_predicate = '`id` = 2'", (DB,)), 0)
            c.check('the model was called twice', mock('assert', args.mock_port, 'retry-once'),
                    'mock_retry_once=ok')

            # 4. a task that can not be computed is removed
            cur.execute(
                "create table " + DB + ".fail_docs ("
                "id bigint primary key, "
                "content varchar(200), "
                "result varchar(400) AI COLUMN (AI_COMPLETE('" + FAIL_MODEL + "', content)) "
                "WITH (sync_mode='async', max_retries=0))")
            cur.execute("insert into " + DB + ".fail_docs(id, content) values(3, 'terminal-failure')")
            gone = wait_for(cur,
                            "select count(*) " + TASK_FROM
                            + " and task.rowkey_predicate = '`id` = 3'", 0, 20, (DB,))
            c.check('the task that can not be computed is removed', gone, 0)
            c.check('the value of the AI column stays null',
                    one(cur, "select result is null from " + DB + ".fail_docs where id = 3"), 1)

            # 5. the result of an old request never overwrites a newer one
            mock('reset', args.mock_port)
            cur.execute("insert into " + DB + ".docs(id, content) values(4, 'slow-old')")
            time.sleep(0.3)
            cur.execute("update " + DB + ".docs set content = 'fast-new' where id = 4")
            latest = wait_for(cur,
                              "select ifnull(tags is not null and score is not null, 0) "
                              "from " + DB + ".docs where id = 4", 1, 30)
            c.check('the newest request wins', latest, 1)
            time.sleep(2)
            c.check('the late result does not overwrite the value',
                    one(cur, "select json_unquote(json_extract(tags, '$.input')) = 'Classify: fast-new' "
                             "and score = 7 from " + DB + ".docs where id = 4"), 1)
            c.check('the model was called for both requests',
                    mock('assert', args.mock_port, 'stale-guard'), 'mock_stale_guard=ok')

            cur.execute("drop database " + DB)
    finally:
        mock('stop', args.mock_port)
        conn.close()

    print('[RESULT] {}/{} passed'.format(c.passed, c.passed + c.failed))
    return 0 if c.failed == 0 else 1


if __name__ == '__main__':
    sys.exit(main())
