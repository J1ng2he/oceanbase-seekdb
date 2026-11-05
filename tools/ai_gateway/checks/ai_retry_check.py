"""Verify the retry of an AI column and the strict read of an AI column.

The check replays the official `ai_column_retry_strict.test`:

* the rows that are selected by the predicate of the retry are invalidated,
  the other AI columns of the table are not touched,
* the pseudo column `status` is the state of the AI column that is retried, a
  quoted name is a physical column of the table,
* the strict read only checks the AI columns that the statement refers to,
  the check is done for the whole column and not only for the rows of the
  statement,
* a statement that is rejected by the strict read does not change any row,
* the retry of a column that is not an AI column is not supported, and the
  retry can not be combined with another change of the table.

Usage:
    python tools/ai_gateway/checks/ai_retry_check.py [--port 2881] [--mock-port 19124]
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

DB = 'ai_check_retry'
MODEL = 'p0_check_retry_model'
ENDPOINT = 'p0_check_retry_ep'

ERR_UNFINISHED_TASKS = 11120
ERR_NOT_SUPPORTED = 1235


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
        print('[{}] {:<52} got={!r} expect={!r}'.format(
            'PASS' if ok else 'FAIL', label, got, expect))
        return ok

    def check_error(self, label, cur, sql, expect_errno, args=()):
        try:
            cur.execute(sql, args)
            got = None
        except pymysql.err.MySQLError as exc:
            got = exc.args[0]
        return self.check(label, got, expect_errno)


def one(cur, sql, args=()):
    cur.execute(sql, args)
    rows = cur.fetchall()
    return rows[0][0] if rows else None


def rows(cur, sql, args=()):
    cur.execute(sql, args)
    return cur.fetchall()


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
    ap.add_argument('--mock-port', type=int, default=19124)
    args = ap.parse_args()

    mock('start', args.mock_port)
    c = Checker()
    conn = pymysql.connect(host='127.0.0.1', port=args.port, user='root', password='',
                           charset='utf8mb4', autocommit=True)
    table = DB + '.docs'
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
                            'access_key': 'retry-check-key',
                            'provider': 'openai',
                            'request_model_name': 'mock-complete'})))

            cur.execute(
                "create table " + table + " ("
                "id bigint primary key, "
                "category varchar(20), "
                "status varchar(20), "
                "content varchar(200), "
                "result varchar(400) AI COLUMN (AI_COMPLETE('" + MODEL + "', content)) "
                "WITH (sync_mode='async', max_retries=0), "
                "label varchar(400) AI COLUMN (AI_COMPLETE('" + MODEL + "', concat('Label: ', content))) "
                "WITH (sync_mode='async', max_retries=0))")
            cur.execute("insert into " + table + "(id, category, status, content) values "
                        "(1, 'news', 'active', 'row-one'), "
                        "(2, 'sports', 'active', 'row-two'), "
                        "(3, 'news', 'inactive', 'row-three')")

            ready = wait_for(cur,
                             "select count(*) = 3 from " + table +
                             " where result is not null and label is not null", 1, 30)
            c.check('the values of the AI columns are ready', ready, 1)
            c.check('the strict read is off by default',
                    one(cur, "select @@ai_column_strict_read"), 0)

            # 1. the retry invalidates the rows of the predicate only
            mock('stop', args.mock_port)
            cur.execute("alter table " + table + " retry ai column result where id in (1, 3)")
            cur.execute("set session ai_column_strict_read = off")
            c.check('the rows of the predicate are invalidated',
                    rows(cur, "select id, result is null, label is not null from " + table +
                         " order by id"),
                    ((1, 1, 1), (2, 0, 1), (3, 1, 1)))

            # 2. the strict read only checks the AI columns of the statement
            cur.execute("set session ai_column_strict_read = on")
            c.check('a statement without an AI column is allowed',
                    rows(cur, "select id, category from " + table + " order by id"),
                    ((1, 'news'), (2, 'sports'), (3, 'news')))
            c.check('a ready AI column is readable',
                    one(cur, "select count(*) = 3 from " + table + " where label is not null"), 1)
            c.check_error('the AI column of the statement is checked',
                          cur, "select result from " + table + " where id = 2",
                          ERR_UNFINISHED_TASKS)
            c.check_error('the AI column of the select list is checked',
                          cur, "select id, result from " + table + " order by id",
                          ERR_UNFINISHED_TASKS)
            c.check_error('the AI column of the where clause is checked',
                          cur, "select id from " + table + " where result is null",
                          ERR_UNFINISHED_TASKS)
            c.check_error('the AI column of a sub query is checked',
                          cur, "select id from " + table + " where id in "
                               "(select id from " + table + " where result is null)",
                          ERR_UNFINISHED_TASKS)
            c.check_error('the update of the rows is rejected',
                          cur, "update " + table + " set category = category where result is null",
                          ERR_UNFINISHED_TASKS)
            c.check_error('the delete of the rows is rejected',
                          cur, "delete from " + table + " where result is null",
                          ERR_UNFINISHED_TASKS)
            cur.execute("set session ai_column_strict_read = off")
            c.check('the rejected statements did not change any row',
                    one(cur, "select count(*) = 3 from " + table), 1)

            # 3. the tasks of the failed rows are cleaned, the column is still
            #    not ready
            gone = wait_for(cur,
                            "select count(*) from oceanbase.__all_ai_column_task task "
                            "join oceanbase.__all_table t on t.table_id = task.table_id "
                            "join oceanbase.__all_database d on d.database_id = t.database_id "
                            "where d.database_name = '" + DB + "' and t.table_name = 'docs'",
                            0, 30)
            c.check('the tasks of the failed rows are cleaned', gone, 0)
            cur.execute("set session ai_column_strict_read = on")
            c.check_error('the column is not ready after the failure',
                          cur, "select result from " + table + " where id = 1",
                          ERR_UNFINISHED_TASKS)
            cur.execute("set session ai_column_strict_read = off")

            # 4. the pseudo column status selects the rows of the state
            cur.execute("alter table " + table +
                        " retry ai column result where status = 'FAILED' and category = 'news'")
            c.check('the pseudo column status selects the failed rows',
                    rows(cur, "select id, result is null from " + table + " order by id"),
                    ((1, 1), (2, 0), (3, 1)))

            # 5. the retry of all the rows makes the column ready again
            mock('start', args.mock_port)
            cur.execute("alter table " + table + " retry ai column result")
            ready = wait_for(cur,
                             "select count(*) = 3 from " + table + " where result is not null",
                             1, 30)
            c.check('the retry of all the rows is done', ready, 1)
            cur.execute("set session ai_column_strict_read = on")
            c.check('the strict read succeeds when all the rows are ready',
                    one(cur, "select count(*) = 3 from " + table + " where result is not null"), 1)
            cur.execute("set session ai_column_strict_read = off")

            # 6. a predicate that matches no row does nothing
            mock('stop', args.mock_port)
            cur.execute("alter table " + table +
                        " retry ai column result where status = 'READY' and id = 999")
            c.check('a predicate that matches no row keeps the values',
                    one(cur, "select count(*) = 3 from " + table + " where result is not null"), 1)
            c.check('a predicate that matches no row creates no task',
                    one(cur, "select count(*) from oceanbase.__all_ai_column_task task "
                             "join oceanbase.__all_table t on t.table_id = task.table_id "
                             "join oceanbase.__all_database d on d.database_id = t.database_id "
                             "where d.database_name = '" + DB + "' and t.table_name = 'docs'"), 0)

            # 7. the pseudo column status selects the rows of the state
            cur.execute("alter table " + table +
                        " retry ai column result where status = 'READY' and category = 'sports'")
            c.check('the pseudo column status selects the ready rows',
                    rows(cur, "select id, result is null from " + table + " order by id"),
                    ((1, 0), (2, 1), (3, 0)))

            # 8. a quoted name is a physical column of the table
            mock('start', args.mock_port)
            cur.execute("alter table " + table + " retry ai column result")
            ready = wait_for(cur,
                             "select count(*) = 3 from " + table + " where result is not null",
                             1, 30)
            c.check('the retry of all the rows is done again', ready, 1)
            mock('stop', args.mock_port)
            cur.execute("alter table " + table +
                        " retry ai column result where `status` = 'active' and id = 1")
            c.check('a quoted name is a physical column',
                    rows(cur, "select id, result is null from " + table + " order by id"),
                    ((1, 1), (2, 0), (3, 0)))

            # 9. the retry of a column that is not an AI column is refused
            c.check_error('the retry of a column that is not an AI column is refused',
                          cur, "alter table " + table + " retry ai column content where id = 1",
                          ERR_NOT_SUPPORTED)
            c.check_error('the retry can not be combined with another change',
                          cur, "alter table " + table +
                               " retry ai column result where id = 1, add column extra_col int",
                          ERR_NOT_SUPPORTED)

            cur.execute("drop database " + DB)
    finally:
        mock('stop', args.mock_port)
        conn.close()

    print('[RESULT] {}/{} passed'.format(c.passed, c.passed + c.failed))
    return 0 if c.failed == 0 else 1


if __name__ == '__main__':
    sys.exit(main())
