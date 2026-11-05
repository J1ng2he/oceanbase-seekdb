"""Local acceptance check for the AI column DDL layer.

It replays the metadata assertions of the official
`ai_column_ddl.test` / `ai_column_ddl.result` pair against the running
observer and prints one PASS/FAIL line per assertion.

Usage: python tools/ai_gateway/checks/ai_column_ddl_check.py [--port 2881]
"""
import argparse
import sys

import pymysql


def run(cur, sql):
    cur.execute(sql)
    if cur.description:
        return cur.fetchall()
    return []


def one(cur, sql):
    rows = run(cur, sql)
    if not rows:
        return None
    return rows[0][0]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--port', type=int, default=2881)
    ap.add_argument('--user', default='root')
    ap.add_argument('--password', default='')
    args = ap.parse_args()

    conn = pymysql.connect(host='127.0.0.1', port=args.port, user=args.user,
                           password=args.password, database='test',
                           charset='utf8mb4', autocommit=True)
    failures = []
    try:
        with conn.cursor() as cur:
            run(cur, 'drop database if exists ai_column_p0_ddl')
            run(cur, 'create database ai_column_p0_ddl')
            # the leftovers of an earlier run must not break the check
            for stmt in (
                "call DBMS_AI_SERVICE.DROP_AI_MODEL_ENDPOINT('p0_ddl_complete_ep')",
                "call DBMS_AI_SERVICE.DROP_AI_MODEL_ENDPOINT('p0_ddl_embed_ep')",
                "call DBMS_AI_SERVICE.DROP_AI_MODEL('p0_ddl_complete')",
                "call DBMS_AI_SERVICE.DROP_AI_MODEL('p0_ddl_embed')",
            ):
                try:
                    run(cur, stmt)
                except Exception:  # noqa: BLE001
                    pass
            run(cur, "call DBMS_AI_SERVICE.CREATE_AI_MODEL('p0_ddl_complete',"
                     " '{\"type\":\"completion\",\"model_name\":\"mock-complete\"}')")
            run(cur, "call DBMS_AI_SERVICE.CREATE_AI_MODEL('p0_ddl_embed',"
                     " '{\"type\":\"dense_embedding\",\"model_name\":\"mock-embed\"}')")
            run(cur, "call DBMS_AI_SERVICE.CREATE_AI_MODEL_ENDPOINT('p0_ddl_complete_ep',"
                     " '{\"ai_model_name\":\"p0_ddl_complete\","
                     "\"url\":\"http://127.0.0.1:19120/v1/chat/completions\","
                     "\"access_key\":\"p0-key\",\"provider\":\"openai\","
                     "\"request_model_name\":\"mock-complete\"}')")
            run(cur, "call DBMS_AI_SERVICE.CREATE_AI_MODEL_ENDPOINT('p0_ddl_embed_ep',"
                     " '{\"ai_model_name\":\"p0_ddl_embed\","
                     "\"url\":\"http://127.0.0.1:19120/v1/embeddings\","
                     "\"access_key\":\"p0-key\",\"provider\":\"openai\","
                     "\"request_model_name\":\"mock-embed\"}')")

            run(cur, """create table ai_column_p0_ddl.docs (
  id bigint primary key,
  content varchar(200),
  tags json AI COLUMN (
    AI_COMPLETE('p0_ddl_complete', concat('Classify: ', content))
  ) WITH (sync_mode='async', max_retries=2),
  emb vector(4) AI COLUMN (
    AI_EMBED('p0_ddl_embed', content)
  ) WITH (sync_mode='async', max_retries=3)
)""")

            base = ("from oceanbase.__all_ai_column a "
                    "join oceanbase.__all_table t on t.table_id = a.table_id "
                    "join oceanbase.__all_database d on d.database_id = t.database_id "
                    "where d.database_name = 'ai_column_p0_ddl' and t.table_name = 'docs'")

            checks = [
                ('ai_metadata_rows',
                 "select count(*) = 2 " + base, True),
                ('three_hidden_columns_per_ai_column',
                 "select count(*) = 6 from oceanbase.__all_ai_column a "
                 "join oceanbase.__all_table t on t.table_id = a.table_id "
                 "join oceanbase.__all_database d on d.database_id = t.database_id "
                 "join oceanbase.__all_column c on c.table_id = a.table_id "
                 "and c.column_id in (a.state_column_id, a.request_column_id, "
                 "a.result_spec_column_id) "
                 "where d.database_name = 'ai_column_p0_ddl' and t.table_name = 'docs' "
                 "and c.is_hidden = 1", True),
                ('result_columns_are_user_visible',
                 "select count(*) = 2 from oceanbase.__all_ai_column a "
                 "join oceanbase.__all_table t on t.table_id = a.table_id "
                 "join oceanbase.__all_database d on d.database_id = t.database_id "
                 "join oceanbase.__all_column c on c.table_id = a.table_id "
                 "and c.column_id = a.column_id "
                 "where d.database_name = 'ai_column_p0_ddl' and t.table_name = 'docs' "
                 "and c.is_hidden = 0", True),
                ('json_type_prompt_persisted',
                 "select count(*) = 1 from oceanbase.__all_ai_column a "
                 "join oceanbase.__all_table t on t.table_id = a.table_id "
                 "join oceanbase.__all_database d on d.database_id = t.database_id "
                 "where d.database_name = 'ai_column_p0_ddl' and t.table_name = 'docs' "
                 "and a.column_name = 'tags' "
                 "and a.normalized_expr like '%Return exactly one valid JSON value%'", True),
            ]
            for name, sql, expected in checks:
                value = one(cur, sql)
                ok = (value == 1) if expected else (value == 0)
                print('[%s] %s -> %r' % ('PASS' if ok else 'FAIL', name, value))
                if not ok:
                    failures.append(name)

            rows = run(cur, "select a.column_name, a.max_retries "
                        + base + " order by a.column_name")
            print('    ai metadata rows: %r' % (rows,))
            if rows and rows[0] == ('emb', 3) and rows[1] == ('tags', 2):
                print('[PASS] max_retries_per_column')
            else:
                print('[FAIL] max_retries_per_column')
                failures.append('max_retries_per_column')

            run(cur, "create table ai_column_p0_ddl.alter_docs("
                     "id bigint primary key, content varchar(200))")
            run(cur, "alter table ai_column_p0_ddl.alter_docs add column tags json "
                     "AI COLUMN (AI_COMPLETE('p0_ddl_complete', content)) "
                     "WITH (sync_mode='async', max_retries=1)")
            value = one(cur, "select count(*) = 1 from oceanbase.__all_ai_column a "
                        "join oceanbase.__all_table t on t.table_id = a.table_id "
                        "join oceanbase.__all_database d on d.database_id = t.database_id "
                        "where d.database_name = 'ai_column_p0_ddl' "
                        "and t.table_name = 'alter_docs'")
            print('[%s] alter_add_metadata -> %r'
                  % ('PASS' if value == 1 else 'FAIL', value))
            if value != 1:
                failures.append('alter_add_metadata')

            for name, sql, expect, expect_errno in [
                ('bad_dimension',
                 "create table ai_column_p0_ddl.bad_dimension("
                 "content varchar(20), "
                 "emb vector(4) AI COLUMN (AI_EMBED('p0_ddl_embed', content, 3)))",
                 'inconsistent dimension: expected 4 got 3', 7600),
                ('bad_dependency',
                 "create table ai_column_p0_ddl.bad_dependency("
                 "content varchar(20), "
                 "first_ai varchar(200) AI COLUMN (AI_COMPLETE('p0_ddl_complete', content)), "
                 "second_ai varchar(200) AI COLUMN (AI_COMPLETE('p0_ddl_complete', first_ai)))",
                 'Not supported feature or function', 1235),
            ]:
                try:
                    run(cur, sql)
                    print('[FAIL] %s -> no error raised' % name)
                    failures.append(name)
                except Exception as exc:  # noqa: BLE001
                    text = str(exc)
                    errno = exc.args[0] if exc.args else None
                    ok = expect in text and errno == expect_errno
                    print('[%s] %s -> errno=%r %s'
                          % ('PASS' if ok else 'FAIL', name, errno, text.strip()))
                    if not ok:
                        failures.append(name)

            run(cur, 'drop table if exists ai_column_p0_ddl.docs')
            run(cur, 'drop table if exists ai_column_p0_ddl.alter_docs')
            run(cur, 'drop database if exists ai_column_p0_ddl')
    finally:
        conn.close()

    if failures:
        print('\n[FAIL] %d check(s) failed: %s' % (len(failures), ', '.join(failures)))
        return 1
    print('\n[PASS] all AI column DDL checks passed')
    return 0


if __name__ == '__main__':
    sys.exit(main())
