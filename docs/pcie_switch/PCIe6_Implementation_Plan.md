# PCIe 6.0 Switch 仿真实施计划

版本：v1.0｜日期：2026-11-17｜状态：M0配置合同已实现并通过本地验收；M1–M5未实现/未验收。
依据[需求v1.1](PCIe6_Switch_Requirements.md)、
[硬件架构](PCIe6_Switch_Architecture.md)与
[仿真架构](PCIe6_Simulation_Architecture.md)。本文件是阶段依赖、任务与
完成证据的唯一计划；不承诺未经估算的日历工期或已运行结果。

## 1. 交付边界与依赖

TDD是下述每个任务的开发方法，不替换M0–M5的功能交付顺序或gem5
Switch实现设计。每项任务在实现前先构造可执行验证场景、独立expected/
checker与退出标准；记录语义Red、最小Green与回归，再重构。
不采用函数→SystemC→RTL降低路线。新增
[验证场景目录](PCIe6_Validation_Scenarios.md)只补充测试合同。


先完成HTML功能合同与参考对照，再实现gem5有限资源模型，然后完善
详细Gen6、页面/workload集成和可选controller/产品扩展。

```mermaid
flowchart LR
  M0[M0 规范与输入合同] --> M1[M1 参考模型与JSON]
  M1 --> M2[M2 gem5有限资源]
  M2 --> M3[M3 Gen6协议细化]
  M3 --> M4[M4 页面 / SimAI闭环workload]
  M3 --> G[详细协议与目标评估]
  M4 --> G
  M2 --> M5[M5 可选controller与扩展]
```

首个可用交付为M0–M2：63字段输入、sizing、参考bench、有限资源单颗/
级联、完整结果与CLI。它仍可能是TLP服务预算子集，不能声称详细FLIT。
M3/M4是完整目标的重要后续工作；M5按明确范围单独验收，不因后置而
删除需求。M4的UI/格式准备可提前，但SimAI执行依赖M3周期验收门；不改变M0–M3
先完成Switch模型的主线。

| 阶段 | 当前状态 | 退出门 | 需求映射 |
|---|---|---|---|
| M0 | 本地验收完成 | 63字段/结构/来源合同与21项测试通过；记录见validation/M0.md | F-01/04/08/09，T-01 |
| M1 | 未开始 | sizing与JSON对照、S-01–S-07参考回归 | F-02/07/19/20/21，T-02/03 |
| M2 | 未开始 | 有限单颗/级联、数据/资源守恒、S-08排空 | F-03/06/17/22，A-01至A-04/07，T-04 |
| M3 | 未开始 | 所选Gen6条款及周期资源/时钟trace验收通过，冻结cycle profile | F-01/02/06/19，T-05 |
| M4 | 未开始 | 已验收gem5周期模型接入SimAI、collective/DAG闭环与结果导入 | B-01/05/06，F-09/20/21/22，T-06 |
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
| M3-06 | 周期级profile和资源服务验收 | V-35：端口TLP/cycle、bank/RAM读写、仲裁/Fabric/staging、时钟/CDC和释放边沿有独立周期trace；通过后才启动SimAI闭环 |

退出：只对覆盖矩阵中已实现条款声明通过；非实现项保持缺能力。FEC以
可控错误/延迟抽象时明确不含模拟信道/电气行为。Gen5/non-FLIT兼容须另
建profile和测试，不由低速FLIT配置自动获得。

C/T在定义安全转发与错误提交条件后单独实现/验收，再评估COMP-04。
不能把90ns或250ns填成固定阶段时延后称为目标闭合。

## 6. M4：HTML、需求分析与workload联动

前置：M1格式合同、M2有限资源和M3周期模型验收。M4-01/02等UI/格式
准备可先做；M4-06–M4-11的SimAI闭环执行须等M3-06及所选协议覆盖
通过后启动，不把预算模型接口联通写成周期级集成已完成。

接口依据SimAI侧`/workspaces/SimAI/docs/pcie_switch/PCIe6_Simulation_Architecture.md`
及当前AstraNetworkAPI/SimCCL源码；本地仿真架构§11定义适配合同。

| 任务 | 产物（计划） | 验收 |
|---|---|---|
| M4-01 | ResultBundle转换与硬件页结果导入 | 旧硬件JSON/workspace分开、错误schema/单位/hash拒绝，旧快照保留 |
| M4-02 | 硬件页参考/严格模式结果展示 | 所有指标/限制项/容量/样本/窗口/差异可见；零样本和截短不误报 |
| M4-03 | FlowIR与拓扑/placement转换 | star/split/direct及graph模式、rank组、端口/lane/域合法性可检查 |
| M4-04 | 显式P2P/collective replay及DAG调度 | 无依赖仅称traffic replay；接收完成真正阻塞后继，回调一次 |
| M4-05 | 对比/模板/证据/历史版本 | config/source hash绑定；COMP目标独立profile及评估状态可追溯 |
| M4-06 | 两仓库接口/构建smoke与runtime facade | BackendType增加PCIe、审查分支；SCons/CMake链接同一gem5模型；版本/byte/time语义固定；不复制Switch核心 |
| M4-07 | PcieNetwork API、匹配与callback | V-27至V-29/33：共享时间、recv/data先后、重复tag、源释放/目的可见、callback一次、有界注入/生命周期 |
| M4-08 | 硬件/adapter同路径对照 | V-30：单对端Read/Write、64B/4KiB、incast/级联保持硬件计数与完成语义，无额外NVLink/NET send_lat |
| M4-09 | 显式collective与SimCCL能力输入 | V-31：小规模Ring/Tree，算法/通道/依赖及字节守恒；PCIe-only禁用NVLS，端点归约计费 |
| M4-10 | SimAI闭环DAG与MoE | V-32：Dispatch/Combine热点与compute依赖；降低带宽/加拥塞改变后续任务时刻；缺计算profile不输出TTFT/TPOT |
| M4-11 | 结束/报告及原backend回归 | V-33/34：全rank/callback/drain后退出、未匹配/超时可诊断；两库版本与flow→TLP/cycle关联，Analytical/NS3不退化 |

退出：可完成“HTML配置→导出→CLI运行→结果导入→同口径比较”；页面离线
参考功能和旧导出兼容。闭环TTFT/TPOT/step需计算profile与依赖，缺少时
只报告通信指标。SimAI/SimCCL适配不得引入独立推进的第二个事件时钟。
SimAI退出门还要求V-27–V-34全部适用case通过：硬件路径对照无无法解释
差异，callback不早发/丢失/重复，所有rank共用time/queue；拥塞确实反馈
DAG；原backend回归通过。scope未支持的协议/collective明确列出。

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

## 11. TDD场景与阶段退出标准（补充，原实施任务保持）

每个M任务先冻结所需场景、参数、required bins、容差/计时点和停止条件，
再写实现。场景先行不代表先实现所有测试基础设施再允许任何产品代码；
按具体功能vertical slice走Red→Green→Refactor，保持原模块/依赖设计。

| 阶段 | 先构造的验证场景 | 阶段退出标准 |
|---|---|---|
| M0 | V-01/08/24的schema/规则/目标fixture；全场景required映射 | 63字段覆盖、规范/HTML近似与能力矩阵一致；每个后续任务有可判定场景/expected/退出条件，未知项显式；不声称DUT已实现 |
| M1 | V-01至V-04参考子集、V-19/20/23参考子集 | 原M1产物完成；S-01–S-07全部符合参考判据；sizing/自动值/格式回归通过；oracle/窗口/容差锁定 |
| M2 | V-03至V-15基础协议/有限资源、V-21、V-22 drain、V-23 timing、V-25 | 原M2产物完成；数据/身份/ordering与所有权正确；S-08实际反压/有界排空；retry不双提交；资源守恒且无静默溢出 |
| M3 | V-06/10/13详细子集、V-16至V-18、V-23/24详细目标、V-35周期合同 | 原M3产物完成；所选FLIT/FC/replay条款和independent traces通过；错误恢复/重复抑制/排空；目标与正确性分别评价 |
| M4 | V-20系统子集、V-26、V-27至V-34，按需V-25联动 | 原M4及新增SimAI产物完成；M3周期门已通过；共享gem5 runtime、匹配/callback/同路径对照/collective/DAG/结束回归通过；页面离线/旧格式不退化 |
| M5 | 各extension先新增controller/CFG/NTB/AI/CXL/RAS场景；V-22 reset、V-24扩展 | 对实际声明支持的分支逐项满足原入口/验收合同；未实现记缺能力，不因基础阶段通过而扩大支持声明 |

共同退出要求：阶段必需case无FAIL/skip/xfail，required bins全命中；
整数/数据/身份精确匹配，时间/浮点符合预声明规则；数据错误、重复完成、
双释放、静默溢出和资源泄漏为0。checker用该阶段适用故意缺陷证明可检测；
支持的有界场景全部排空。测试未执行或仅文档完成均不得标PASS。

每次Red/Green保存任务/M-ID、V-ID、需求/规范条款、命令与返回码、版本/
输入hash/seed、错误原因和trace。编译失败不代替预期语义Red；golden或
合同变更独立解释，不倒推expected迎合DUT。完整OS/外部工具和未支持
扩展不作为核心unit/timing场景前置，也不能替代这些场景。

当前状态：M0配置合同本地验收完成，证据见
[M0验收记录](validation/M0.md)。下一步M1-01先构造参考sizing的分段/
字节/限制项fixture，再实现解析计算；M1–M5尚未实现，不提前宣称周期
模型或SimAI可用。单元函数用于测试分段/分配/仲裁，真实事件/背压/完成
行为仍在gem5 timing场景验收，不改变原架构路线。

## 12. SimAI需求追踪

需求§12的SIM-01对应M3-06/V-35；SIM-02/10对应M4-08/V-30；
SIM-03/16对应M4-06/11及V-34；SIM-04/05/06/07/15对应M4-07/11及
V-27/28/29/33；SIM-08/09对应M4-09/V-31；SIM-11/12/13/14对应
M4-03/04/05/10及V-09/15/20/26/30/32。实现前冻结每个SIM项的
required子case，Train/Prefill/Decode/Diffusion分别有DAG fixture，
不是用一个P2P smoke替代全部workload验收。

## 13. 实现设计索引（不改变原任务与阶段门）

| 既有任务 | 必须遵循的详细设计 | 首要验证场景 |
|---|---|---|
| M2-01 | 硬件§11.2/11.8；仿真§14.6的Requester/Completer状态 | V-03至V-08 |
| M2-02 | 硬件§11.8/11.9 Link保护与FC backing；仿真§14.2消息 | V-13/15，M3扩展V-17/18 |
| M2-03 | 硬件§11.3/11.4/11.5 route/order/allocator/VOQ | V-09至V-12 |
| M2-04 | 硬件§11.6/11.7 grant/bank/Fabric/staging；仿真§14.3/14.4 phase与伪代码 | V-12/14/21/23 |
| M2-05/07 | 仿真§4.1/14.2/14.6/14.7 Packet、callback、失败与debug | V-22/25 |
| M3-01至M3-04/06 | 硬件§11.4/11.7/11.8/11.9：所选条款、分组FC、周期trace | V-10/13/16至V-18/35 |
| M4-06至M4-11 | 仿真§11与§14.6共用runtime/完成路径，不能旁路数据面 | V-27至V-34 |

详细设计是拟实现合同，M0配置验收没有实现上述模块。每项任务先按此
合同写fixture/checker，协议policy或优化ordering域改变时补相应反例，
再写代码；不借TDD重新定义或删除已有实现任务。

## 14. 性能目标闭合工作（沿用原任务）

需求§6.4–6.6给出P目标/profile，硬件§12给出预算，仿真§15定义判定；
[性能审查记录](validation/performance_review.md)保存已发现缺口和解析算例。
此补充不改变M0完成状态，也不宣称M3或产品指标已达到。

- M1-01/02须区分输入target与独立协议上限；保留原HTML公式，报告默认
  128 GB/s在64B Read/Write等组合不可达，不将自动宽度当达标证据。
- M2-03/04按真实Cell/beat/metadata/port映射建有限服务；除了Fabric字节
  总带宽，还验证parser、descriptor、grant率、bank与staging瓶颈。
- M3-05/06先构造SW-LAT/RTT-64/SW-BULK/SW-SMALL/SW-PERM/SW-TAIL/
  CT-PIN实例和独立expected budgets，再跑V-23/24/35及V-10/14/15。
  旧1472×9或3×512配置在64B算例应暴露core瓶颈；新候选不得只凭总和判PASS。
- V-24增加每P-ID的PASS/FAIL/UNSUPPORTED/NOT_COMPARABLE/NOT_RUN子case；
  性能不达、测点缺失、样本不足与正确性失败分别诊断。V-23/35校验stage
  时间分解和窗口，而非固定switchNs；V-10包含保守ordering的HOL反例。
- P-04/P-13预算或PHY/endpoint证据不全保持OPEN；P-11/12待业务输入，
  不阻塞已有schema/模块开发，但阻止对应产品性能承诺冻结。
- M4只使用已验收cycle profile接入SimAI；比较模型step/TPOT/collective
  需相同compute/workload/拓扑/算法，不能把COMP宣传数值当预期结果。
