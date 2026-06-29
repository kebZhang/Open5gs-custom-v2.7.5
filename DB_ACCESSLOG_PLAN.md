# Open5GS v2.7.5 — DB (MongoDB) Access Log 方案（已实现）

> 目的：为注册延迟实验，记录每个 NF 与 MongoDB 的每次交互（request/response 时间戳），
> 与 `HTTP_log.txt` 配套，离线按 **ueid** 重组每个 UE 的注册全链路。
>
> **实现状态：代码已落地（见 §6 改动清单）。需在 Linux 上用 meson/ninja 构建**
> （本仓库是 Linux 5GC，Windows 端无法编译）。
>
> 复用 `lib/sbi/http-log.c` 已验证的异步机制，做成 `lib/dbi/db-log.c` 的对应实现，
> 两个日志时间戳格式一致（`YYYY-MM-DDTHH:MM:SS.ffffffZ`，UTC 微秒），可一起排序分析。

---

## 1. 要满足的要求（锁定）

| # | 要求 | 如何满足 |
|---|------|---------|
| R1 | 一行同时记录 DB request 与 response 时间 | `ogs_dbi_*` 是**同步阻塞**调用，入口取 `req_time`、出口取 `resp_time`，一次往返写一行 |
| R2 | 每条含 `nf, resource, subresource, operation, ueid, req_time, resp_time, latency_us` | 见 §3 字段 |
| R3 | 同一 `subscriber` collection 的多次读能区分开（方案 A） | 上层 SBI 资源名作为 `subresource` 传入；§4 |
| R4 | **核心要求：DB log 绝不影响 UE 注册端到端 latency** | 业务线程零磁盘 I/O + 单生产者环形缓冲（持锁仅移动索引）+ 独立 writer 线程 + 满则丢弃；§5 |
| R5 | 写入顺序不重要、乱序/丢弃可接受 | writer 不保序；满则丢弃并计数 |
| R6 | 每个 NF 写自己 pod 内的 DB_log.txt | 每个 NF 进程各自 `fopen(getenv("DB_LOG_PATH"))`，每 pod 一份文件 |

## 2. 哪些 NF 写 DB_log

凡调用 `ogs_dbi_init()` 的 NF：**UDR、PCF**（5GC 注册路径），以及 **HSS、PCRF**（4G/EPC）。
插桩统一在 `lib/dbi/` 函数内，自动覆盖以上全部，无需在各调用点重复埋点。

## 3. DB_log.txt 字段（JSON Lines）

| 字段 | 含义 |
|------|------|
| `nf` | 发起该 DB 操作的 NF（`ogs_db_log_init("UDR")` 显式传入）|
| `mongo` | 固定 `"mongodb"` |
| `resource` | collection 名（open5gs 只有一个 `subscriber`）|
| `subresource` | 更细的语义/SBI 资源名，用于区分对同一 collection 的多次访问 |
| `operation` | `GetOne` / `UpdateSqn` / `IncrementSqn` / `UpdateImeisv` / `UpdateMme` |
| `ueid` | SUPI/IMSI（所有 `ogs_dbi_*` 首参就是 supi，查询即 `{imsi}`）|
| `req_time` / `resp_time` | DB 调用前/后的 `ogs_time_now()`（UTC 微秒）|
| `latency_us` | `resp_time - req_time`（微秒）|

例：
```json
{"nf":"UDR","mongo":"mongodb","resource":"subscriber","subresource":"authentication-subscription","operation":"GetOne","ueid":"imsi-208930000000001","req_time":"...","resp_time":"...","latency_us":544}
{"nf":"UDR","mongo":"mongodb","resource":"subscriber","subresource":"am-data","operation":"GetOne","ueid":"imsi-208930000000001","req_time":"...","resp_time":"...","latency_us":300}
{"nf":"PCF","mongo":"mongodb","resource":"subscriber","subresource":"am-policy","operation":"GetOne","ueid":"imsi-208930000000001","req_time":"...","resp_time":"...","latency_us":410}
```

## 4. 区分同一 collection 的多次读（方案 A）

`ogs_dbi_subscription_data()` 被 am-data / smf-selection / policy 等多处复用，仅靠
`(resource, operation, ueid)` 三者相同分不开。故给它**加一个 `subresource` 参数**，由上层
handler 传入 SBI 资源名：

| 调用点 | 传入的 subresource |
|--------|-------------------|
| `src/udr/nudr-handler.c` provisioned（GET .../provisioned-data/X）| `recvmsg->h.resource.component[4]`（am-data / smf-selection-subscription-data / …）|
| `src/udr/nudr-handler.c` policy-data | `recvmsg->h.resource.component[3]` |
| `src/pcf/nudr-handler.c` AM 策略直读 | `"am-policy"` |
| `src/hss/hss-context.c` | `"hss-lte"` |

其余 dbi 函数（auth_info / update_sqn / increment_sqn / update_imeisv / update_mme /
session_data / msisdn_data / ims_data）**函数语义唯一**，`subresource` 由库内硬编码。

## 5. 为什么不影响注册 latency（与 HTTP_log 同一机制）

- 业务（事件循环）线程在 `ogs_db_log_emit()` 内只做：取 2 个时间戳 → `ogs_snprintf` 格式化
  一行 → 持锁 `memcpy` 进环形缓冲 + 移动索引。**锁内无任何文件 I/O**。
- 唯一一个后台 writer 线程负责落盘（buffered `FILE*`，每 200ms flush）。单 writer ⇒ 行不撕裂。
- 环形缓冲满 ⇒ 直接丢弃并计数（`ogs_db_log_dropped()`），**绝不阻塞数据面**。
- 这套实现是 `lib/sbi/http-log.c` 的逐行移植，机制已在 HTTP_log 验证。

## 6. 改动清单

**新增**
- `lib/dbi/db-log.h` / `lib/dbi/db-log.c` — 异步 DB 日志（ring buffer + writer 线程）
- `lib/dbi/meson.build` — 加入 `db-log.h` / `db-log.c`
- `lib/dbi/ogs-dbi.h` — include `dbi/db-log.h`

**插桩（lib/dbi，每个函数改为 `_impl` + 薄包装，包装里取时间戳并 emit）**
- `subscription.c`：auth_info, update_sqn, update_imeisv, update_mme, increment_sqn, subscription_data(+`subresource`)
- `session.c`：session_data
- `ims.c`：msisdn_data, ims_data
- `subscription.h`：`ogs_dbi_subscription_data` 增加 `const char *subresource` 形参

**调用点改动（传 subresource）**
- `src/udr/nudr-handler.c`（2 处）、`src/pcf/nudr-handler.c`（1 处）、`src/hss/hss-context.c`（1 处）
  - 注：`src/pcf/context.c` 的 `ogs_dbi_session_data` 调用走 session.c 包装，无需改

**init/final（启动/停止 writer 线程）**
- `src/udr/init.c`、`src/pcf/init.c`、`src/hss/hss-init.c`、`src/pcrf/pcrf-init.c`
  - 紧随 `ogs_dbi_init()` 调 `ogs_db_log_init("XXX")`；`ogs_dbi_final()` 前调 `ogs_db_log_final()`
  - PCF/PCRF 仅在 `ogs_app()->db_uri` 存在时初始化（与其 dbi 初始化一致）

## 7. 配置 / K8s

每 pod 各写各的文件，路径由环境变量决定：

| env | 默认 |
|-----|------|
| `DB_LOG_PATH` | `/tmp/DB_log.txt` |

挂载示例（与 HTTP_log 同一卷即可）：
```yaml
env:
  - name: DB_LOG_PATH
    value: /var/log/open5gs/DB_log.txt
volumeMounts:
  - name: accesslog
    mountPath: /var/log/open5gs
volumes:
  - name: accesslog
    emptyDir: {}
```
