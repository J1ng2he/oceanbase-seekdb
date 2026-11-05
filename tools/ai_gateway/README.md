# 评测网关（AI 列）

赛题三「AI 列」的本地评测网关。它站在数据库与模型之间，负责三件事：

1. **提供模型端点** —— 数据库侧的 AI 列任务通过 HTTP 调用这个服务拿到推理结果；
2. **记录与统计** —— 记录每次请求（提示词、凭据、token 数），可以统计评测的调用量与费用；
3. **汇总验收** —— 一条命令跑完全部 5 个验收脚本并给出汇总。

网关只是评测工具，**不在数据库的事务边界内**：它不参与 AI 列任务的调度、提交与重试，
数据库侧的调度完全由内核的后台任务机制负责。

## 目录

| 文件 | 说明 |
| --- | --- |
| `gateway.py` | 网关本体，`serve` / `stats` / `verify` 三个子命令 |
| `test_gateway.py` | 网关单测，用假上游服务，不需要真实模型的凭据 |
| `ai_column_mock.py` | 官方 mock 的可移植副本，协议与官方 `include/ai_column_mock.py` 一致 |
| `checks/` | 5 个验收脚本，复刻官方 mysqltest 用例的语义 |
| `state/` | 运行时状态（请求流水 `events.jsonl`、统计 `stats.json`），不纳入版本控制 |

## 用法

### serve：起推理服务

```powershell
python tools/ai_gateway/gateway.py serve --port 19130
```

不带 `--upstream` 时，服务返回**确定性**答案（同样的输入永远得到同样的输出），
所以 AI 列的结果与测试的执行时刻无关。答案是按提示词里的类型提示生成的：

| 提示词包含 | 返回 |
| --- | --- |
| `base-10 integer` | `7` |
| `valid JSON value` | `{"category": "mock", "input": "<用户输入>"}` |
| 其它 | `mock:<用户输入>` |

embedding 请求按输入内容返回固定向量（`vec-zero` / `vec-ten` / `vec-twenty` 有专门取值，
默认 `[1.0, 0.25, 0.5, 0.75]`）。

把数据库指到这个端点：

```sql
CALL DBMS_AI_SERVICE.CREATE_AI_MODEL_ENDPOINT('gw_ep',
  '{"ai_model_name":"gw_model","url":"http://127.0.0.1:19130/v1/chat/completions",
    "access_key":"gw-key","provider":"openai","request_model_name":"mock-complete"}');
```

### serve：透明代理与故障注入

```powershell
# 转发到真实模型，原样返回上游的答案
python tools/ai_gateway/gateway.py serve --port 19130 --upstream http://127.0.0.1:8000

# 给每个请求加 400ms 延迟
python tools/ai_gateway/gateway.py serve --port 19130 --delay-ms 400

# 让 30% 的请求返回 500（故障发生在转发之前，上游看不到这些请求）
python tools/ai_gateway/gateway.py serve --port 19130 --upstream http://127.0.0.1:8000 --fail-rate 0.3

# 模型已经回答，但答案在回程丢失（客户端超时）
python tools/ai_gateway/gateway.py serve --port 19130 --upstream http://127.0.0.1:8000 --drop-response
```

| 参数 | 含义 |
| --- | --- |
| `--upstream` | 上游模型地址；给了就是透明代理，不给就是确定性服务 |
| `--delay-ms` | 每个请求额外增加的延迟（毫秒） |
| `--fail-rate` | 失败的请求占比（0~1）；失败发生在转发**之前**，上游看不到这些请求 |
| `--drop-response` | 答案在回程丢失：请求已经到达上游，但连接被直接关闭，客户端只能超时 |
| `--price-per-1k` | 千 token 单价，用于估算评测费用 |
| `--seed` | 故障注入的随机种子，保证可复现 |

> `--fail-rate` 与 `--drop-response` 覆盖的是两种不同的故障：前者是「模型直接报错」，
> 后者是「模型答了但没收到」，AI 列的重试与超时路径对这两种故障的处置不同。

### stats：用量与费用

```powershell
python tools/ai_gateway/gateway.py stats --port 19130
```

输出示例：

```json
{
 "requests": 12,
 "prompt_tokens": 340,
 "answer_tokens": 57,
 "total_tokens": 397,
 "failures": 1,
 "dropped": 0,
 "price_per_1k": 2.0,
 "cost": 0.000794
}
```

token 数是估算值（按字符数 ÷ 4），只用于衡量评测的调用规模，不做计费。

### verify：汇总验收

```powershell
python tools/ai_gateway/gateway.py verify --port 2881
```

依次跑 `checks/` 下的 5 个验收脚本并打印汇总：

```
[gateway] the summary of the evaluation
  ddl      PASS
  dml      PASS   [RESULT] 30/30 passed
  worker   PASS   [RESULT] 16/16 passed
  vector   PASS   [RESULT] 19/19 passed
  retry    PASS   [RESULT] 24/24 passed
[RESULT] 5/5 checks passed
```

这 5 个脚本复刻了官方 mysqltest 用例的语义（官方 mysqltest 在 Windows 上无法直跑）：

| 脚本 | 对应的官方用例 |
| --- | --- |
| `ai_column_ddl_check.py` | `ai_column/t/ai_column_ddl.test` |
| `ai_dml_link_check.py` | AI 列的 DML 联动：写行后自动失效、任务入队 |
| `ai_worker_check.py` | 后台任务写回、事务一致性（回滚不留任务） |
| `ai_vector_check.py` | 向量列与向量索引维护 |
| `ai_retry_check.py` | 失败重试、超时、条件重试 |

### 单测

```powershell
python tools/ai_gateway/test_gateway.py
```

6 个用例，覆盖：转发与凭据透传、延迟注入、500 故障注入、答案丢失、用量与费用计算、
无上游时的确定性。单测起一个假上游服务，不需要真实模型的凭据。
