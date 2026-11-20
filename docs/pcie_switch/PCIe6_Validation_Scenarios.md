# PCIe 6.0 Switch 仿真验证场景

版本：v1.1｜日期：2026-11-17｜状态：场景设计，harness与DUT待实现。
依据[需求](PCIe6_Switch_Requirements.md)和
[仿真架构](PCIe6_Simulation_Architecture.md)，配合
[实施计划M0–M5](PCIe6_Implementation_Plan.md)使用。
本文件补充TDD场景，不替换Switch实现设计或引入SystemC/RTL降低路线。

## 1. 场景必须先定义的内容

每个场景先写manifest、stimulus、独立expected/checker、覆盖和停止条件，
再实现对应功能。schema包括：id/version、requirement IDs/规范条款、模式/
精度、topology/resources、端点角色、输入事务及字节、environment、
expected、计时点/容差、required bins、stop/watchdog、版本/hash/seed。

结果为PASS/FAIL/UNSUPPORTED；已冻结的阶段必需能力不能用UNSUPPORTED
或skip绕过。负例必须观察到指定错误和保护行为，不能只检查“有报错”。
oracle不从DUT输出倒推；页面只作为lab_reference的oracle，协议另用规范
fixture与独立规则。允许的多种仲裁/分段策略用允许结果集合/偏序检查。

## 2. 场景目录与实施阶段

| ID | 刺激与需求 | 独立expected/关键判据 | 阶段 |
|---|---|---|---|
| V-01 | 63字段默认/边界/越界/enum/step；F-04/08 | 每字段有bin，合法值规范化，非法值定位准确 | M0/M1 |
| V-02 | 全部0=自动、非零override；需求§5.2 | 手算整数fixture及单位正确，显式值不被覆盖 | M1 |
| V-03 | MemRd payload4096、offset4092、MRRS4096 | 请求4+4092、地址连续、不跨4KiB、总4096 | M1参考/M2协议 |
| V-04 | MemWr payload516、offset4092、MPS256 | 请求4+256+256；字节连续，无虚构网络Cpl | M1参考/M2协议 |
| V-05 | Read512、MPS256、合法RCB128、同Tag两片256B | 重组512B；首片不释Tag，末片才释放，父完成一次 | M2 |
| V-06 | 错Requester/Tag、重复Cpl、错误长度/状态 | 不误完成、不重复回调/释放，指定错误收束 | M2/M3 |
| V-07 | 两Function相同数值Tag、同Requester重复占用Tag | 身份不混淆；未完成Tag不复用；预算逐Function | M2 |
| V-08 | RC/non-RC角色与RCB64/128 | 参考选择保留；严格按角色允许/拒绝，不静默当通用64B | M0/M2 |
| V-09 | 地址路由、Requester ID返回、非法域/目标 | 正确出口/返回路径、无越权、transit不占本地Tag | M2 |
| V-10 | P/NP/Cpl阻塞与ordering组合 | 禁止越过不发生，必要合法越过可前进 | M2/M3 |
| V-11 | 小RX/core/descriptor、分配失败/重试 | 守恒、失败不留部分预留，无越界/静默丢包 | M2 |
| V-12 | 8→1、egressCells=1，sink阻塞后恢复 | 真实反压上溯；停止注入/恢复ready后排空，每事务一次 | M2 |
| V-13 | FC耗尽、接收占用、释放/更新延迟 | 未释放不返；更新到达后才恢复发送资格 | M2/M3 |
| V-14 | 多eligible队列/有限bank与Fabric/staging | 无重复grant；服务机会界内公平，无部分持有死锁 | M2 |
| V-15 | 双向/两颗级联、慢响应端口 | 请求和Cpl分别竞争/路由，资源守恒，可恢复排空 | M2 |
| V-16 | FLIT边界、小包混合、padding/flush | 独立编码fixture内容/有效字节一致，无重复/丢失 | M3 |
| V-17 | Shared FC TX能力、RX开/关、VC池耗尽 | gate/update与协商profile一致，计数/usage不越界 | M3 |
| V-18 | ACK延迟、NAK/timeout、replay满/重复到达 | 保护副本存在、重发占wire、DLP更新、payload只交付一次 | M3 |
| V-19 | HTML S-01–S-07真实diff | 需求§7阈值；S-07预期捕获峰值超限，不当作有限容量通过 | M1 |
| V-20 | 硬件JSON/null、workspace及未知schema | round-trip完整，错误格式拒绝、旧结果hash不覆盖 | M1/M4A |
| V-21 | 同Tick入队/FC更新/服务完成、仲裁已pending | 确定顺序、无双grant/重复调度，资源计数正确 | M2 |
| V-22 | 资源/队列/Tag/响应在途时drain与明确支持的reset | drain完整完成；reset按已定义失败/清理合同；无幽灵响应/泄漏 | M2 drain/M5 reset |
| V-23 | port/Fabric/replay容量及链路/端点延迟扫描 | 独立预算与可解释瓶颈；测量起止/单位/窗口正确，不双计RTT | M1/M2/M3 |
| V-24 | COMP容量/时延/吞吐/P99与扩展profile | 目标/假设/缺能力/不可比/结果区分；profile显式override | M0登记/M3评估/M5扩展 |
| V-25 | gem5 Packet请求/响应retry、posted接管/sink可见 | false不夺包、重试不双执行、Read完整响应、drain正确 | M2 |
| V-26 | HTML 配置导出、gem5 结果导入、历史结果/单位/模式 | 快照/hash 绑定，区分参考和周期结果；旧格式、离线及比较保留 | M4A |
| V-27 | 多rank sim_schedule/sim_get_time、零delta/时间单位/边界 | 全rank同gem5时间，事件不逆因果；整数转换/舍入一致、同Tick callback无inline重入 | M4B |
| V-28 | recv先到/data先到、相同tag并发、不同communicator/channel | 按明确key/sequence匹配一次，无串消息/丢数据；byte/count/type与调用链一致 | M4B |
| V-29 | Write push/Read pull、源可复用早于目的可见 | send/recv按各自合同callback；后继不早于所需完成；Read方向正确、全部Cpl齐后完成 | M4B |
| V-30 | 相同profile的standalone和SimAI P2P/Read/Write/incast | 以实际相同release时刻/地址/数据对照trace/字节/路径/资源与完成时间，无重复latency；软件排队差异单列 | M4B |
| V-31 | 2/8-rank Ring AllReduce、显式Tree、PCIe-only SimCCL | Ring每rank字节2×(N−1)/N×tensor及阶段依赖正确；Tree独立expected；无NVLS隐式路径/免费INC | M4B |
| V-32 | Train/Prefill/Decode/Diffusion各自compute/通信DAG与MoE热点，缩小ISL/Fabric或端点窗口 | 拥塞改变通信及后继释放时刻；Dispatch/Combine按token/专家映射守恒；关闭compute profile不输出模型级指标 | M4B |
| V-33 | 有界注入/匹配队列、各rank错时finish、未匹配与超时 | 无无限payload积压/静默拒绝；全rank+callback+drain结束一次；孤立rank finish不提前退出，错误可诊断 | M4B |
| V-34 | PCIe新增BackendType/构建模式、NS3/Analytical旧case | 新backend明确选中，旧backend基线/初始化不退化；两个仓库版本可关联 | M4B |
| V-35 | cycle profile：parser/bank/RAM/arbiter/Fabric/staging/CDC边沿 | 独立小参数周期trace：服务率、grant/占用/释放时刻、瓶颈与量化规则正确；此门先于 M4A | M3 |
| V-36 | 配置驱动实际 gem5 拓扑，单颗/级联/分支和非法配置 | 创建的端口、窗口、链路、路由及生效参数与配置一致；逐跳计数能区分拓扑；非法输入启动前拒绝 | M4A |
| V-37 | gem5 独立 P2P、incast、collective 流与通信 DAG | 从配置发流，数据/资源守恒，依赖按实际完成推进，停发后有界排空，结果可独立重放 | M4A |

表中多阶段场景按子case标注required阶段；不能因后续可选扩展未实现而
阻塞前阶段，也不能把未来测试计为当前通过。M5的controller/NTB/AI/CXL
需先新增具体场景，不能仅用V-24笼统说明覆盖。

V-05固定Completer分段策略以测试Tag/重组，不要求所有合法Completer都
输出相同两片。strict模式的RCB、Tag、FC规则以Base Spec为准。

## 3. 测试构造和Red/Green证据

示例vertical slice：V-04先给出三条预期请求/字节及checker，缺分段功能
时观察跨页/长度语义Red，再实现最小`splitRequests`使其Green并重构。
这些函数是gem5实现的可单测部件，不要求先做另一套完整模型。

同样为Tag、allocator、scheduler和credit分别先写场景。checker先用
适用故意缺陷自测：遗漏4KiB拆分、首Cpl早释Tag、失败预留不回滚、
无容量仍接受、未释放就返信用、重传重复交付、retry重复写、同Tick双grant。
能检测对应缺陷后再信任Green；编译失败不替代语义Red。

### M2-01 请求与 Completion 单元场景

首批采用 DW 对齐、全字节有效的普通 MemRd/MemWr，MPS=256 B，
MRRS=4096 B。分段和重组的整数与数据逐项精确对照。以下地址为字节地址。

| 子例 | 刺激 | 独立 expected |
|---|---|---|
| V-03a | Read 4096 B，首地址 0x1FFC | 4 B@0x1FFC 与 4092 B@0x2000，每条请求各自在一个 4 KiB 页内 |
| V-04a | Write 516 B，首地址 0x1FFC | 4 B@0x1FFC、256 B@0x2000、256 B@0x2100；网络 Cpl 数为 0 |
| V-05a | Read 512 B，首地址 0x1000，RCB=128 | 固定两片 256 B，Byte Count 512/256，Tag 只在第二片后释放 |
| V-05b | Read 512 B，首地址 0x1020，RCB=128 | 96 B@0x1020、256 B@0x1080、160 B@0x1180，Byte Count 512/416/160，Lower Address 0x20/0/0 |
| V-06a | V-05b 首片后重送首片，或注入错误身份、Byte Count、Lower Address 与非 RCB 切点 | 不修改已收数据和 Tag；随后合法片仍能完成一次 |
| V-06b | V-05b 首片后返回无数据的 CA，Byte Count=416，Lower Address=0 | 只产生一个失败终止并归还重组空间与 Tag 额度；迟到片归为非预期 |
| V-07a | 两个 Function 分别占用 Tag 7；以逆序返回；另在第一个 Function 用不同 TC 再尝试 Tag 7 | 跨 Function 可同时占用；同 Function 重复占用拒绝；每片只修改匹配的请求 |
| V-08a | 请求方设为 Root Complex 或 Endpoint，分别声明 RCB 64/128 | Root Complex 两档通过；Endpoint 仅 128 通过 |

成功重组的数据使用数组模式 `byte[i] = i & 0xff`。每片前后记录
Tag 占用、保留的 Completion 字节额度及父请求终止次数。V-05b 的分段
是一个合法接收样例，Completer 可选择其他满足 RCB/MPS 的分段。
V-06a 的错误报告与保留等待是本单元的处理合同，整机还需验证错误收束
和超时后的资源处理。

依据本地 PCI Express Base Specification Rev. 6.0，MPS 见 §2.2.2
（印刷页141–142），MRRS/4 KiB 见 §2.2.7（162–163），身份与 Tag
见 §2.2.6.2（153–159），Completion 字段见 §2.2.9.1（196–197），
RCB、分片顺序和状态见 §2.3.1.1（214–218），非预期匹配见 §2.3.2
（219–220）。暂定单元目标为 `pcie_request.test`，后续由带端点和
Link 的 gem5 测试验证完整事件路径。

### M2-02 有限 Link 的逐跳信用子例

V-13a 将一个 VC 的相同流控类别配置为 H=1、D=2，线宽为
16 B/周期、周期为 10 tick，单向传播 7 tick，信用更新延迟 11 tick。
在 tick 0 发送只携带 17 B 有效数据的 32 B 测试传输，占用 2 个 D
单位。发送端在 tick 20 可再次使用串行资源，接收端在 tick 27 才能
看到数据。故意卡住接收端至 tick 40，确认 tick 20–39 仍因 FC 阻塞。
接收端在 tick 40 释放，发送端在 tick 51 才可重新发包。每一步检查
可用、在途、占用、返回在途四项的 H/D 整数之和。测试传输服务开销
和非 DW 对齐数据用于单独验证流控额度，协议分段由 V-03–V-08 覆盖。

V-13b 用独立的方向和类别确认同方向类别共享串行服务，反方向可并行。
超配置窗口的请求应明确拒绝，重复释放和未知 handle 不改变账本。
M2 使用配置初始化后的 dedicated-credit TLP 模型。FC 单位见本地
Base Spec Rev. 6.0 §2.6.1（印刷页236–242）。FLIT 的 Shared FC、
更新编码和组粒度留待 M3 验收。

### M2-03 核心资源和排序单元用例

- V-11a 使用 2 descriptors、2 sectors、2 VOQ 索引、64 B/sector。
  一笔 65 B 写占 2 sectors，下一笔 1 B 请求因空间不足被拒绝，
  整数占用不变。此后无数据的请求可占剩余 descriptor 和索引。
- V-11b 在同 VOQ 预留两笔事务，先 commit 一笔再 cancel 另一笔。
  第二个 VOQ 只有在首笔 release 后可分配。分别拒绝已 commit 的
  cancel、未 commit 的 release、重复操作和已退休 token。
- V-11c 给 P 和 Cpl 各留一个 descriptor、sector 和索引。压力下 NP
  无法消费这些保留量，P 和 Cpl 仍可分配。释放后保留额度继续有效。
- V-10a 在同入口、partition 和 TC 暂停 NP，后到 P 和 Cpl 仍 eligible。
  反向把早到 P 卡住时，后到 P/NP/Cpl 均不 eligible，直到该 P
  的最后字节从出口串行器发出。不同 TC 独立检查。
- V-10b 两片同 Requester+Tag 的 Cpl 依次就绪时只能按顺序提交，
  不同 Requester 的同数值 Tag 可以独立前进。排序收束后账本为空。

上述为单元判据。V-10/V-11 的整机验收还需实际 VOQ、Fabric、staging、
出口竞争和资源释放事件。排序依据 Base Spec §2.4.1 Table 2-42
（印刷页222–224）。

### M2-04 数据面联合授权子例

V-12a 连续三笔单 Cell 写入同一出口。每入口 core 只有一个
descriptor，出口只有一个 Cell 暂存，发送端模拟下游无信用。
完成首笔 Fabric 服务后，仅首笔占用暂存，第二笔留在 core，
第三笔留在 RX 并持续占用链路信用。出口恢复后，三笔依序进入
输出 Link，各发一次，RX、core、staging 及排序引用归零。

V-12b 接入两段有限信用 Link 和目标内存。下游冻结到 tick 200，
出口信用 H=1/D=2，入口 H=2/D=4。六笔 32 B 写入时，源应在
恢复前耗尽信用且目的内存可见的事务为零。恢复后按 1–6 顺序
提交，读回 192 B 并比较数据，两段 Link 及交换器全部排空。

V-14a 在两个入口以 1R/1W bank 和独立出口驱动两个事务。两个
bank 和两个 Fabric 通道时可以同沿授权。只保留一个 Fabric
通道时，第二笔等待首笔释放，已分配的暂存与 bank 不会被
未授权请求占用。测试以 10 tick 为周期，单 Cell 读写各一周期。

V-21a 同 tick 产生链路到达、释放及唤醒时合并处理，下一周期边沿
才读更新后的状态。正常路径的一个 transfer 只做一次 RX 释放、
Fabric grant、出口提交与排序退休。

### M2-05/06 原生 Packet 与级联子例

V-15a/V-25a 在一颗及两颗交换器的链路上发送 516 B 跨页 Write，
并在同 tick 发第二笔 16 B Write。配置 outstanding=1。
第二笔首次必须收到 false，且只有 Request retry 才可重新发出。
接收端故意拒绝第一个 WriteResp 一次，10 个周期后发 Response retry。
预期两笔 Write 只产生两个源接管响应，四个 MemWr 分片只在末端提交
一次。500 周期后读回 516 B，比较完整 payload，ReadResp 恰好一个。
另检查关闭注入后 Link、SwitchCore 和外部 Packet 状态归零。
每跳精确核对 4 个 MemWr、2 个 MemRd 及 3 个 CplD。输出 JSON
分别记录源接管响应、目的写可见和读完成 tick。

V-22a 在两颗拓扑的第一笔 Write 仍在途时调用 gem5 drain。已接管
的三片必须到达末端并完成资源归还。尚未接管的第二笔仍由测试驱动
持有，恢复模拟后才 retry，最终全链路为空。

### S-08 原生 8→1 与容量扰动

八路 Requester 同时起跑，每路使用独立地址范围、数据模式和 Tag 额度。
单颗与两颗以每类 H=1、D=8 运行，目标第一次接收后冻结 250 周期。
在 tick 150000 查询目标 RX、信用阻塞和等待依赖，要求尚无写可见，
且最终所有 Packet、Link credit、core 与 staging 所有权归零。

两颗额外使用相同 32 B 流量对照 1 与 64 Cell 暂存，首交换器的各
入口依赖独立，可看出共享出口暂存的容量差异。每路 3 个 MemWr、
2 个 MemRd 和 2 个 CplD，应为每跳 56 次授权和发送。标准 516 B
版本应为每跳 128 次。一格场景最多保持一格在首交换器热点出口。

单路 64 B 与两级 4 KiB 各自逐字节读回，独立预期每跳 7 与 44 个
TLP。4 KiB 子例缩小为 1 Tag、1 descriptor、4 sectors 和 1 个
Fabric 通道。排空后 functional 读比较目标字节，映射外 atomic
检查必须返回错误且没有 Link 传输或性能计数。

### M3-01 FLIT 编码与装填用例

- V-16a 对照 Mem32/Mem64、CplD 和带 OHC-A5 的 UR/CA Cpl：
  逐字节验证类型、TC、Length=0 的 1024 DW 语义、Requester/
  Completer ID、Tag、地址和 Byte Count。低于 4 GiB 使用 Mem32。
- V-16b 两个 TLP 接在一起跨第 235 字节，无额外填充；下个 FLIT
  从续传字节开始。确认每半不超过 8 个非 NOP（含续传）。
  空闲处的 NOP 补到下一个 4 DW 边界或本 FLIT 末尾。
- V-16c 显式 flush 后拆分 256 B 的 TLP/DLP/CRC/FEC 区，验证
  236/6/8/6 B 槽位。校验/重放与注错在后续 V-18 单列。

规范依据本地 Base Spec §2.2.1.2（印刷页122–133）、§2.2.5.2
（152）、§2.2.7.2（168–169）、§2.2.9.2（198–199）及
§4.2.3.3–4.2.3.4.1（351–358）。

### M3-02 Shared FC 分组与归还用例

- V-17a 未合并 P 窗口 H=4/D=4，17 B 与 32 B 依次消耗
  同组余量。任何先归还的单笔都不能单独恢复该组。直到两笔的
  RX 释放和对应 Update 均到达，才恢复 H=4/D=4。
- V-17b P/Cpl 合并 H=8/D=8 时，P 与 Cpl 共享容量，不能
  借用对方已打开组的余量。NP 仍各以 4 为粒度。
- V-17c VC0 限制为共享窗口的 4/8，VC1 独立使用剩余容量。测试
  专用信用回退、先到的专用信用归还优先，以及无 RX Shared 的
  专用路径。非法 VC、重复 ID 和不匹配的 Update 拒绝。

依据本地 Base Spec §2.6.1（印刷页241–242）及 §3.4.1 Table 3-2
（271–272）。单元采用有限、已初始化的账本，部分组在最后一个
RX owner 释放时关组，Update 为协调抽象事件。运行时另行核对
声明的能力、更新粒度及 DLP 时序。

### M3-03 Standard Replay 窗口用例

- V-18a 保护窗口容量 2。发出序号 1、2，第三帧阻塞。帧 1
  校验失败且帧 2 先到时均不交付。累计 NAK=1023 后按 1、2
  重发，原 TLP 区不变。ACK=1 仅释放一格，ACK=2 才排空。
- V-18b 已接收 1 但丢失 ACK，超时只重新调度受保护副本。
  再到的 1 只回 ACK，交付仍为一次。跨 1023→1 的累计 ACK
  和窗口外 ACK 也逐项验证。
- V-18c 独立核对 DLP0/1 的 Flit Usage、Prior Payload、
  Replay Command 与 10-bit 累计序号字段。运行时另测重发的
  线速占用、延迟、可重复注错和解除等待。

初始窗口用例从 post-init、正常传输态出发。依据本地 Base Spec
§4.2.3.4.2.1（印刷页362–383），首版选择 Standard Replay，
Selective Replay 与训练期隐式序号同步单独标记缺能力。
CRC 单测对比 Appendix K.2 的独立 basis bytes，并改变 TLP、DLP 或
CRC 字节触发拒绝。FEC 区与 CRC 区独立，FEC 允许故障/延迟抽象。

V-18d 定时链路每周期 64 B、周期 10 tick，FLIT 串行化为 40 tick。
76 B 的 Mem32 Write 在 tick 0 接管，tick 10 开始发送，末字节
位于第二拍，tick 30 标记一次发出。首 FLIT 在 tick 55 被 CRC
拒绝，NAK 于 tick 70 返回，原序号和 payload 在 tick 70–110
重新占用串行器，tick 115 首次交付。tick 115 释放 RX 后，直到
tick 135 信用更新到达才允许新 TLP。两次数据 FLIT 共占 80 tick。

V-18e 用只容纳一格的重放窗口注入首 ACK 丢失，避免后续累计
ACK 掩盖故障。超时复用原副本，接收端只更新确认状态。原生用例
分别跑无错 Shared、无错 Dedicated、首 FLIT CRC 故障与丢首 ACK，
均逐字节校验跨页读写和每跳 TLP 数。

V-18f 配对方向每个 FLIT 占 40 tick，单向传播 5 tick。反向
NOP ACK 在 tick 70–110 占线，将同一方向的 payload 首次可接收
时刻从 115 推迟到 155。独立检查 NOP 不增加数据序号或 replay
项，以及三帧可被同一累计 ACK 确认。CRC/丢 ACK 在配对原生
路径重跑。另保留延迟回程对照；两种配置的 FC Update 均为协调
事件。

## 4. expected、时间和覆盖合同

- 数据、身份、错误状态、分段字节及容量整数精确比较；浮点/时间量化
  容差在实现前声明，差异后不得随意放宽。
- ordering比较规范偏序与固定仲裁策略，允许合法不同结果；reference
  模式需要复现页面平局/窗口，timing模式不强行复现其不合法近似。
- 计时点分别为应用arrival、first launch、core入口/出口、目的可见、
  last Cpl及父应用完成。HTML只测分段请求launch至完成，不混为应用尾延。
- required bins在任务实现前冻结；阶段必需case全部PASS且bins全命中，
  无必需skip/xfail，数据错/溢出/双释放/重复提交/资源泄漏为0。
- 每阶段仅声明其条款覆盖；unit test通过不代表事件交互或整机通过。

有界排空环境必须定义最大sink阻塞、最小服务、ACK/FC延迟及事务总数。
watchdog根据预算检查并dump等待图，不强制回收资源伪装PASS。永久不ready
只检查安全性，不要求有限liveness；统计场景预声明窗口/样本和停止条件。

## 5. 阶段检查和扩展

M0冻结测试输入/规则；每项M1–M5任务先提交相关场景、checker和退出
标准，再实现任务。M3 的 V-35 及所选协议验收之后，先完成 M4A 的
V-20/26/36/37，再执行 M4B 的 SimAI V-27–V-34。阶段退出详见实施计划 §11。
M0 已通过本地配置验收。M1 的执行入口、oracle 与阶段证据分别见
`tests/gem5/pcie/` 和 [M1验收记录](validation/M1.md)。

SystemC/RTL不作为本项目的必经降低阶段；若未来另行开展硬件验证，独立
定义接入目标与验收，不能挤占当前gem5 Switch仿真实现路线。

### M4A 配置到运行的独立预期

- **V-36** 从 JSON 构建单颗 star、双颗级联及含空闲支路的透明树，
  核对生成的对象、端口、地址窗口和每条双向链路。对齐 64 B Read
  采用 MPS 256 B、MRRS 512 B、RCB 128 B，独立预期是选定路径
  每跳一个 MemRd 和一个反向 CplD，空闲支路零数据 TLP。改变选定
  链路的时钟/宽度时验证该链路服务时刻，其他链路的配置值仍可追溯。
  重复 port、窗口重叠、超 lane 与不合法路径在实例化前返回诊断。
- **V-37** 用 gem5 独立运行 64 B/4 KiB Read/Write、8→1 及显式
  collective 流。DAG fixture 在同一地址写入已知数据，再按目标
  可见事件释放依赖读；读回字节与完成次数需精确匹配。延迟写
  目标服务后，后继读在新的可见时刻之后才可发起。停发后所有
  接管事务完成且等待图为空。归档配置、flow/release ticks、路径、
  payload、完成与结果 hash 供 V-30 重放。
- **V-26** 从离线 HTML 导出 V-36/37 fixture 并导回 CLI 结果。
  将 TestLib 分支树结果直接导入未导出该配置的页面，草稿无效时
  也应成功。检查窗口、模式、内嵌输入哈希和数值保持，拒绝错误
  单位、schema 或哈希。两个导入入口共用同一组记录，重开后在
  「验证记录 → 仿真」中仍能按原输入快照查看。

## 6. SimAI需求覆盖

需求§12的SIM-01–SIM-16是集成验收依据。V-27覆盖SIM-04，V-28覆盖
SIM-03/05/08，V-29覆盖SIM-06，V-30覆盖SIM-02/10/13/14，V-31覆盖
SIM-08/09/12，V-32覆盖SIM-11/12/13/14，V-33覆盖SIM-06/07/15，
V-34覆盖SIM-03/16，V-35覆盖SIM-01。结果/版本还结合V-20/26。
M4B 的 V-30 使用 M4A 的 V-36/37 配置和独立实际完成 trace 作同路径基线。
V-32分Train、Prefill、Decode、Diffusion和MoE子fixture，分别覆盖
无争用/受控争用、完成依赖和输入compute profile；缺失子case不计覆盖。

## 7. 性能目标的required子case

需求§6.4–6.6/硬件架构§12定义P-01–P-13。M3之前为V-23/24/35固定
完整PerformanceCase：独立协议/端点B_ref/R_ref、traffic/ordering/packing/
clock、start/end测点、warmup/window/drain、样本门槛及目标比较式。

V-23/35分别测试70-cycle候选stage预算的实际事件分解、连续byte与整数
Cell-lane容量差异、Tag/FC/replay窗口限制。V-24逐P-ID验证达标、未达标、
缺能力、测点不可比、未运行及样本不足，不以DUT峰值反推分母。
64B/9口旧Fabric配置应暴露瓶颈，不因只有纸面raw speedup就通过；
V-10加入保守ordering造成跨出口HOL而影响SW-PERM目标的反例。

V-35 原生单跳 fixture 使用 2 ns link、1 ns core、各 2 个 CDC
周期，首短包的 12 个独立阶段时刻为
`39/40/42/44/45/46/46/47/47/52/53/56 ns`。其中读服务和
Fabric release 均为 47 ns；staging 直到 TX 接受才归还。
多拍 fixture 以 144 B/64 B Cell、每拍 1 Cell，验证写中间拍
20/30 tick、读中间拍 60/70 tick，拥堵时仍持有完整 staging。
同出口前序可并行进入 Fabric，跨出口前序保持物理发出依赖。

SW-PERM 将每个端口连接到独立 Requester/Completer 地址窗。
同 Slice 在各三个端口内轮换，跨 Slice 轮换三口，每个目的
恰有一个来源。读用例分别检查请求和 Completion 方向。九通道
与三通道保持相同流量和分母，用每周期 grant 上限解释差异。
SW-TAIL 使用同路由空载和按 70% 独立参考速率发送的两套输入，
每流三个各 400 笔的入窗 cohort，按入口选样并计删失。窗口内
发起、入核、离核和目的完成量必须稳定，同时核对两级积压边界。

数值目标不改变数据正确性：任何协议/身份/字节错误先FAIL；性能判据
另报。P-04/P-13缺PHY/endpoint证据不可PASS，P-11/12缺目标输入明确待定。
