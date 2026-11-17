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
| V-20 | 硬件JSON/null、workspace及未知schema | round-trip完整，错误格式拒绝、旧结果hash不覆盖 | M1/M4 |
| V-21 | 同Tick入队/FC更新/服务完成、仲裁已pending | 确定顺序、无双grant/重复调度，资源计数正确 | M2 |
| V-22 | 资源/队列/Tag/响应在途时drain与明确支持的reset | drain完整完成；reset按已定义失败/清理合同；无幽灵响应/泄漏 | M2 drain/M5 reset |
| V-23 | port/Fabric/replay容量及链路/端点延迟扫描 | 独立预算与可解释瓶颈；测量起止/单位/窗口正确，不双计RTT | M1/M2/M3 |
| V-24 | COMP容量/时延/吞吐/P99与扩展profile | 目标/假设/缺能力/不可比/结果区分；profile显式override | M0登记/M3评估/M5扩展 |
| V-25 | gem5 Packet请求/响应retry、posted接管/sink可见 | false不夺包、重试不双执行、Read完整响应、drain正确 | M2 |
| V-26 | 页面导入、DAG依赖、历史结果/单位/模式 | 真正完成才推进任务；快照/hash绑定，无误标后端精度 | M4 |
| V-27 | 多rank sim_schedule/sim_get_time、零delta/时间单位/边界 | 全rank同gem5时间，事件不逆因果；整数转换/舍入一致、同Tick callback无inline重入 | M4 |
| V-28 | recv先到/data先到、相同tag并发、不同communicator/channel | 按明确key/sequence匹配一次，无串消息/丢数据；byte/count/type与调用链一致 | M4 |
| V-29 | Write push/Read pull、源可复用早于目的可见 | send/recv按各自合同callback；后继不早于所需完成；Read方向正确、全部Cpl齐后完成 | M4 |
| V-30 | 相同profile的standalone和SimAI P2P/Read/Write/incast | 以实际相同release时刻/地址/数据对照trace/字节/路径/资源与完成时间，无重复latency；软件排队差异单列 | M4 |
| V-31 | 2/8-rank Ring AllReduce、显式Tree、PCIe-only SimCCL | Ring每rank字节2×(N−1)/N×tensor及阶段依赖正确；Tree独立expected；无NVLS隐式路径/免费INC | M4 |
| V-32 | Train/Prefill/Decode/Diffusion各自compute/通信DAG与MoE热点，缩小ISL/Fabric或端点窗口 | 拥塞改变通信及后继释放时刻；Dispatch/Combine按token/专家映射守恒；关闭compute profile不输出模型级指标 | M4 |
| V-33 | 有界注入/匹配队列、各rank错时finish、未匹配与超时 | 无无限payload积压/静默拒绝；全rank+callback+drain结束一次；孤立rank finish不提前退出，错误可诊断 | M4 |
| V-34 | PCIe新增BackendType/构建模式、NS3/Analytical旧case | 新backend明确选中，旧backend基线/初始化不退化；两个仓库版本可关联 | M4 |
| V-35 | cycle profile：parser/bank/RAM/arbiter/Fabric/staging/CDC边沿 | 独立小参数周期trace：服务率、grant/占用/释放时刻、瓶颈与量化规则正确；此门先于SimAI执行 | M3 |

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
标准，再实现任务。计划原有实现产物/依赖不变，阶段退出详见实施计划§11；SimAI的V-27–V-34须在M3的V-35周期门及
相应协议覆盖通过后执行。
所有阶段目前未通过，本次只有验证场景文档，没有可执行回归结果。

SystemC/RTL不作为本项目的必经降低阶段；若未来另行开展硬件验证，独立
定义接入目标与验收，不能挤占当前gem5 Switch仿真实现路线。

## 6. SimAI需求覆盖

需求§12的SIM-01–SIM-16是集成验收依据。V-27覆盖SIM-04，V-28覆盖
SIM-03/05/08，V-29覆盖SIM-06，V-30覆盖SIM-02/10/13/14，V-31覆盖
SIM-08/09/12，V-32覆盖SIM-11/12/13/14，V-33覆盖SIM-06/07/15，
V-34覆盖SIM-03/16，V-35覆盖SIM-01。结果/版本还结合V-20/26。
V-32分Train、Prefill、Decode、Diffusion和MoE子fixture，分别覆盖
无争用/受控争用、完成依赖和输入compute profile；缺失子case不计覆盖。
