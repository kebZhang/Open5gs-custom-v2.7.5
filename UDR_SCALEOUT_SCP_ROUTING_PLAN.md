# UDR 横向扩容 + SCP 基于 ueid 的确定性路由 —— 修改方案(定稿)

> **目标**:K8s 中把 UDR 扩到 N 个实例(其余 NF 各 1),由 **SCP** 用
> `idx = ueid % N` 把每个 UE 确定性地路由到固定 UDR,保证**同一 UE 的所有
> UDR 交互(注册 + 去注册)只由一个 UDR 处理**。
>
> **实验约束(用户确认)**:每次约 1000 个 UE 注册;实验期间 UDR 数量不变、不
> 重启、不 down;UDR 数量在不同实验之间**可能增加**;要求 UE **dereg 也不出错**。
>
> 本文档只针对 `open5gs-custom-v2.7.5`。所有行号引用为本仓库当前代码。
> 部署事实基于实测的 Gradiant chart `gradiantcharts/open5gs:2.3.4` →
> 子 chart `open5gs-udr:2.3.1`(config `sbi.server: - dev: eth0`)。

---

## 0. 结论速览

| 项目 | 结论 |
|---|---|
| 谁做路由 | **SCP**(delegated discovery 下唯一同时掌握「全部 UDR 候选 pod IP」+「带 imsi 的请求 URI」的节点) |
| 必改 C 文件 | **只有 1 个**:`src/scp/sbi-path.c`(`nf_discover_handler` 内加 UDR 分支,约 90 行) |
| UDR/UDM/PCF/AMF/NRF 的 C 代码 | **零改动** |
| 路由键 | 从请求 URI 抽 `imsi-<digits>`;`idx = imsi % N`;第 idx 个 = 候选 UDR **按 pod IP 排序**后的第 idx 个 |
| N 可增加 | ✅ 支持。N 用**运行时数到的可用 UDR 数**,加 UDR 副本后自动生效(同一实验内 N 必须固定,见坑 1) |
| dereg 是否出错 | **不会**。dereg 的所有 UDR 请求(DELETE auth-status / context-data)URI 仍带同一 imsi ⇒ 路由到与注册时相同的 UDR(§2 已核实) |
| 影响 NRF 心跳/注册/订阅? | **不影响**(纯本地只读选择,§6) |
| 状态一致性 | 天然安全:UDR 无进程内 per-UE 状态,SQN 用 Mongo 原子 `$inc`/`$bit`;路由错也不会数据错乱,只破坏「绑定性」这一实验性质 |
| K8s | `udr.replicaCount: N`(chart 已支持,**不用 StatefulSet、不用多 Service/多 release**)+ 每 pod 独立日志路径(§8) |

---

## 1. 为什么是 SCP 路由(不是 UDM / AMF / Service+kube-proxy)

- delegated discovery(Model D,本套模式)下,UDM/PCF **自己不选 UDR 实例**,交给
  SCP 去 NRF 发现并挑。选择点 = `src/scp/sbi-path.c` 的 `nf_discover_handler()`,
  **line 802** 调 `ogs_sbi_nf_instance_find_by_discovery_param()`。
- **只有 SCP 同时握有**:① NRF 发现来的**全部 UDR 候选(各自 pod IP)**;
  ② 携带 imsi 的**原始请求 URI**(`assoc->request->h.uri`)。UDM 没有「所有 UDR」的
  全局视图;AMF 隔两跳看不到 UDM/PCF→UDR 这一 hop 的 URI。
- **不能靠 Service + kube-proxy 做 UE 亲和**:kube-proxy 是 L4,看不到 imsi;且
  SBI 是 HTTP/2 长连接,一条连接上的请求会全落到同一个 pod。必须在**应用层(SCP,
  能看到 imsi)** 路由。
- 一致性收益:访问 UDR 的调用方有 **UDM 和 PCF 两个**,但都过**同一 SCP**;路由放
  SCP 一处即可保证「同一 UE 的 UDM 请求与 PCF 请求落到同一 UDR」。

### 1.1 SCP 如何直连指定 pod(为什么绕过 Service)

UDR config `sbi.server: - dev: eth0`(无 advertise)⇒ 每个 UDR 用**自己的 pod IP**
注册 NRF。SCP discover 得到 N 条 **pod IP 各异**的 profile,并为每个 pod IP 建**独立
HTTP/2 client**(`ogs_sbi_client_find_by_service_type(nf_instance,...)`,sbi-path.c
line 816)。选中 `udr_arr[idx]` → `send_request` 直接连**那个 pod 的 IP**,点对点,
**不经 Service、不经 kube-proxy**。Service `open5gs-udr-sbi` 依然存在但不在此路径上。

---

## 2. 注册 + 去注册中所有到 UDR 的请求(逐条核实,均带干净 imsi)

**注册**(来源 `Visualization/Open5gs_UE_reg_process.html` + `src/udr/nudr-handler.c`):

| # | 调用方→UDR | method | URI | imsi 位置 |
|---|---|---|---|---|
| 1 | UDM→UDR | GET | `/nudr-dr/v1/subscription-data/{imsi}/authentication-data/authentication-subscription` | 第 3 段 |
| 2 | UDM→UDR | PUT | `/nudr-dr/v1/subscription-data/{imsi}/authentication-data/authentication-status` | 第 3 段 |
| 3 | UDM→UDR | PUT | `/nudr-dr/v1/subscription-data/{imsi}/context-data/amf-3gpp-access` | 第 3 段 |
| 4 | UDM→UDR | GET | `/nudr-dr/v1/subscription-data/{imsi}/{plmnId}/provisioned-data/am-data` | 第 3 段 |
| 5 | UDM→UDR | GET | `/nudr-dr/v1/subscription-data/{imsi}/{plmnId}/provisioned-data/smf-selection-subscription-data` | 第 3 段 |
| 6 | **PCF**→UDR | GET | `/nudr-dr/v1/policy-data/ues/{imsi}/am-data` | **第 4 段** |

**去注册(dereg)**(来源 `src/udm/nudr-build.c`,所有 `component[1] = udm_ue->supi`):

| # | 调用方→UDR | method | URI | imsi 位置 |
|---|---|---|---|---|
| 7 | UDM→UDR | DELETE / PUT | `/nudr-dr/v1/subscription-data/{imsi}/authentication-data/authentication-status`(`auth_removal_ind` 时 DELETE) | 第 3 段 |
| 8 | UDM→UDR | DELETE | `/nudr-dr/v1/subscription-data/{imsi}/context-data/amf-3gpp-access` | 第 3 段 |
| — | (SMF 相关 sess) | PUT/DELETE | `/nudr-dr/v1/subscription-data/{imsi}/context-data/smf-registrations/{psi}` | 第 3 段 |

**结论**:注册与去注册的**每一个** UDR 请求 URI 都含同一个 `imsi-<digits>`。同一 UE
的 imsi 恒定 ⇒ `hash(imsi)%N` 恒定 ⇒ **该 UE 的注册全程 + 去注册全程都路由到同一个
UDR**。dereg **无需任何特殊处理,天然安全**。

**两个实现要点(直接决定正确性):**
- **坑 A:imsi 段位不统一**(UDM 系在第 3 段,PCF 在第 4 段)⇒ 抽取**必须用正则找
  第一个 `imsi-\d+`**,不能按固定段位取。
- **坑 B:调用方有 UDM 和 PCF**⇒ gate 必须 `target_nf_type == UDR`(覆盖两者),
  **不能**写 `requester == UDM`。

---

## 3. 关键代码事实(为什么「不重启」前提下方案成立)

1. **每个 UDR 请求都重新 discover + 重新选实例**:带 discovery 头且未带
   `Target-nf-instance-id` 的请求,每次都走 `send_discover → nf_discover_handler`
   (sbi-path.c line 633 → 802)。⇒ round-robin **每请求生效**,同一 UE 每次重算
   `hash(imsi)%N`,恒选同一实例。

2. **discovery 结果按 UUID 去重、不重复插入**:
   `ogs_nnrf_disc_handle_nf_discover_search_result()`(`lib/sbi/nnrf-handler.c`
   line 1244)对每个 profile 先 `nf_instance_find(UUID)`,命中复用、未命中才尾插。
   ⇒ **UDR 不重启 ⇒ UUID 与 pod IP 不变 ⇒ 候选集合稳定**(排序后顺序固定)。

3. **每个实例独立 HTTP/2 client**(sbi-path.c line 816):选谁打谁,不会被连接复用
   粘到别的 pod。

4. **UDR 无进程内 per-UE 状态**:`udr_context_t`(`src/udr/context.c`)为空;SQN 递增
   是 Mongo 服务器端原子 `$inc`+`$bit`(`lib/dbi/subscription.c` line 314/327)。
   ⇒ 即使路由偶发错误把同一 UE 打到两个 UDR,**Mongo 层不会状态不一致**;错误只破坏
   实验的「绑定性」,不影响功能。

---

## 4. 稳定序号:按 **pod IP** 排序(已按实际 config 定稿)

UDR 用 `dev: eth0`(无 advertise)注册 ⇒ `nf_instance->fqdn` **为空**,注册地址是
**pod IP**。因此:

- **排序键 = pod IP 字符串**(`ogs_sockaddr_to_string_static(nf->ipv4[0])`)。pod IP
  在实验期间(不重启)恒定 ⇒ 排序稳定、可复现。
- 收集所有匹配 UDR → 按 pod IP `strcmp` 升序 → 取第 `idx = ueid % N` 个。

> **`idx` ↔ 具体 pod 不可肉眼预测**(pod IP 由 K8s 随机分配)。不影响正确性(同一
> UE 恒映射同一 IP),仅**日志对照**需靠「IP↔pod 名」表:
> `kubectl get pods -o wide -n open5gs | grep udr` 打印各 UDR pod 的 IP,再与 SCP
> 日志 `[SCP-UDR-ROUTE] ... -> <IP>` 对照(§9)。

### 4.1 chart 事实表(实测)

| 事实 | 出处 | 含义 |
|---|---|---|
| UDR = **Deployment**,`replicas: {{ .Values.replicaCount }}` | `open5gs-udr/templates/deployment.yaml` | 支持 `replicaCount: N` 直接扩,**N 可增加** |
| 只有 1 个 Service `open5gs-udr-sbi`,N 副本共用 | `service-sbi.yaml` | Service 不能区分实例(不靠它路由) |
| N 副本共用 1 份 ConfigMap `open5gs-udr` | `configmap.yaml`(名=fullname) | 同一 config,但注册靠 pod IP 天然区分 |
| config `sbi.server: - dev: eth0`,无 advertise | `resources/config/udr.yaml` | **每副本用 pod IP 注册 ⇒ NRF/SCP 看到 N 个独立实例** |
| `DB_URI` env 指向共享 mongodb | `deployment.yaml` env | N 个 UDR 共享一 Mongo(安全,§3.4) |
| `extraEnvVars`/`extraVolumes`/`extraVolumeMounts` 可用 | `values.yaml` | 用于每 pod 独立日志(§8) |

**⇒ `replicaCount: N` 可行,不需要 StatefulSet / 多 Service / 多 release。**

---

## 5. 代码修改(唯一必改文件:`src/scp/sbi-path.c`)

### 5.1 新增 3 个 static helper(放在文件靠前的 static 函数区)

```c
/* ===== TYcustom: UE-affinity routing for UDR (ueid % N) ===================
 * 从请求 URI 抽取该 UE 的 imsi 数字串。覆盖两类 UDR 路径:
 *   /nudr-dr/v1/subscription-data/imsi-<digits>/...   (UDM 系, 注册+去注册)
 *   /nudr-dr/v1/policy-data/ues/imsi-<digits>/...     (PCF)
 * 只抓第一个 "imsi-" 之后的连续数字(与分析脚本 _IMSI_RE 一致)。 */
static bool scp_udr_extract_imsi_digits(const char *uri, char *buf, size_t buflen)
{
    const char *p;
    size_t n = 0;

    if (!uri) return false;
    p = strstr(uri, "imsi-");
    if (!p) return false;
    p += 5; /* skip "imsi-" */
    while (*p >= '0' && *p <= '9' && n + 1 < buflen)
        buf[n++] = *p++;
    if (n == 0) return false;
    buf[n] = '\0';
    return true;
}

/* 稳定排序键 = pod IP 字符串(本 chart UDR 用 eth0 IP 注册, fqdn 为空)。
 * 只读, 勿 free。ogs_sockaddr_to_string_static() 返回共享静态 buffer。 */
static const char *scp_udr_sort_key(ogs_sbi_nf_instance_t *nf)
{
    if (nf->num_of_ipv4 > 0 && nf->ipv4[0])
        return ogs_sockaddr_to_string_static(nf->ipv4[0]);
    if (nf->fqdn)
        return nf->fqdn;
    return nf->id;
}

/* 收集所有「target==UDR 且 discovery 匹配」的实例, 按 pod IP 升序稳定排序,
 * 返回命中数, 数组写入 out[](容量 out_max)。 */
static int scp_collect_sorted_udr(
        OpenAPI_nf_type_e target_nf_type,
        OpenAPI_nf_type_e requester_nf_type,
        ogs_sbi_discovery_option_t *discovery_option,
        ogs_sbi_nf_instance_t **out, int out_max)
{
    ogs_sbi_nf_instance_t *nf = NULL;
    int cnt = 0, i, j;

    ogs_list_for_each(&ogs_sbi_self()->nf_instance_list, nf) {
        if (!ogs_sbi_discovery_param_is_matched(
                nf, target_nf_type, requester_nf_type, discovery_option))
            continue;
        if (cnt < out_max)
            out[cnt++] = nf;
    }

    /* 插入排序(N 小)。ogs_sockaddr_to_string_static() 共享静态 buffer,
     * 故先把被插入元素的键拷到栈上 kk[] 再比较, 避免两次调用互相覆盖。 */
    for (i = 1; i < cnt; i++) {
        ogs_sbi_nf_instance_t *key = out[i];
        char kk[OGS_ADDRSTRLEN];
        ogs_strlcpy(kk, scp_udr_sort_key(key), sizeof(kk));
        j = i - 1;
        while (j >= 0) {
            if (ogs_strcmp(scp_udr_sort_key(out[j]), kk) <= 0) break;
            out[j + 1] = out[j];
            j--;
        }
        out[j + 1] = key;
    }
    return cnt;
}
```

### 5.2 替换 `nf_discover_handler()` 中 line 802 的选择逻辑

**原代码(约 line 800-810):**
```c
    ogs_nnrf_disc_handle_nf_discover_search_result(message.SearchResult);

    nf_instance = ogs_sbi_nf_instance_find_by_discovery_param(
            target_nf_type, requester_nf_type, discovery_option);
    if (!nf_instance) { ... goto cleanup; }
```

**改为:**
```c
    ogs_nnrf_disc_handle_nf_discover_search_result(message.SearchResult);

    /* ===== TYcustom: UDR UE-affinity routing (ueid % N) ==================
     * 仅当目标是 UDR 且能从 URI 抽到 imsi 时做确定性 hash 路由;
     * 否则走原逻辑。gate 用 target==UDR 以覆盖 UDM->UDR 和 PCF->UDR。 */
    nf_instance = NULL;
    if (target_nf_type == OpenAPI_nf_type_UDR) {
#define SCP_MAX_UDR 64
        ogs_sbi_nf_instance_t *udr_arr[SCP_MAX_UDR];
        char imsi_digits[32];
        int n_udr;

        if (scp_udr_extract_imsi_digits(
                request->h.uri, imsi_digits, sizeof(imsi_digits))) {
            n_udr = scp_collect_sorted_udr(
                    target_nf_type, requester_nf_type, discovery_option,
                    udr_arr, SCP_MAX_UDR);
            if (n_udr > 0) {
                uint64_t ueid = ogs_strtoull(imsi_digits, NULL, 10);
                int idx = (int)(ueid % (uint64_t)n_udr);
                nf_instance = udr_arr[idx];
                ogs_info("[SCP-UDR-ROUTE] imsi=%s n_udr=%d idx=%d -> %s",
                        imsi_digits, n_udr, idx,
                        scp_udr_sort_key(nf_instance));
            } else {
                ogs_warn("[SCP-UDR-ROUTE] no UDR candidate for imsi=%s",
                        imsi_digits);
            }
        }
    }

    /* fallback:非 UDR / 抽不到 imsi / 无 UDR 候选 -> 原始选择逻辑 */
    if (!nf_instance)
        nf_instance = ogs_sbi_nf_instance_find_by_discovery_param(
                target_nf_type, requester_nf_type, discovery_option);

    if (!nf_instance) {
        strerror = ogs_msprintf("(NF discover) No NF-Instance [%s:%s]",
                    ogs_sbi_service_type_to_name(service_type),
                    OpenAPI_nf_type_ToString(requester_nf_type));
        res_status = OGS_SBI_HTTP_STATUS_GATEWAY_TIMEOUT;
        goto cleanup;
    }
```

> `request = assoc->request;` 已在函数开头(line 744)取得,`request->h.uri` 是原始
> URI(未加 apiroot),含 `imsi-...`。

### 5.3 依赖检查
- `ogs_strtoull` / `ogs_strcmp` / `ogs_strlcpy` / `OGS_ADDRSTRLEN` /
  `ogs_sockaddr_to_string_static`:均 core 提供,SCP 已包含 `ogs-core.h`。
  (若某工具链无 `ogs_strtoull`,用 `<stdlib.h>` 的 `strtoull`。)
- `OpenAPI_nf_type_UDR`、`ogs_sbi_discovery_param_is_matched`、
  `ogs_sbi_self()->nf_instance_list`:均已导出。

**改动量**:3 helper ≈ 65 行 + 选择点替换 ≈ 25 行,单文件单函数,其它 NF 转发不受影响。

---

## 6. 是否影响 NRF 心跳 / 注册 / 订阅?——不影响

1. **路由是纯本地只读**:只遍历 `nf_instance_list`、读 IP、取模,不改任何状态,不发
   起/取消任何 NRF 交互。
2. **UDR 侧零改动**:每 pod 照常独立注册 + 心跳(`NF_INSTANCE_HEARTBEAT_INTERVAL`)。
   N 个 UDR = NRF 里 N 条独立 profile,标准行为。
3. **SCP 发现/订阅不变**:仍按原样订阅 UDR(`scp_sbi_open` line 92)并 discover;
   只在「拿到列表后如何挑一个」插了 hash,未改发现频率/订阅/xact 生命周期。
4. **不新增 NRF 负载**:每请求 discover 是原本就有的行为(§3.1)。
5. **实例失效处理不变**:失效实例被 `is_matched`(line 2303 `EXCLUDED_FROM_DISCOVERY`)
   过滤,不会进候选数组——但这会改变 N(见坑 1)。

---

## 7. 所有可能的 bug / 边界

### 坑 1 —— N 抖动导致映射漂移(**最重要**)
`idx = ueid % n_udr`。若打流期间某 UDR 掉线,`n_udr` 变化 ⇒ **所有 UE 映射瞬间重排**。
**缓解**:① 你保证实验内 UDR 不重启不下线 ⇒ 不会发生;② **打流前确认 SCP 日志
`n_udr==<你的副本数>`** 再开始;③ **N 可跨实验增加**:加副本后新实验重新数 n_udr
即可,但**同一实验内 N 必须固定**(不要中途 `kubectl scale`)。

### 坑 2 —— discover 未返回全部实例时的早期请求
SCP 刚启动/某 UDR 刚注册,首轮 discover 可能只返回部分。⇒ 部署后等所有 UDR Ready +
SCP 完成首轮 discover(同坑 1 的 `n_udr` 校验)再打流。

### 坑 3 —— imsi 抽取失败退化
若某 UDR 请求 URI 不含 `imsi-`(本注册+去注册 8 条已核实全含),返回 false ⇒ fallback
「选第一个」,**不 crash、不丢请求**,只是不参与绑定。补丁已在该分支加 `ogs_warn`。

### 坑 4 —— gate 写错漏掉 PCF(见 §2 坑 B):必须 `target==UDR`。补丁正确。
### 坑 5 —— imsi 段位假设(见 §2 坑 A):必须正则抽。补丁正确。
### 坑 6 —— ueid 溢出:imsi 15 位超 uint32,补丁用 `ogs_strtoull`(64 位),安全。
### 坑 7 —— 候选数组容量:`SCP_MAX_UDR 64` ≫ 实际,`cnt < out_max` 保护,不越界。

### 坑 8 —— 日志文件互相覆盖(非 C bug,但毁数据分析)
N 个 UDR 默认都写 `/tmp/*.txt` ⇒ 互相追加、无法区分。§8 用 `$(POD_NAME)` 做文件名
后缀 + hostPath 卷解决。**文件名后缀是 pod 名,不是 idx**;对照 idx↔pod 靠 §9 的 IP
表。hostPath 要求这些 UDR pod 在**同一节点**(单节点实验满足);多节点需逐节点收集。

### 坑 9 —— 必须是 delegated discovery(经 SCP)
若 UDM/PCF 配成 direct discovery,本补丁形同虚设。你已确认「必过 SCP」,前提成立;
仍建议核一眼 UDM/PCF/SCP config 为 Model D。

### 坑 10 —— UDR 之外的 target 复用同一 discover 回调
补丁用 `if (target_nf_type == OpenAPI_nf_type_UDR)` 严格圈定,其它 target 走 fallback,
零影响。

### 坑 11 —— eth0 必须是 pod IP(部署后一次性验证)
个别 CNI/hostNetwork 下 eth0 可能是节点 IP ⇒ N 个 pod 注册出相同 IP,方案退化。
**部署后验证**(§9 第 1 步):`kubectl get pods -o wide` 的 N 个 UDR pod IP 应各不
相同,且与 NRF 里注册的 UDR 地址一致。

---

## 8. K8s 部署(基于实际 chart,已定稿)

**方式:UDR 保持 Deployment,`udr.replicaCount: N`;每 pod 独立日志路径。**

### 8.1 values 追加(并入你的 `5gSA-values.yaml`)
```yaml
udr:
  enabled: true
  replicaCount: 10            # N;下次实验想加就改这里
  mongodb:
    enabled: false           # 沿用现有共享 mongodb

  # 每 pod 用 pod 名做日志后缀。POD_NAME 必须排在 *_LOG_PATH 之前,
  # K8s 才会在 value 里展开 $(POD_NAME)。open5gs 侧是纯 getenv。
  extraEnvVars:
    - name: POD_NAME
      valueFrom:
        fieldRef:
          fieldPath: metadata.name
    - name: HTTP_LOG_PATH
      value: "/var/log/udr/HTTP_log_$(POD_NAME).txt"
    - name: DB_LOG_PATH
      value: "/var/log/udr/DB_log_$(POD_NAME).txt"
    - name: UDR_LAT_LOG_PATH
      value: "/var/log/udr/UDR_log_$(POD_NAME).txt"

  extraVolumeMounts:
    - name: udrlog
      mountPath: /var/log/udr
  extraVolumes:
    - name: udrlog
      hostPath:
        path: /local/UDRlog
        type: DirectoryOrCreate
```

> `$(POD_NAME)` 展开要求 `POD_NAME` 在同一 env 列表中更早出现(上面已满足)。
> 若环境不展开,退路是用容器 entrypoint 拼路径,但 chart 默认 `args:["open5gs-udrd"]`,
> 覆盖 command 需谨慎,优先用上面的 extraEnvVars 方式。

### 8.2 部署命令(沿用现有脚本)
`Open5gs_v275_K8s_Setup.sh` 的 helm 命令不用改(已 `-f 5gSA-values.yaml`)。节点先建
目录:`run_remote "sudo mkdir -p /local/UDRlog && sudo chmod -R 777 /local/UDRlog"`。

### 8.3 探针 / HPA
chart readiness/liveness 用 `tcpSocket:7777`,与副本数无关,不用改。**不要启用 UDR 的
HPA/autoscaling**(避免副本数被自动改动,触发坑 1)。

### 8.4 SCP 镜像
§5 改的是 SCP 源码 ⇒ 需把改后 SCP 编进自定义镜像,chart `scp.image` 指向它(参考
memory「free5gc custom image update」同类做法)。**UDR 镜像不用改**(UDR 代码零改动),
仍用官方 `gradiant/open5gs:2.7.5`(除非你要 UDR 的 http/db/lat-log 插桩镜像,那与本
路由方案正交,可叠加)。

### 8.5 其它 NF
AMF/AUSF/UDM/PCF/NRF 各 `replicaCount: 1`,不改。

---

## 9. 验证清单

1. **eth0=pod IP 验证(坑 11)**:`kubectl get pods -o wide -n open5gs | grep udr`
   → N 个 UDR pod IP 各不相同;抽查 NRF 注册地址与之一致。
2. 打少量流,`kubectl logs <scp-pod> -n open5gs | grep SCP-UDR-ROUTE`:
   - 同一 imsi 的**多条注册请求 + dereg 请求**打印的 `-> <IP>` 应**完全一致**;
   - 不同 imsi 的目标 IP 大致均匀分布在 N 个 pod IP;
   - `n_udr` 恒为 N。
3. 各 UDR pod 日志(`/local/UDRlog/DB_log_<podname>.txt`)里,**同一 imsi 只出现在
   同一个 pod 的文件**(绑定性最终证据),含 dereg 的 DELETE 记录。
4. 功能:1000 个 UE 全部注册成功、去注册无错。

---

## 10. 改动汇总

| 文件 | 改动 | 说明 |
|---|---|---|
| `src/scp/sbi-path.c` | **改 C** | 3 helper(imsi 抽取 / IP 排序键 / 收集排序)+ `nf_discover_handler` 分支(§5) |
| 其余所有 `.c/.h` | 不改 | UDR/UDM/PCF/AMF/NRF/dbi/log 全零改动 |
| SCP 镜像 | 重建 | 编入改后 SCP,`scp.image` 指向(§8.4) |
| `5gSA-values.yaml` | 改部署 | `udr.replicaCount: N` + 日志 env/卷(§8.1) |
| 节点 | 建目录 | `/local/UDRlog`(§8.2) |
| 其它 NF | 不改 | 各 1 副本,官方镜像 |

**一句话**:C 代码只动 `src/scp/sbi-path.c` 一个函数(约 90 行,按 pod IP 排序取模);
K8s 只需 `udr.replicaCount: N` + 日志按 pod 名区分(chart 已支持);注册与去注册的每个
UDR 请求都带 imsi ⇒ 同一 UE 全程绑定同一 UDR、dereg 不出错;不碰 NRF 心跳/注册/订阅;
正确性三要点:「gate=target==UDR」「imsi 正则抽」「同一实验内 N 固定且打流前确认
n_udr=N」。
