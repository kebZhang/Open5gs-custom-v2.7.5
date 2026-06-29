# Open5GS v2.7.5 — HTTP Access Log 方案（已实现）

> 目的：为注册延迟实验，记录每条 SBI（HTTP/2）消息在**收发两端**的时间戳，
> 离线按 **ueid** 重组每个 UE 的注册全链路。
>
> **实现状态：代码已落地（见 §7 改动清单）。需在 Linux 上用 meson/ninja 构建**
> （本仓库是 Linux 5GC，Windows 端无法编译，仅做了独立的解析逻辑单测验证）。
>
> **时间戳精度说明**：open5gs 的 `ogs_time_now()` 是 GMT epoch **微秒**（非纳秒），
> 故 `ts` 字段格式为 `YYYY-MM-DDTHH:MM:SS.ffffffZ`（UTC，6 位小数）。free5gc 是纳秒，
> 离线脚本需兼容两种精度。匹配靠 ueid 不靠时间，微秒分辨率对本实验足够。

---

## 1. 要满足的要求（锁定）

| # | 要求 | 如何满足 |
|---|------|---------|
| R1 | 记录每条 HTTP request/response 的 4 个视角（发请求、收请求、发响应、收响应） | lib/sbi 4 个埋点，每点写一行 |
| R2 | 每条记录含 `src NF, dst NF, method, uri, ueid, timestamp` | 见 §4 字段 |
| R3 | **匹配只用 ueid，绝不用时间窗口**（1000 UE/s、每 UE 仅一次注册 ⇒ ueid 全局唯一） | 离线按 ueid 分组；§5 |
| R4 | **核心要求：HTTP log 的记录绝不影响 UE 注册端到端 latency** | 业务线程零磁盘 I/O + 单生产者无锁环形缓冲 + 独立 writer 线程 + 满则丢弃；§6 |
| R5 | **写入顺序不重要**（离线处理），乱序/丢弃可接受 | writer 不保序；满则丢弃 |
| R6 | **尽量只改 `lib/sbi/`，零 NF 业务代码改动** | 普通 NF 全部逻辑在 lib/sbi；**SCP/SEPP 例外见 §11** |

> **R6 修订（开 SCP 后）**：普通 NF（AMF/AUSF/UDM/UDR/PCF/NRF/NSSF/SMF/BSF）的收包都经过
> lib/sbi 的 `ogs_sbi_server_handler`，四视角全在 lib/sbi 完成，零业务改动。**但 SCP/SEPP
> 注册的是自己的 `request_handler`（不经过 `ogs_sbi_server_handler`）**，REQ_RX 视角无法在
> lib/sbi 捕获，必须在 `src/scp/sbi-path.c` 与 `src/sepp/sbi-path.c` 各加一行埋点。详见 §11。

### 实验前提（本方案成立的硬条件，已与用户确认）
- 1000 个 UE 连一个 gNB，向 5GC 注册；**注册后不 dereg、不离开**，记录完成后才 dereg。
- 实验期间 **AUSF 不重启**。
- 这两条共同保证 AUSF 的 `ctx_id`（见 §5.C）在整个实验内**唯一、不复用**。
- **通信方式：本实验开启 SCP（间接通信）。** 每条逻辑消息变成两跳、共 **8 行** log（consumer↔SCP、
  SCP↔producer）。SCP 是也被埋点的 NF，配对靠 (uri-path, ueid)，离线已知 SCP 为中间跳。详见 §11。
  （若改回直连，则退化为一跳 4 行，§11 不适用。）

---

## 2. 四个记录点（与 free5gc 同层：应用层/协议栈外缘，非 h2 帧层）

| 视角 | event | 文件:函数 | 时间戳含义 | 对应 free5gc |
|------|-------|-----------|-----------|--------------|
| 发请求 | `REQ_TX` | `lib/sbi/client.c` `ogs_sbi_client_send_request()` 入口（connection_add 前） | 交给 libcurl 前一刻 | `base.RoundTrip()` 前 |
| 收响应 | `RSP_RX` | `lib/sbi/client.c` `check_multi_info()` 的 `CURLMSG_DONE`（client_cb 前） | curl 报告响应到齐 | `base.RoundTrip()` 后 |
| 收请求 | `REQ_RX` | `lib/sbi/path.c` `ogs_sbi_server_handler()` 入口（queue_push 前）；**SCP/SEPP 另在各自 `request_handler()` 入口** | 收到请求、入业务队列前 | gin `c.Next()` 前 |
| 发响应 | `RSP_TX` | `lib/sbi/server.c` `ogs_sbi_server_send_response()` 入口 | 业务提交发响应 | gin `c.Next()` 后 |

每点只插一行 `ogs_http_log_*()` 调用，其余逻辑在新模块 `lib/sbi/http-log.c`。

> **REQ_RX 的特殊性**：`ogs_sbi_server_handler()` 是普通 NF 注册给 `ogs_sbi_server_start_all()`
> 的回调，故其入口能覆盖所有普通 NF 的收包。但 SCP/SEPP 注册的是自己的 `request_handler`，
> 不走这个公共回调，所以 REQ_RX 在 SCP/SEPP 上要单独埋点（§11）。其余三视角（REQ_TX/RSP_RX/RSP_TX）
> SCP/SEPP 仍走 lib/sbi 的 client.c/server.c，无需额外处理。

> **RSP_TX 的特殊性**：发响应时 `response` 对象**没有 h.uri/h.method**（`ogs_sbi_build_response()`
> 不设置它们）。故 RSP_TX 改用 `stream->request`（原始请求，send_response 时仍存活）取 method/uri/dst。
> 实现为独立函数 `ogs_http_log_rsp_tx(response, req)`，`req` 由 `ogs_sbi_request_from_stream(stream)`
> 取得（新增的 server-actions 访问器，nghttp2-server.c 与 mhd-server.c 各实现一份）。

---

## 3. src / dst 的确定（不靠时间）

- `self` = `OpenAPI_nf_type_ToString(ogs_sbi_self()->nf_instance->nf_type)`（每个 NF 进程即自己）。

| event | src | dst |
|-------|-----|-----|
| REQ_TX | self | 从 `uri` 的 service 前缀推断（`/nausf-auth/…`→AUSF），同 free5gc `nfFromServicePrefix` |
| REQ_RX | 请求头 `User-Agent`（open5gs 自动填发送方 NF 类型，message.c:719） | self |
| RSP_TX | self | **对应请求（`stream->request`）的 `User-Agent`**，取不到则 `""`（已实现，见下） |
| RSP_RX | 从 `uri` 前缀推断 | self |

每行 src/dst 都是确定值（不出现 free5gc 的 `"NaN"`）。

> **RSP_TX 的 dst 已正确实现**：`ogs_http_log_rsp_tx()` 从 `stream->request->http.headers` 读
> `User-Agent`，即"发请求方 NF 类型" = "响应要回给谁"。与 REQ_RX 取 src 同源。
> 注意开 SCP 时：SCP 会**原样透传** consumer 的 User-Agent（TS29.500 §5.2.2.2，scp/sbi-path.c:206-211），
> 所以 producer（如 AUSF）的 RSP_TX dst = **原始 consumer（AMF）**，而非 SCP——这正是语义上有意义的值。
>
> **不依赖前提**：R3 匹配只用 ueid，src/dst 仅作展示。即使某条取不到 User-Agent（留 `""`），也不影响重组。
> **discovery 不污染**：每行 src/dst 只从该行自身的 uri/header 推断，无跨消息状态，discovery 行再不准也
> 不影响其他消息。注册主链路（auth/sdm/udr/policy）uri-path 都带 service 前缀，REQ_TX/RSP_RX 的推断均正确。

---

## 4. 日志格式（JSON Lines，一事件一行）

写入 `HTTP_log.txt`，路径由环境变量 `HTTP_LOG_PATH` 控制，默认 `/tmp/HTTP_log.txt`。

字段：
```
event   : REQ_TX | REQ_RX | RSP_TX | RSP_RX
src      : 发送方 NF（或 ""）
dst      : 接收方 NF（或 ""）
method   : GET | POST | PUT | PATCH | DELETE
uri      : 完整 uri
ueid     : imsi-… / suci-… ；URI 自带 ueid 的留 ""（离线从 uri 抽）
ctx_id   : 仅认证响应这一条有（建 id→ueid 表用）；其余省略
ts       : RFC3339Nano UTC
```

---

## 5. ueid 的三类来源（全部在 lib/sbi 内，零 NF 改动）

注册路径共 13 条 SBI 请求，按 ueid 怎么拿分三类：

### A. URI 自带 ueid（10 条）——log 里 `ueid` 留空，离线从 uri 抽
例：
- `GET /nudm-sdm/v2/imsi-…/am-data`、`/smf-select-data`、`/ue-context-in-smf-data`
- `POST /nudm-sdm/v2/imsi-…/sdm-subscriptions`
- `GET|PUT /nudr-dr/v1/subscription-data/imsi-…/…`
- `POST /nudm-ueau/v1/suci-…/security-information/generate-auth-data`
- `POST /nudm-ueau/v1/imsi-…/auth-events`
- `PUT /nudm-uecm/v1/imsi-…/registrations/amf-3gpp-access`
- `GET /nudr-dr/v1/policy-data/ues/imsi-…/am-data`

### B. ueid 只在请求 body（2 条）——lib/sbi sniff 请求 body，log 直接带 ueid
| 请求 | body 字段 |
|------|----------|
| `POST /nausf-auth/v1/ue-authentications` | `supiOrSuci` |
| `POST /npcf-am-policy-control/v1/policies` | `supi` |
（open5gs 的 body 是内存 `char* http.content`，只读不消费，无需像 free5gc 那样还原。）

### C. ueid 既不在 uri 也不在 body（1 条）——靠 id→ueid 表，离线翻译
对象：`PUT /nausf-auth/v1/ue-authentications/<id>/5g-aka-confirmation`
- 它的 uri 里只有 `<id>`（= AUSF 的 `ctx_id`，nudm-handler.c:220/240），**不是** imsi/suci。
- **表怎么来**：这条 confirmation 的 `<id>`，是前一条 `POST /ue-authentications` 的**响应**给出的：
  - 该 POST 的**请求 body** 有 ueid（B 类）。
  - 该 POST 的**响应** 的 `Location` 头 / body `_links.5g-aka.href` 里含 `<id>`（nudm-handler.c:240-242）。
  - **AMF 同一进程**既发了请求又收了响应，lib/sbi 在同一个 `conn` 上把两者配对（curl `CURLINFO_PRIVATE` 取回同一 `conn`，client.c:697）。
- **实现机制**：`connection_t` 加字段 `char *ue_id`。
  - REQ_TX：对该 POST，sniff 请求 body 得 ueid，存入 `conn->ue_id`。
  - RSP_RX：从 `conn->ue_id` 取回 ueid，从响应 `Location`/href 抽 `<id>`，
    **这条响应记录同时写 `ueid` 和 `ctx_id`** → 即一条 `{id→ueid}` 表项。
- **离线**：扫所有这种响应记录建表 `{id: ueid}`；每条 confirmation 用 uri 里的 `<id>` 查表填 ueid。
- **唯一性**：实验前提（不 dereg、不重启 AUSF）保证 `ausf_ue` 全程存活（ue-sm.c:196 只有 dereg 进 deleted/remove）⇒ `ctx_id` 不复用 ⇒ 查表无歧义。
- 全程字符串精确匹配，**不用时间**。

---

## 6. 异步 writer（满足 R4/R5）—— 核心要求：绝不影响 UE 注册端到端 latency

> **本节是不可妥协的设计约束。** 用户的核心要求：HTTP log 的记录绝不能增加 UE 注册的
> 端到端 latency。日志只用于离线分析，**写入顺序不重要**，因此可以为了"不阻塞业务"
> 牺牲顺序、甚至牺牲少量记录（丢弃）。下面的设计据此而定。

### 6.0 为什么在 open5gs 里"异步"尤其关键（架构事实）
- open5gs 每个 NF 是**单 event-loop 线程**模型（一个 `ogs_app()->queue` + 一个
  `ogs_pollset`，见 ogs-init.c:160-165）。**所有 SBI 收发、所有业务处理、本方案的 4 个埋点，
  全部跑在这同一个线程上。**
- 推论：埋点里**任何**阻塞操作（尤其 `write()`/`fwrite()`/`fflush()` 到磁盘）都会卡住整个
  事件循环 ⇒ 那一刻**所有 UE 的所有处理全停** ⇒ 直接累加进端到端 latency。
- 因此本方案**绝不能**沿用 v2.7.0 udr_log/amf_log 的"主线程内同步 `O_APPEND write()`"做法。
  在 1000 UE/s × ~45 消息/UE × 4 视角 ≈ **18 万行/秒** 的量级下，同步写会把磁盘延迟
  （page cache 刷盘、文件系统抖动）直接灌进被测 latency。

### 6.1 机制：业务线程零磁盘 I/O、零文件锁
```
[event-loop 线程 / 4 个埋点]                 [独立 writer 线程]
  ogs_time_now() 取时间戳(内存,~ns)             循环:
  snprintf 格成一行(预分配/栈)                    从环形缓冲批量取出
  入队: memcpy 进环形缓冲 + 原子推进写游标 ─ring─>  writev() 批量写文件
  立即返回(亚微秒级, 全内存)                       定期 fflush
```
1. **业务线程（4 个埋点）只做 3 件全内存的事**：取时间戳、格式化成一行、把该行 memcpy 进
   环形缓冲。**不碰磁盘、不碰文件锁、不 fflush**，亚微秒级返回。时间戳在入队前捕获，
   writer 滞后也不影响记录值的准确性。
2. **磁盘 write 全部在独立 writer 线程**（`ogs_thread_create`，ogs-thread.h:110）。
   writer 慢、磁盘抖动**都不会回灌**到 event-loop 线程。单写者 ⇒ 行不撕裂/不交错，无需文件锁。

### 6.2 两个不可妥协的设计点（否则会破坏"不阻塞"）
- **(P1) 单生产者无锁环形缓冲（SPSC）**：因为每个 NF 是单 event-loop 线程，生产者**只有一个**，
  天然适配 SPSC。业务线程入队 = "写数据 + 原子推进游标"，**完全无锁**，无 mutex 竞争。
  ⇒ 不要用"每条 mutex_lock + push"的普通队列（高频下锁竞争会给 event-loop 引入微停顿）。
- **(P2) 缓冲满 ⇒ 丢弃 + 计数，绝不等待**（`ogs_http_log_dropped()`）。做延迟实验，
  宁可丢几条日志，也**绝不能**让"等待写日志"反向把延迟加进被测对象。"满则阻塞等待"被明确禁止。

### 6.3 容量与吞吐（用户机器内存大，缓冲可开很大）
- 环形缓冲开大（如 256 MB，可缓冲数百万行），吸收突发。
- 稳态产生速率 ≈ 18 万行/s × ~200 B ≈ **36 MB/s**，远低于 SSD 顺序写带宽（数百 MB/s ~ GB/s），
  ⇒ writer 稳态轻松跟上，缓冲只在突发时短暂上涨，既不丢也不阻塞。
- **自检**：`ogs_http_log_dropped()` 长期为 0 ⇒ writer 跟得上、缓冲无压力、零丢弃。

### 6.4 残留影响（诚实评估，极小且不随磁盘放大）
- 业务线程唯一残留开销 = 每条消息 4 次"取时间戳 + 格式化 + memcpy 入队"，亚微秒级纯内存操作。
- 相对一次 SBI 消息处理（微秒~毫秒级）可忽略；且磁盘的不确定性已被 writer 线程隔离，**不放大**。
- 唯一能让它显现的情形是 event-loop 线程 CPU 已饱和；可由 `ogs_http_log_dropped()` 监控佐证。

### 6.5 收尾
- 定期 flush（~200ms）+ `final()` 优雅停机 flush。`SIGKILL` 可能丢最后 ~200ms 未 flush 的记录
  （实验场景可接受）。

函数签名（**已实现的最终版**）：
```c
void ogs_http_log_init(void);
void ogs_http_log_final(void);
/* REQ_TX / REQ_RX。ue_id 可为 NULL/""（URI 自带 ueid 时留空，body-only 时传 sniff 值） */
void ogs_http_log_request(const char *event,
        ogs_sbi_request_t *request, const char *ue_id);
/* RSP_RX。ctx_id 仅认证响应非 NULL（建 ctx_id→ueid 表） */
void ogs_http_log_response(const char *event,
        ogs_sbi_response_t *response, const char *ue_id, const char *ctx_id);
/* RSP_TX。response 无 h.uri/h.method，故 method/uri/dst 取自原始请求 req（可 NULL） */
void ogs_http_log_rsp_tx(
        ogs_sbi_response_t *response, ogs_sbi_request_t *req);
uint64_t ogs_http_log_dropped(void);
```

---

## 7. 改动清单

| 文件 | 改动 |
|------|------|
| `lib/sbi/http-log.c`（新增） | 异步 writer + 格式化 + dst 前缀推断 + ueid 三类提取 + sniff body + `ogs_http_log_rsp_tx()` |
| `lib/sbi/http-log.h`（新增） | 函数声明（含 `ogs_http_log_rsp_tx`） |
| `lib/sbi/meson.build` | +1 源文件（`http-log.c`） |
| `lib/sbi/ogs-sbi.h` | `#include "http-log.h"`（在 `OGS_SBI_INSIDE` 之外） |
| `lib/sbi/client.c` | `connection_t` 加 `ue_id` 字段；`ogs_sbi_client_send_request()` +REQ_TX；`check_multi_info()` +RSP_RX；`sniff_req_ueid`/`sniff_json_string`/`ctx_id_from_uri`（含前向声明） |
| `lib/sbi/path.c` | `ogs_sbi_server_handler()` +REQ_RX |
| `lib/sbi/server.c` | `ogs_sbi_server_send_response()` 改用 `ogs_http_log_rsp_tx(response, ogs_sbi_request_from_stream(stream))`；+`ogs_sbi_request_from_stream()` 包装 |
| `lib/sbi/server.h` | server-actions 结构体 +`request_from_stream` 指针；+`ogs_sbi_request_from_stream()` 声明 |
| `lib/sbi/nghttp2-server.c` | +`request_from_stream()`（返回 `stream->request`）+ actions 表项 |
| `lib/sbi/mhd-server.c` | 同上（mhd 后端） |
| `lib/sbi/context.c` | `ogs_sbi_context_init/final` 调 `ogs_http_log_init()/final()` |
| **`src/scp/sbi-path.c`** | **`request_handler()` 入口 +REQ_RX（SCP 不走公共 handler，§11）** |
| **`src/sepp/sbi-path.c`** | **`request_handler()` 入口 +REQ_RX（同上）** |
| **普通 NF 业务代码** | **零改动**（仅 SCP/SEPP 例外，开 SCP 实验不可回避） |

---

## 8. 离线分析（重组一个 UE 的注册全链路）

1. **补 ueid**：
   - A 类（log 里 ueid 为空）→ 从 uri 正则抽 `imsi-\d+` / `suci-[\w-]+`。
   - B 类 → log 已带 ueid。
   - C 类（confirmation）→ 先扫认证响应记录建表 `{ctx_id: ueid}`，按 confirmation uri 里的 id 查表。
2. **按 ueid 分组**。
3. **uri 归一化**（必做，因为四视角 uri 形态不同）：去掉 scheme+host（`http://host:port`）、去掉 query（`?...`），
   归一到 path。归一后：
   - REQ_TX（query 在埋点后才拼）、REQ_RX（path-only）、RSP_TX（取自 request path）、RSP_RX（EFFECTIVE_URL 带 query）
     **四者 path 一致** → 可配对。
4. **拼消息**：
   - **直连（一跳，4 行）**：组内按 `(method, uri-path)` 把 4 行拼一条：`REQ_TX/REQ_RX` 一对、`RSP_TX/RSP_RX` 一对。
   - **开 SCP（两跳，8 行，本实验）**：同一 `(method, uri-path, ueid)` 会出现 **8 行**，分布在 consumer、SCP、producer
     三个 NF 的日志里。按 NF（src/dst 或日志文件来源）区分两跳：
     - 第一跳 consumer↔SCP：consumer 的 REQ_TX/RSP_RX + SCP 的 REQ_RX/RSP_TX
     - 第二跳 SCP↔producer：SCP 的 REQ_TX/RSP_RX + producer 的 REQ_RX/RSP_TX
     离线已知 SCP 为中间跳，靠 (uri-path, ueid) 把 8 行归到同一逻辑消息。详见 §11。
5. 计算（时间只用于"算差值"，不用于"判断哪两行是一对"）：
   - 单向(发→收)延迟 = `REQ_RX.ts − REQ_TX.ts`（每跳各算一次）
   - server 处理 = `RSP_TX.ts − REQ_RX.ts`（SCP 这跳即 SCP 的转发耗时；producer 这跳即真实业务处理）
   - 往返 = `RSP_RX.ts − REQ_TX.ts`
   - **端到端（UE 视角）** = consumer 的 `RSP_RX.ts − REQ_TX.ts`（含 SCP 中转，最接近 UE 感知）

---

## 9. 例子

### 例 1：AMF→UDM 普通请求（A 类，ueid 在 uri，log 里留空）
```json
{"event":"REQ_TX","src":"AMF","dst":"UDM","method":"GET","uri":"http://udm:7777/nudm-sdm/v2/imsi-999700000000001/am-data","ueid":"","ts":"2026-06-28T09:00:00.140000100Z"}
{"event":"REQ_RX","src":"AMF","dst":"UDM","method":"GET","uri":"http://udm:7777/nudm-sdm/v2/imsi-999700000000001/am-data","ueid":"","ts":"2026-06-28T09:00:00.140350200Z"}
{"event":"RSP_TX","src":"UDM","dst":"AMF","method":"GET","uri":"http://udm:7777/nudm-sdm/v2/imsi-999700000000001/am-data","ueid":"","ts":"2026-06-28T09:00:00.141900300Z"}
{"event":"RSP_RX","src":"UDM","dst":"AMF","method":"GET","uri":"http://udm:7777/nudm-sdm/v2/imsi-999700000000001/am-data","ueid":"","ts":"2026-06-28T09:00:00.142100400Z"}
```
离线：ueid 从 uri 抽 = `imsi-999700000000001`。

### 例 2：AMF→AUSF 认证请求（B 类 + 建 id→ueid 表）
请求 body 有 `supiOrSuci`；响应 Location/href 有 `<id>=37`。AMF 端 lib/sbi 在 conn 上配对，
REQ 带 ueid，RSP 同时带 ueid 与 ctx_id：
```json
{"event":"REQ_TX","src":"AMF","dst":"AUSF","method":"POST","uri":"http://ausf:7777/nausf-auth/v1/ue-authentications","ueid":"suci-0-999-70-0-0-0-0000000001","ts":"2026-06-28T09:00:00.130000123Z"}
{"event":"REQ_RX","src":"AMF","dst":"AUSF","method":"POST","uri":"http://ausf:7777/nausf-auth/v1/ue-authentications","ueid":"suci-0-999-70-0-0-0-0000000001","ts":"2026-06-28T09:00:00.130410456Z"}
{"event":"RSP_TX","src":"AUSF","dst":"AMF","method":"POST","uri":"http://ausf:7777/nausf-auth/v1/ue-authentications","ueid":"","ts":"2026-06-28T09:00:00.134800789Z"}
{"event":"RSP_RX","src":"AUSF","dst":"AMF","method":"POST","uri":"http://ausf:7777/nausf-auth/v1/ue-authentications","ueid":"suci-0-999-70-0-0-0-0000000001","ctx_id":"37","ts":"2026-06-28T09:00:00.135000999Z"}
```
（RSP_RX 这条同时含 `ueid` 与 `ctx_id`=37 → 建表 {"37": "suci-…0001"}。RSP_TX 在 AUSF 端无 ueid，留空。）

### 例 3：AMF→AUSF 确认请求（C 类，ueid 离线查表）
```json
{"event":"REQ_TX","src":"AMF","dst":"AUSF","method":"PUT","uri":"http://ausf:7777/nausf-auth/v1/ue-authentications/37/5g-aka-confirmation","ueid":"","ctx_id":"37","ts":"2026-06-28T09:00:00.150000Z"}
{"event":"REQ_RX","src":"AMF","dst":"AUSF","method":"PUT","uri":"http://ausf:7777/nausf-auth/v1/ue-authentications/37/5g-aka-confirmation","ueid":"","ctx_id":"37","ts":"2026-06-28T09:00:00.150300Z"}
{"event":"RSP_TX","src":"AUSF","dst":"AMF","method":"PUT","uri":"http://ausf:7777/nausf-auth/v1/ue-authentications/37/5g-aka-confirmation","ueid":"","ctx_id":"37","ts":"2026-06-28T09:00:00.151800Z"}
{"event":"RSP_RX","src":"AUSF","dst":"AMF","method":"PUT","uri":"http://ausf:7777/nausf-auth/v1/ue-authentications/37/5g-aka-confirmation","ueid":"","ctx_id":"37","ts":"2026-06-28T09:00:00.152000Z"}
```
离线：从 uri（或 ctx_id 字段）取 `37` → 查例 2 建的表 → ueid = `suci-0-999-70-…-0000000001`。

### 例 4：AMF→PCF 策略请求（B 类，ueid 在 body）
```json
{"event":"REQ_TX","src":"AMF","dst":"PCF","method":"POST","uri":"http://pcf:7777/npcf-am-policy-control/v1/policies","ueid":"imsi-999700000000001","ts":"2026-06-28T09:00:00.160000Z"}
```

---

## 10. 已知边界（如实记录）
- `RSP_TX` 的 dst 取自请求 User-Agent；极少数请求若无 User-Agent 则留 `""`，不影响匹配（ueid 已足够）。
- 结论成立硬前提：AUSF 不重启 + 记录期不 dereg（见 §1）。若做 dereg/重启实验，`ctx_id` 会复用，
  C 类需改为"AMF build confirmation 时注入 `X-Ue-Id` header"（AMF 1 函数 +1 行）。
- `SIGKILL` 可能丢最后 ~200ms 未 flush 的日志。
- **开 SCP（本实验）**：每条逻辑消息两跳 8 行，需按 §11/§8 重组。uri host 在 SCP 跳上是 SCP 地址，
  但 path 保留 service 前缀，归一化后可配。ctx_id 全程一致（SCP 透传 Location，§11 已验证）。
- **纯 discovery 头路由的消息**（如 `nnrf-disc`）dst 可能推不出（真实目标在 `3gpp-Sbi-Target-apiRoot`
  头而非 URI），src/dst 会空。本实验**不关心 discovery**，直接丢弃这些行即可，不影响其他消息。

### 部署边界（K8s）
- 每个 NF 一个独立 pod ⇒ 各自容器 `/tmp` 隔离，**默认路径不冲突**（无需担心多进程争写同一文件）。
- **但容器 `/tmp` 易失**：pod 重启/销毁日志即丢。需把 `HTTP_LOG_PATH` 指向持久卷（emptyDir/hostPath/PVC）。
- 若多 NF 日志挂到**同一共享卷**做集中收集，**必须每个 NF 用不同文件名**（如按 pod 名 `HTTP_log_${HOSTNAME}.txt`），
  否则跨进程 append 同一文件在高吞吐下会交错/丢写。

---

## 11. 开 SCP（间接通信）——本实验适用

### 11.1 为什么 SCP 需要单独埋点
普通 NF 把 lib/sbi 的 `ogs_sbi_server_handler` 注册给 `ogs_sbi_server_start_all()`，REQ_RX 埋点在其入口，
覆盖所有普通 NF。**但 SCP/SEPP 注册的是自己的 `request_handler`**（src/scp/sbi-path.c、src/sepp/sbi-path.c），
不经过那个公共回调，所以 lib/sbi 的 REQ_RX 对 SCP/SEPP **永不触发**。其余三视角（REQ_TX/RSP_RX/RSP_TX）
SCP/SEPP 仍走 lib/sbi（client.c 发请求/收响应、server.c 发响应），无需改。

**修复**：在 SCP/SEPP 各自 `request_handler()` 入口（`ogs_assert(request->h.uri)` 之后）各加一行
`ogs_http_log_request(OGS_HTTP_LOG_REQ_RX, request, NULL);`。两文件均已 include `ogs-sbi.h`，符号可见。

### 11.2 一条逻辑消息的 8 行（以 AMF→AUSF 为例）
| 跳 | AMF | SCP | AUSF |
|----|-----|-----|------|
| 第一跳 AMF→SCP | REQ_TX, RSP_RX | **REQ_RX(新增)**, RSP_TX | — |
| 第二跳 SCP→AUSF | — | REQ_TX, RSP_RX | REQ_RX, RSP_TX |

共 8 行：AMF 2 + SCP 4 + AUSF 2。

### 11.3 关键机制在 SCP 下仍成立（已逐一验证源码）
- **uri-path 全程不变**：SCP 转发时 `scp_request.h.uri = apiroot(producer) + 收到的 request->h.uri`
  （scp/sbi-path.c:1036），service 前缀和 ueid 保留 ⇒ 8 行归一化后 path 一致，可配对。
- **ueid sniff 不受影响**：`sniff_req_ueid` 读 AMF 本地 `request->http.content`，与 SCP 无关。
- **ctx_id 配对不断**：SCP 的 `response_handler` 把 producer 响应**原样透传**（scp/sbi-path.c:710），
  Location 头保留；AMF 端 `header_cb` 从转发回来的响应抽出同一 `<id>` ⇒ `{ctx_id→ueid}` 表照常建，
  AMF 后续 confirmation 用的 `<id>` 与表一致。
- **User-Agent 透传**：SCP 原样转发 consumer 的 User-Agent（TS29.500 §5.2.2.2，scp/sbi-path.c:206-211）
  ⇒ SCP 的 REQ_RX src = 原始 consumer（AMF，非 SCP）；producer 的 RSP_TX dst 也 = 原始 consumer。

### 11.4 离线注意
- 按 NF 来源（独立日志文件 / src·dst 字段）区分两跳，靠 (uri-path, ueid) 把 8 行串成一条逻辑消息。
- 端到端（UE 视角）延迟取 consumer 的 `RSP_RX.ts − REQ_TX.ts`（含 SCP 中转）。
- 分段延迟：第一跳 AMF↔SCP、第二跳 SCP↔producer 各自算。
```
