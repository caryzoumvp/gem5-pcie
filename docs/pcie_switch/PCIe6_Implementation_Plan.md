# PCIe 6.0 Switch 仿真实施计划

版本：v1.0｜日期：2026-11-17｜状态：计划已编写，M0–M5均未实现/未验收。
依据[需求v0.9](PCIe6_Switch_Requirements.md)、
[硬件架构](PCIe6_Switch_Architecture.md)与
[仿真架构](PCIe6_Simulation_Architecture.md)。本文件是阶段依赖、任务与
完成证据的唯一计划；不承诺未经估算的日历工期或已运行结果。

## 1. 交付边界与依赖

先完成HTML功能合同与参考对照，再实现gem5有限资源模型，然后完善
详细Gen6、页面/workload集成和可选controller/产品扩展。

```mermaid
flowchart LR
  M0[M0 规范与输入合同] --> M1[M1 参考模型与JSON]
  M1 --> M2[M2 gem5有限资源]
  M2 --> M3[M3 Gen6协议细化]
  M2 --> M4[M4 页面与workload]
  M3 --> G[详细协议与目标评估]
  M4 --> G
  M2 --> M5[M5 可选controller与扩展]
```

首个可用交付为M0–M2：63字段输入、sizing、参考bench、有限资源单颗/
级联、完整结果与CLI。它仍可能是TLP服务预算子集，不能声称详细FLIT。
M3/M4是完整目标的重要后续工作；M5按明确范围单独验收，不因后置而
删除需求。两条分支表示依赖允许独立实现，不表示已经安排并行agent。

| 阶段 | 当前状态 | 退出门 | 需求映射 |
|---|---|---|---|
| M0 | 未开始 | 63字段及规范/近似/能力矩阵可审查 | F-01/04/08/09，T-01 |
| M1 | 未开始 | sizing与JSON对照、S-01–S-07参考回归 | F-02/07/19/20/21，T-02/03 |
| M2 | 未开始 | 有限单颗/级联、数据/资源守恒、S-08排空 | F-03/06/17/22，A-01至A-04/07，T-04 |
| M3 | 未开始 | 所选Gen6 FLIT/FC/replay/ordering条款与trace通过 | F-01/02/06/19，T-05 |
| M4 | 未开始 | 后端结果导入、工作负载与快照闭环 | B-01/05/06，F-09/20/21/22，T-06 |
| M5 | 未开始 | 按extension逐项满足入口与测试合同 | F-05/10至F-16/18/23至F-32，T-07至T-10 |

## 2. M0：规范、schema与能力合同

前置：Base Spec 6.0、HTML和需求可访问。databook为补充，目前不在工作区；
不阻塞基础阶段，但M5接口细化前须恢复可核对版本。历史竞品链接不视为
当前有效证据；涉及冻结时重新核查。

| 任务 | 产物（计划路径） | 完成证据 |
|---|---|---|
| M0-01 | `util/pcie_lab/`字段manifest/schema | 与HTML hardwareFields逐项一一对应，共63项；默认/范围/条件/单位一致 |
| M0-02 | 同目录protocol capability表与条款测试索引 | 请求/Completion/RCB/Tag/ordering/VC/FC/FLIT的规则、近似和不支持项明确 |
| M0-03 | ExperimentSpec/ResultBundle及metadata schema | raw/effective、mode/fidelity/profile、seed/hash/version、窗口和单位明确 |
| M0-04 | topology/resource/profile schema | HTML slices与物理Slice分开；端点角色/Shared FC补参有来源；未知项不静默忽略 |
| M0-05 | 固定HTML/Base Spec来源hash与参考提取说明 | oracle可重复提取，不复制PDF正文/图/RTL；记录同Tick排序和舍入策略 |

退出：字段覆盖100%；边界与非法配置案例可审查；RCB=64B的参考含义和
严格端点角色约束分开；任何缺省严格参数有显式默认及理由或明确拒绝。
此阶段不要求新增仿真功能，也不以文档完成声称协议实现完成。

## 3. M1：参考sizing、事件与JSON对照

前置：M0合同固定；新增源文件版权持有人明确。

| 任务 | 产物（计划） | 验收 |
|---|---|---|
| M1-01 | 纯解析sizing工具 | 分段、字节/Cell、RTT、Tag、限制项排序及全部资源输出与页面一致 |
| M1-02 | 自动资源解析与诊断 | 全部0=自动、32-bit取整、2次幂及H/D非2次幂规则；显式值不被覆盖 |
| M1-03 | legacy JSON导入/结果导出adapter | hardware-calibration全字段/null保持；workspace另识别；未知版本明确拒绝 |
| M1-04 | gem5参考事件组件与运行配置 | 参考FC回收、replay限速、FIFO、时长截短、20%预热和样本口径可复现 |
| M1-05 | `tests/gem5/pcie/`参考golden/回归 | S-01–S-07按HTML真实diff执行，结果绑定source hash/输入 |

解析整数项和字段必须精确匹配；浮点比较事先声明绝对/相对容差，不能
观察到差异后任意放宽。事件结果按同一窗口/同Tick规则比较，额外保存
首个分歧事件和误差来源；未经对照不能声称参考复现。

S-01/02/03覆盖64B/4KiB/MRRS512；S-04/05/06覆盖32 Tag、H=1、256-bit
端口；S-07的1-cell出口预期容量检查失败，是成功捕获不足，不是模型
通过有限容量验收。五个preset与七个bench区分保存。

退出：输入/输出契约、sizing回归和S-01–S-07通过；导出可重新导入相同
配置；HTML保持离线能力。尚无gem5有限队列正确性的承诺。

## 4. M2：gem5有限资源单颗与级联

前置：M1提供输入和对照，M0明确TLP子集与所需扩展参数。

| 任务 | 产物（计划） | 验收 |
|---|---|---|
| M2-01 | transaction、Requester/Completer、测试sink | MPS/MRRS/4KiB/RCB、Tag与Cpl预留；数据内容/长度正确，完成一次 |
| M2-02 | 双向Link、RX backing与资源事件 | 逐跳服务/传播、有限credit窗口、实际释放更新；无无条件定时回收 |
| M2-03 | core allocator/VOQ/order engine | 原子分配、保留池、eligible关系与守恒；小事务descriptor瓶颈可观察 |
| M2-04 | scheduler/Fabric/staging | 并行通道服务、出口争用、同/跨区路径；联合grant无部分长期持有 |
| M2-05 | Packet adapter、retry、drain | 请求/响应重试不双提交；源可复用与目的可见分开；drain后数据正确 |
| M2-06 | 单颗/两颗级联configs与TestLib | request/Completion路径、慢出口反压上溯、停发后排空 |
| M2-07 | metrics/trace与差异报告 | 资源峰值、stall、事务/应用时间戳、未完成/超时；解释与参考结果差异 |

最小实验集：单Read/Write、双向、64B与4KiB、8→1、steady/burst、
小Tag/credit/descriptor/pool/egress、端口/Fabric瓶颈和级联返回路径。
严格模式缩小egress必须造成反压，不复现参考模式“超限后继续入队”。

退出：S-08全部通过；字节/Tag/credit/sector/descriptor/引用守恒；无
双释放/静默drop/重复回调，必要的ordering越过可前进且可排空。超时必须
带等待图/在途快照；watchdog只检测问题，不能当作死锁修复或通过证据。

M2无错TLP预算下的ACK/replay空间若采用近似须标记；不得提前声明M3。

## 5. M3：详细Gen6与目标性能评估

前置：M2有限资源正确，协议条款预期结果独立于被测实现。

| 任务 | 产物（计划） | 验收 |
|---|---|---|
| M3-01 | 合法FLIT header/OHC、packing/flush、跨FLIT组装 | 256B布局、TLP边界/填充、小包混合和部分FLIT的expected traces |
| M3-02 | dedicated/shared FC及能力/更新状态 | TX必需能力、RX可选配置、VC计数/usage限制、信用回收与计数边界 |
| M3-03 | ACK/NAK/timeout/replay与注错 | 副本容量、DLP更新、重发服务、重复抑制、恢复/排空与资源守恒 |
| M3-04 | ordering/Completion规则覆盖 | 合法/非法越过、分段/错误Completion、能力不匹配的条款测试 |
| M3-05 | 参数敏感性/competitor_target评估 | 吞吐/短包/置换/尾延与测点明确，目标结果有输入与误差边界 |

退出：只对覆盖矩阵中已实现条款声明通过；非实现项保持缺能力。FEC以
可控错误/延迟抽象时明确不含模拟信道/电气行为。Gen5/non-FLIT兼容须另
建profile和测试，不由低速FLIT配置自动获得。

C/T在定义安全转发与错误提交条件后单独实现/验收，再评估COMP-04。
不能把90ns或250ns填成固定阶段时延后称为目标闭合。

## 6. M4：HTML、需求分析与workload联动

前置：M1格式合同和M2可用后端。页面联动可在M3完成前进行，但必须显示
实际精度；不能用UI选项代替后端能力。

| 任务 | 产物（计划） | 验收 |
|---|---|---|
| M4-01 | ResultBundle转换与硬件页结果导入 | 旧硬件JSON/workspace分开、错误schema/单位/hash拒绝，旧快照保留 |
| M4-02 | 硬件页参考/严格模式结果展示 | 所有指标/限制项/容量/样本/窗口/差异可见；零样本和截短不误报 |
| M4-03 | FlowIR与拓扑/placement转换 | star/split/direct及graph模式、rank组、端口/lane/域合法性可检查 |
| M4-04 | 显式P2P/collective replay及DAG调度 | 无依赖仅称traffic replay；接收完成真正阻塞后继，回调一次 |
| M4-05 | 对比/模板/证据/历史版本 | config/source hash绑定；COMP目标独立profile及评估状态可追溯 |

退出：可完成“HTML配置→导出→CLI运行→结果导入→同口径比较”；页面离线
参考功能和旧导出兼容。闭环TTFT/TPOT/step需计算profile与依赖，缺少时
只报告通信指标。SimAI/SimCCL适配不得引入独立推进的第二个事件时钟。

## 7. M5：补充controller及产品扩展

以下每行独立定义所需能力，不能因M0–M4通过而标已支持。实现按需求
优先级选择，不预设DWC接口细化必须先于所有产品研究。

| 分支 | 入口条件 | 验收合同 |
|---|---|---|
| controller_detail | 补充PDF可访问、版本/页码核对 | F-23至F-31：TRGT1 grant/class halt、XALI稳定/连续供数、RAM服务与FC所有权 |
| 配置/完整系统 | Type-1映射、DBI代理和Packet适配方案明确 | CFG0/1、DSP代理一次响应、UR、bus/window更新、枚举/P2P/中断无绕行 |
| 多域/NTB | COMP-02转换/权限/端点合同 | 双域窗口、门铃、域隔离、在途撤销/复位资源回收 |
| 广播/INC | COMP-09至COMP-11协议/数值/资源定义 | 慢成员引用、去重/context/epoch、累加结果和真实复制/算力带宽 |
| RAS/管理 | COMP-12/13故障域和接口合同 | read tracking、恢复、INTx/PM/reset、更新隔离；总线能力逐项实现 |
| CXL/其他协议 | 新规范版本、Host/端点/FM矩阵 | COMP-14独立profile、路由/一致性/管理与系统验证，不复用PCIe名义声明 |

## 8. 竞品补充规格的实施分组

| 目标组 | 对应COMP | 纳入阶段 | 输出证据 |
|---|---|---|---|
| 参数/规模/MPS | 01/03/15 | M0 schema，M2/M3扫描，M4展示 | port/lane/域预算与合法性；未知物理组合单列 |
| 时延/吞吐/尾延 | 04至08 | M2分项指标，M3详细/可选C/T | 测点、独立上限、负载、窗口和样本；target通过不等于硅规格 |
| NTB/AI/管理/CXL | 02/09至14 | M5分支 | 缺能力时输出blocked-by-capability，不虚构结果 |
| 外部对照/待核实 | 16至18 | M0登记、冻结前重核 | 原厂来源/日期/型号/测点、不可比字段及假设 |

`competitor_target`覆盖HTML默认需显式选择，保留override diff。产品采纳
和证据冻结另登记；计划只组织实验，不自动把补充目标升级为基础MUST。

## 9. 验证命令与证据登记

本次只改文档，实际运行了`git diff --check`及文档结构/引用检查；下面是
后续实施时的验证层次，尚未运行，目标名需以实际SCons注册为准。

- Python/schema：使用`python3 -m py_compile <实际文件>`查语法，再跑输入/
  自动值/错误诊断和JSON round-trip测试；语法检查不等于行为正确。
- SCons/SimObject：以窄的显式目标验证source与SimObject注册；
  `scons -Q --help`不能证明新SConscript或configure探测正确。
- C++：先构建相关object/GTest；随后运行最小gem5 timing配置。
- TestLib：先在`tests/`执行`./main.py list -q --suites`确认注册/选择，
  再用实际SuiteUID运行；仅在所有required binaries存在时使用`--skip-build`。
- broad ALL binary、quick suite和完整系统测试按改动范围扩展；不用
  `very-long`代替有针对性的资源/协议测试。

每项证据包含任务ID/需求ID、commit及dirty、源文件hash、完整命令、输入、
模式/seed、返回码、日志/trace/结果路径、expected/actual与限制。浮点容差
和统计停止条件先定义；失败后定位首个分歧，不仅比较最终GB/s。

## 10. 完成管理与下一步

任务状态只在实现和对应检查完成后更新，文档提交不改变M0–M5状态。
计划路径集中在仿真架构§9；不要编辑`build/`生成文件、临时复制目录或
vendored库来持久实现。先核查SCons发现/注册规则、版权和MAINTAINERS，
再按M0-01开始；提交/推送遵循用户指示，不在本计划中隐含授权。

当前尚待实现的设计选择：详细FC/VC profile、bank映射、C/T安全提交、
统计收敛/trace开销和DAG输入格式。它们在对应阶段通过可审查schema与
小案例冻结；不会用未定选项阻塞已明确的63字段/参考sizing工作。
