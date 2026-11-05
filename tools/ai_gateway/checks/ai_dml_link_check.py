"""Verify the AI column DML linkage.

The check covers the task creation, the invalidation, the transaction
visibility, the rollback and the unrelated update of the official
`ai_column_t1_dml.test`.  The official test relies on the worker to compute
the value of the AI column, so the check starts the mock of the model and
waits for the worker.

Usage:
    python tools/ai_gateway/checks/ai_dml_link_check.py [--port 2881] [--mock-port 19121]
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

DB = 'ai_check_dml'
MODEL = 'p0_check_dml_complete'
ENDPOINT = 'p0_check_dml_complete_ep'

TASK_SQL = ("select count(*) from oceanbase.__all_ai_column_task task "
            "join oceanbase.__all_table t on t.table_id = task.table_id "
            "join oceanbase.__all_database d on d.database_id = t.database_id "
            "where d.database_name = %s and t.table_name = %s")

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
        print('[{}] {:<44} got={!r} expect={!r}'.format(
            'PASS' if ok else 'FAIL', label, got, expect))
        return ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--port', type=int, default=2881)
    ap.add_argument('--mock-port', type=int, default=19121)
    args = ap.parse_args()

    mock('start', args.mock_port)
    c = Checker()
    conn = pymysql.connect(host='127.0.0.1', port=args.port, user='root', password='',
                           charset='utf8mb4', autocommit=False)
    try:
        with conn.cursor() as cur:
            cur.execute("drop database if exists " + DB)
            cur.execute("create database " + DB)
            drop_endpoint(cur, ENDPOINT)
            drop_model(cur, MODEL)
            cur.execute("call DBMS_AI_SERVICE.CREATE_AI_MODEL(%s, %s)",
                        (MODEL, json.dumps({'type': 'completion', 'model_name': 'mock-complete'})))
            cur.execute("call DBMS_AI_SERVICE.CREATE_AI_MODEL_ENDPOINT(%s, %s)",
                        (ENDPOINT, json.dumps({
                            'ai_model_name': MODEL,
                            'url': 'http://127.0.0.1:{}/v1/chat/completions'.format(args.mock_port),
                            'access_key': 'p0-check-key',
                            'provider': 'openai',
                            'request_model_name': 'mock-complete'})))
            conn.commit()
            cur.execute("create table " + DB + ".docs ("
                        "id bigint primary key, "
                        "title varchar(100), "
                        "content varchar(200), "
                        "tags json AI COLUMN (AI_COMPLETE('" + MODEL + "', concat('Classify: ', content))) "
                        "WITH (sync_mode='async', max_retries=2))")
            conn.commit()

            # 1. the insert creates one task and hides the result
            cur.execute("insert into " + DB + ".docs(id, title, content) values(1, 't1', 'initial')")
            c.check('insert hides the AI result',
                    one(cur, "select tags is null from " + DB + ".docs where id = 1"), 1)
            c.check('insert creates one task',
                    one(cur, TASK_SQL, (DB, 'docs')), 1)
            c.check('the task is eligible immediately',
                    one(cur, "select count(*) from oceanbase.__all_ai_column_task task "
                             "join oceanbase.__all_table t on t.table_id = task.table_id "
                             "join oceanbase.__all_database d on d.database_id = t.database_id "
                             "where d.database_name = %s and t.table_name = 'docs' "
                             "and task.eligible_ts <= now(6)", (DB,)), 1)
            c.check('the task uses the row key predicate',
                    one(cur, "select count(*) from oceanbase.__all_ai_column_task task "
                             "join oceanbase.__all_table t on t.table_id = task.table_id "
                             "join oceanbase.__all_database d on d.database_id = t.database_id "
                             "where d.database_name = %s and t.table_name = 'docs' "
                             "and task.rowkey_predicate = '`id` = 1'", (DB,)), 1)
            c.check('the task is pending',
                    one(cur, "select count(*) from oceanbase.__all_ai_column_task task "
                             "join oceanbase.__all_table t on t.table_id = task.table_id "
                             "join oceanbase.__all_database d on d.database_id = t.database_id "
                             "where d.database_name = %s and t.table_name = 'docs' "
                             "and task.state = 0 and task.retry_count = 0", (DB,)), 1)
            conn.commit()
            # the worker computes the value of the AI column
            c.check('the worker computes the value',
                    wait_for(cur, "select tags is not null from " + DB + ".docs where id = 1", 1, 30), 1)
            c.check('the worker drains the task',
                    wait_for(cur, "select count(*) " + TASK_FROM, 0, 30, (DB,)), 0)

            # 2. an unrelated update keeps the result and creates no task
            mock('reset', args.mock_port)
            cur.execute("update " + DB + ".docs set title = 't2' where id = 1")
            c.check('the unrelated update creates no task',
                    one(cur, TASK_SQL, (DB, 'docs')), 0)
            conn.commit()
            time.sleep(1)
            c.check('the unrelated update keeps the result',
                    one(cur, "select tags is not null from " + DB + ".docs where id = 1"), 1)
            c.check('the model was not called for the unrelated update',
                    mock('assert', args.mock_port, 'no-requests'), 'mock_no_requests=ok')

            # 3. a dependency update invalidates the result and creates a task
            cur.execute("update " + DB + ".docs set content = 'changed' where id = 1")
            c.check('the dependency update hides the result',
                    one(cur, "select tags is null from " + DB + ".docs where id = 1"), 1)
            c.check('the dependency update creates one task',
                    one(cur, TASK_SQL, (DB, 'docs')), 1)
            conn.commit()
            c.check('the worker computes the value again',
                    wait_for(cur, "select tags is not null from " + DB + ".docs where id = 1", 1, 30), 1)

            # 4. the rollback restores the row and removes the task
            mock('reset', args.mock_port)
            cur.execute("update " + DB + ".docs set content = 'rolled-back' where id = 1")
            c.check('the transaction sees the invalidation',
                    one(cur, "select content = 'rolled-back' and tags is null "
                             "from " + DB + ".docs where id = 1"), 1)
            c.check('the transaction sees the task',
                    one(cur, TASK_SQL, (DB, 'docs')), 1)
            conn.rollback()
            c.check('the rollback restores the content',
                    one(cur, "select content from " + DB + ".docs where id = 1"), 'changed')
            c.check('the rollback restores the result',
                    one(cur, "select tags is not null from " + DB + ".docs where id = 1"), 1)
            c.check('the rollback removes the task',
                    one(cur, TASK_SQL, (DB, 'docs')), 0)
            time.sleep(2)
            c.check('the model was not called for the rolled back value',
                    mock('assert', args.mock_port, 'no-rollback-value'), 'mock_no_rollback_value=ok')

            # 5. the primary key update uses the new primary key
            cur.execute("update " + DB + ".docs set id = 2 where id = 1")
            c.check('the old primary key is gone',
                    one(cur, "select count(*) from " + DB + ".docs where id = 1"), 0)
            c.check('the new primary key is invalidated',
                    one(cur, "select tags is null from " + DB + ".docs where id = 2"), 1)
            c.check('the task uses the new primary key',
                    one(cur, "select count(*) " + TASK_FROM
                             + " and task.rowkey_predicate = '`id` = 2'", (DB,)), 1)
            c.check('the task of the old primary key is removed',
                    one(cur, "select count(*) " + TASK_FROM
                             + " and task.rowkey_predicate = '`id` = 1'", (DB,)), 0)
            conn.commit()
            c.check('the worker computes the value of the new row',
                    wait_for(cur, "select tags is not null from " + DB + ".docs where id = 2", 1, 30), 1)

            # 6. a batch insert creates one task per row
            cur.execute("insert into " + DB + ".docs(id, title, content) "
                        "values(10, 'b10', 'b10'), (11, 'b11', 'b11')")
            c.check('the batch insert invalidates two rows',
                    one(cur, "select count(*) from " + DB + ".docs "
                             "where id in (10, 11) and tags is null"), 2)
            c.check('the batch insert creates two tasks',
                    one(cur, "select count(*) " + TASK_FROM
                             + " and task.rowkey_predicate in ('`id` = 10', '`id` = 11')",
                             (DB,)), 2)
            # 7. the delete removes the pending tasks
            cur.execute("delete from " + DB + ".docs where id in (10, 11)")
            c.check('the delete cleans the pending tasks',
                    one(cur, "select count(*) " + TASK_FROM
                             + " and task.rowkey_predicate in ('`id` = 10', '`id` = 11')",
                             (DB,)), 0)
            conn.commit()
            c.check('no task is left after the delete',
                    one(cur, TASK_SQL, (DB, 'docs')), 0)

            # 8. the delete of the last row leaves no task
            cur.execute("delete from " + DB + ".docs where id = 2")
            conn.commit()
            c.check('the delete removes the row',
                    one(cur, "select count(*) from " + DB + ".docs"), 0)
            c.check('the delete leaves no task',
                    one(cur, TASK_SQL, (DB, 'docs')), 0)

            cur.execute("drop database " + DB)
            conn.commit()
    finally:
        mock('stop', args.mock_port)
        conn.close()

    print('[RESULT] {}/{} passed'.format(c.passed, c.passed + c.failed))
    return 0 if c.failed == 0 else 1


if __name__ == '__main__':
    sys.exit(main())
