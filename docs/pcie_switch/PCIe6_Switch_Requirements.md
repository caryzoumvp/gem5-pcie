# PCIe 6.0 Switch 仿真需求定义

版本：v1.1（补齐性能目标与架构闭合口径）｜日期：2026-11-17｜状态：需求与验收计划，尚无实现结果。

## 1. 目标、来源与优先级

本项目在gem5中建立PCIe 6.0 Switch性能与资源仿真，回答离线实验台的
“目标带宽能否达到、受什么限制、需要多少端口/数据面/缓存资源”问题。
协议正确性以正式规范为准，功能范围由实验台驱动，controller资料补充
实现合同。gem5周期级模型通过验收后，必须接入SimAI/SimCCL，使同一
PCIe数据面驱动模型通信与计算依赖，输出闭环工作负载性能。只完成硬件
微基准、导出带宽/延迟标量或离线flow replay，不满足SimAI集成交付。
本文替换此前以竞品产品规格或DWC controller为中心的需求。

| 优先级 | 来源 | 决定内容 | 不能据此承诺 |
|---|---|---|---|
| 1 | [PCI Express Base Specification Revision 6.0](PCI-Express-Base-Specification-Revision-6.0.pdf)，本地6.0-1.0-PUB | TLP/FLIT、事务身份、ordering、路由、FC和重传的规范行为 | 本模型已经完整合规或已做PHY签核 |
| 2 | [PCIe6_Scaleup_Lab_Offline.html](PCIe6_Scaleup_Lab_Offline.html) | 应用功能、设计输入、资源推导、结果与交换格式 | 其中简化公式等于全部规范规则 |
| 3 | [pcie_sw_databook.pdf](pcie_sw_databook.pdf)，DWC v5.71a，May 2020 | controller/application边界、队列、接口、配置代理等细化参考 | Gen6支持、完整Switch或本项目必须复现DWC RTL |
| 集成范围 | SimAI侧架构与AstraNetworkAPI/SimCCL，见§12 | workload、API、消息/完成/时间与构建兼容 | 覆盖PCIe规范或复制一套硬件后端 |
| 补充层 | 附录A竞品资料与§11补充目标 | 产品容量、功能候选、性能对标与扫描目标 | 取代规范、自动成为首版强制验收或本项目实测能力 |

发生冲突时：规范约束优先；HTML近似保留为显式`lab_reference`模式，
不得作为`pcie6_timing`的协议规则。databook只补充实现选择，不能扩大
首版范围或替换HTML中的配置项。未核实的竞品数值不参与验收。

本次不修改HTML。其现有计算、缺陷和近似均需可追溯，协议修正导致的
结果差异单列解释，不能悄悄改为另一套默认值。

## 2. 使用场景与交付范围

| ID | 场景 | 必须交付的能力 |
|---|---|---|
| B-01 | 需求分析：Decode/Prefill/Train/Diffusion | 模型、并行策略、rank放置与通信需求到拓扑负载的映射 |
| B-02 | 硬件规格：Read/Write sizing | 应用传输到TLP/Cell/Tag/Credit/Replay/队列/SRAM预算 |
| B-03 | 硬件仿真：单链路、单Switch和级联 | 当前配置运行、吞吐/时延/停顿/占用及目标可达性 |
| B-04 | Incast与突发 | 端口比、热点、Tag/信用/端口/Fabric瓶颈与容量不足诊断 |
| B-05 | 对比、模板、验证记录和历史版本 | 配置快照、结果来源、模式、可复现对比与导出 |
| B-06 | SimAI闭环工作负载（周期模型验收后必须交付） | SimAI/SimCCL调度/展开通信依赖，同一gem5 PCIe runtime负责硬件周期与完成反馈；要求见SIM-01–SIM-16 |
| B-07 | 后续协议/产品扩展 | 多域/NTB、AI复制/INC、CXL等独立立项，不阻塞核心实验台交付 |

首版先实现硬件页的配置兼容、解析sizing、synthetic及gem5周期模型，
通过M3周期/所选协议验收后进入M4 SimAI闭环集成。UI/schema准备可提前，
SimAI执行不能以M2的预算模型代替该入口门；这项顺序不要求先完成M5
所有可选产品功能。需求分析页进一步复用该闭环后端。完整OS、PHY训练、RTL/VIP联动和专用IP
接口复现不是首版前置条件。详细协议模式仅宣称已测试的规范子集。

## 3. 协议约束与近似边界

以下均指本地Base Spec印刷页，作为协议覆盖矩阵的起点；实现时必须
继续展开所选事务/能力的适用条款，不以本表代替整份规范。

| 约束 | 首版要求 | 规范索引 |
|---|---|---|
| Requester/Tag身份 | 每Requester未完成请求唯一；最后Completion后才释放；编码能力、enable及实际预算分开 | §2.2.6.2，页153–159 |
| Memory请求 | MPS/MRRS分别约束相应事务，单请求不跨4 KiB；检查长度/地址/byte enable | §2.2.7、§2.3.1，页162起、208起 |
| Completion | 校验状态、长度、Byte Count、Lower Address；同请求数据按地址递增且完整 | §2.2.9、§2.3.1.1、§2.3.2，页195起、214起、219起 |
| RCB | RC可为64/128B；其他System Elements为128B；非任意端点通用64B开关 | §2.3.1.1，页214–215 |
| ordering | 在合法eligible集合内仲裁，禁止任意重排；逐项声明RO/IDO支持 | §2.4，页222起 |
| TC/VC | 外链VC与内部通道/队列不是同一对象；保存映射与资格 | §2.5，页229起 |
| FC | 逐链路接收空间约束；不代表端到端完成；Flit Mode TX必须支持Shared FC，RX支持可选 | §2.6，页235起；§2.6.1.1，页242起 |
| FLIT | 256B：236B TLP区、6B DLP、8B CRC、6B ECC；TLP区不等于应用payload | §4.2.3.3，页351–352 |
| replay | 有限副本、确认/重传及重复抑制；重放TLP信息不等于重放旧DLP内容 | §4.2.3.3及其引用的数据完整性规则 |

硬件页的`rate=16/32/64`都标作FLIT Mode；16/32不能直接解释为传统
Gen4/Gen5 non-FLIT。严格模式按实际链路模式计编码和服务开销；HTML
`rate×lanes/8×236/256×packing`保留为参考公式，并标明低速编码近似。

HTML可调`reqHeader/cplHeader`是开销假设，严格模式由所选FLIT格式/
OHC/事务推导合法头部。HTML `tagCeiling`表32/256/768/15360仅是参考
模式的预算校验；严格模式按规范能力与路径选择合法Tag集合。

`cplSplit=rcb`用于高Completion率敏感性分析，不把RCB当成无条件最大
payload。HTML中的64B RCB默认不证明XPU-to-XPU配置合法；严格模式必须
补充端点角色并验证RCB，非法配置拒绝或要求显式参考模式。

## 4. 功能需求与模式

### 4.1 必须区分的模式

- `lab_reference`：复现HTML sizing及现有事件近似，保留输入默认值、
  公式、截短和统计口径。结果标记“估算/参考”，不作协议正确性证据。
- `pcie6_timing`：gem5中的有限资源、合法事务、ordering和逐跳反压。
  可先用TLP服务预算近似FLIT，结果必须标记粒度和未实现条款；详细FLIT
  才进一步实现packing、Shared FC与replay状态。尚未实现的能力拒绝。
- `controller_detail`：可选的databook接口细化，包含TRGT1/XALI beat、
  DBI配置代理等。不得成为硬件页解析sizing/首版timing的强制依赖。

### 4.2 功能矩阵（保留ID，重新定义验收范围）

| ID | 需求 | 验收条件 |
|---|---|---|
| F-01 | 支持显式PCIe6协议profile与参考模式 | 每份结果保存模式、粒度、已覆盖条款及近似 |
| F-02 | MemRd/MemWr/CplD拆分、路由与完成 | 规范子集合法；字节守恒，无重复完成 |
| F-03 | 单域P2P与发起/响应端口比 | 单颗与级联路径正确，两个方向分别争用 |
| F-04 | 接受硬件页链宽/速率/端口参数 | 校验schema、模式、lane/端口预算；不强制144 lanes |
| F-05 | 域与可达性显式 | 首版透明单域；多Host/NTB仅独立扩展 |
| F-06 | timing模式ordering、FC与前进性 | 有限队列、资源释放驱动信用、可排空；参考模式另标理想回收 |
| F-07 | 每Function Tag与实际预算 | 分开统计；参考模式保留均分聚合近似 |
| F-08 | 异常与未完成请求可见 | 非法配置拒绝；仿真截短、容量不足、无样本明确报告 |
| F-09 | 版本化配置/结果及来源 | normalized输入、版本、seed、模式与时间窗口可重放 |
| F-10 | 广播作为可选扩展 | 普通Read/Write不隐式复制 |
| F-11 | 动态多播作为可选扩展 | 另定义组、权限、慢成员与资源合同 |
| F-12 | INC作为可选扩展 | 另定义数值和有限算力合同 |
| F-13 | AI上下文/epoch/重试 | 启用AI服务前独立验证，不阻塞核心lab |
| F-14 | 非均匀通信/热点 | 首版8→1；后续All-to-Allv流量可映射 |
| F-15 | CXL独立profile | 不把PCIe内存事务仿真称作CXL.mem支持 |
| F-16 | pooling/sharing独立范围 | 后续定义，不作为首版验收 |
| F-17 | 权限与未知能力 | 不支持的配置不得静默启用 |
| F-18 | 端口状态/故障扩展 | 首版可预设link-up，注明未模拟训练/热插拔/DPC |
| F-19 | 实验计数与trace | 输出payload/TLP/FLIT估算/Cell、stall与队列峰值 |
| F-20 | 离线JSON交换 | 保留现有workspace与hardware-calibration两种格式的区别 |
| F-21 | 快照与结果绑定 | 修改输入后旧结果失效或仍绑定旧配置，不能误标为新结果 |
| F-22 | SimAI/SimCCL workload适配与闭环反馈 | 同一gem5周期数据面，流量/依赖/版本可追溯，拥塞影响任务完成；满足§12，估算/CSV不称闭环模型性能 |

### 4.3 Databook的补充细化，不扩大基础范围

DWC是Gen5单端口controller。以下保留F-23–F-32作为细化ID，除实现资源
所有权所必需的合同外，其接口级复现只在`controller_detail`验收。

| ID | 补充合同 | 来源 / 适用性 |
|---|---|---|
| F-23 | 分离controller与application core状态 | §1.1、2.1、P.4；核心架构参考 |
| F-24 | RADM P/NP/Cpl资源；S/F、C/T、bypass责任 | §2.5页83–87；首版S/F，其余后续 |
| F-25 | TRGT1提前class halt与在途grant预留 | §4.4.3页328–329；beat接口启用时验证 |
| F-26 | XALI client、halt稳定及连续供数；普通TX队列归application | §3.3、4.1；不要求lab界面直接配置DWC引脚 |
| F-27 | USP本地CFG、DSP DBI代理和下游CFG转换 | §3.4.2、P.4.1；配置/枚举扩展 |
| F-28 | 本地ELBI与transit身份/路由分离 | 页108–110、P.4.1；事务语义必须明确 |
| F-29 | RADM出队与core资源释放是不同事件 | §3.9；FC backing设计参考，不覆盖Gen6规范 |
| F-30 | 本地NP发起预留Cpl header/data预算 | §3.23；本地Requester适用，transit不分配本地Tag |
| F-31 | RX header/data/formation、retry及core SRAM独立计量 | §2.2、5.17–5.21；Gen6布局另定，不强制DWC RAM结构 |
| F-32 | reset/INTx/PM/RAS跨端口协调 | P.4.3–P.5.1；系统扩展，首版明确缺失 |

### 4.4 SimAI集成的必需范围

B-06/F-22是M4的必需交付，不再仅以“后续适配”描述。§12的SIM-01–SIM-16
给出可执行的功能、时序、生命周期与回归验收；不改变F-01–F-32或COMP
编号。验证环境与expected先于每项实现，遵循原M0–M5计划的TDD要求。


## 5. 硬件页输入合同

HTML内嵌`hardwareFields`是字段名、UI默认值、范围、步长和枚举的直接
来源。后端不得删除或重命名这些字段。下表完整列出当前字段；实际运行
须保存原始输入和解析后的有效值。`read/cascade/fixed`条件只控制显示，
不应把仍存在的隐藏字段误当成已启用行为。

GB/s为十进制，KiB为1024B，ns/µs分别换算；频率为MHz。所有“0=自动”
字段的0是推导请求，不能当作零容量或无限容量。

| 字段 | 含义/单位 | 默认值 | 范围/枚举 |
|---|---|---|---|
| `op` | 事务类型 | read | read / write |
| `payload` | 每笔应用传输 (B) | 64 | 4–1048576；步长4 |
| `target` | 每发起端口目标 Payload (GB/s) | 128 | .001–1e5；步长.001 |
| `offset` | 起始地址 4 KiB 页内偏移 (B) | 0 | 0–4092；步长4 |
| `mrrs` | MRRS (B) | 4096 | 128/256/512/1024/2048/4096 |
| `mps` | MPS (B) | 256 | 128/256/512/1024/2048/4096 |
| `rcb` | RCB (B) | 64 | 64/128 |
| `cplSplit` | Completion 策略 | mps | mps / rcb |
| `reqHeader` | 请求 TLP 开销 (B) | 16 | 12–128；步长4 |
| `cplHeader` | Completion TLP 开销 (B) | 12 | 12–128；步长4 |
| `packing` | FLIT TLP 区填充率 (%) | 100 | 1–100；步长.1 |
| `rate` | FLIT Mode 链路速率 (GT/s) | 64 | 16/32/64（FLIT Mode） |
| `reqPorts` | 活跃发起端口 | 1 | 1–32；步长1 |
| `cplPorts` | 活跃响应端口 | 1 | 1–32；步长1 |
| `reqLanes` | 发起端链宽 | 16 | 1/2/4/8/16 |
| `cplLanes` | 响应端链宽 | 16 | 1/2/4/8/16 |
| `switches` | 路径经过的 Switch 数 | 1 | 1–8；步长1 |
| `islLanes` | 级联链宽 | 16 | 1/2/4/8/16 |
| `chipLanes` | 单颗 IP SerDes lanes | 144 | 1–2048；步长1 |
| `chipPorts` | 单颗 IP 逻辑端口 | 9 | 2–256；步长1 |
| `reqMeters` | 发起端链路长度 (m) | .5 | 0–1000；步长.001 |
| `cplMeters` | 响应端链路长度 (m) | .5 | 0–1000；步长.001 |
| `islMeters` | 每段级联链路长度 (m) | .5 | 0–1000；步长.001 |
| `nsMeter` | 单向传播延迟 (ns/m) | 5 | .001–1000；步长.001 |
| `retimers` | 每段链路 Retimer 数 | 0 | 0–8；步长1 |
| `retimerNs` | 每 Retimer 单向延迟 (ns) | 10 | 0–10000；步长.01 |
| `switchNs` | 单次 Switch 穿越 (ns) | 60 | 0–100000；步长.01 |
| `endpointNs` | 端点处理及其余延迟预算 (ns) | 120 | 0–100000；步长.01 |
| `rttMode` | 事务 RTT 口径 | derived | derived / fixed |
| `fixedRtt` | 手动事务 RTT (ns) | 250 | 1–1e6；步长.01 |
| `creditFixed` | Credit 环路固定延迟 (ns) | 180 | 1–1e6；步长.01 |
| `ackFixed` | ACK 环路固定延迟 (ns) | 180 | 1–1e6；步长.01 |
| `functions` | 每发起端口的均分 Function 数 | 1 | 1–256；步长1 |
| `tagBits` | Tag 字段位宽 | 10 | 5/8/10/14 |
| `tagBudget` | 每 Function 实际可用 Tag | 768 | 1–16384；步长1 |
| `trackMode` | Switch 跟踪范围 | transparent | transparent / proxy |
| `trackExtra` | 跟踪表附加字段 (bit/entry) | 32 | 0–512；步长1 |
| `requestLimit` | 每发起端口请求率上限 (MTLP/s，0=不限) | 0 | 0–100000；步长.01 |
| `cplLimit` | 每响应端口 Cpl 速率上限 (MTLP/s，0=不限) | 0 | 0–100000；步长.01 |
| `margin` | 资源裕量 (%) | 20 | 0–400；步长.1 |
| `headerCredits` | 每 VC 每类别 H 窗口 (0=自动) | 0 | 0–65536；步长1 |
| `dataCredits` | 每 VC 每类别 D 窗口 (0=自动) | 0 | 0–1048576；步长1 |
| `replayFlits` | 每端口 TX Replay 深度 (FLIT，0=自动) | 0 | 0–1048576；步长1 |
| `clock` | 数据面频率 (MHz) | 1000 | 1–20000；步长1 |
| `util` | 内部有效利用率 (%) | 85 | 1–100；步长.1 |
| `channels` | 每端口每方向 IP 物理通道 | 1 | 1–32；步长1 |
| `portWidth` | 每物理通道位宽 (bit，0=自动) | 0 | 0–65536；步长32 |
| `slices` | Crossbar / Fabric 并行 Slice | 9 | 1–256；步长1 |
| `fabricWidth` | 每 Slice 位宽 (bit，0=自动) | 0 | 0–65536；步长32 |
| `vc` | 每端口 VC 数 | 2 | 1–8；步长1 |
| `cell` | 内部 Cell 数据区 (B) | 64 | 64/128/256 |
| `meta` | 每 Cell 元数据 (B) | 8 | 0–128；步长1 |
| `fabricCreditNs` | 片内 Fabric 槽位返回 (ns) | 40 | 1–1e6；步长.01 |
| `egressCells` | 每出口每 VC 暂存 (Cell，0=自动) | 0 | 0–1048576；步长1 |
| `voqDepth` | 每活跃 VOQ 的 Cell 额度 (0=自动) | 0 | 0–65536；步长1 |
| `activeVoq` | 每入口支持活跃 VOQ 数 | 8 | 1–4096；步长1 |
| `descriptor` | 每 VOQ Descriptor (B) | 32 | 8–512；步长1 |
| `poolKiB` | 每入口共享 Payload 池 (KiB，0=自动) | 128 | 0–1048576；步长1 |
| `burst` | 分析突发倍率 | 2 | 1–100；步长.1 |
| `burstNs` | 分析突发窗口 (ns) | 250 | 0–1e6；步长1 |
| `sramFactor` | SRAM ECC / 对齐附加 (%) | 12.5 | 0–100；步长.1 |
| `simUs` | 仿真时长 (µs) | 12 | 1–100；步长1 |
| `simArrival` | 仿真发包节奏 | steady | steady / burst |

### 5.1 后端映射要求

| UI字段组 | 必须映射的模型与边界 |
|---|---|
| `functions/tagBits/tagBudget` | 本地Requester每Function身份/Tag；proxy和transparent分开 |
| `rate/*Lanes/*Meters/retimers/*Ns` | 每跳双向link及传播；禁止重复加入完整RTT |
| `channels/portWidth/clock/util` | 端口内部每方向服务；`util`是预算折减，详细模型不与显式气泡重复扣减 |
| `slices/fabricWidth` | 并行Fabric服务通道；不是物理Forwarding Slice数量 |
| `vc/headerCredits/dataCredits` | 参考模式均分窗口；timing模式转换为有backing的合法FC配置，Shared FC补参 |
| `cell/meta` | 内部cell与metadata，不等于外链FLIT或SRAM sector |
| `poolKiB/activeVoq/voqDepth/descriptor` | 每入口资源和每芯片descriptor预算分开；物理共享策略另声明 |
| `egressCells/fabricCreditNs/replayFlits` | 出口staging、内部slots、外链replay分别独立 |
| `trackMode/trackExtra` | transparent只可选监控；proxy才有本地Requester跟踪表 |
| `burst/burstNs/simArrival/simUs` | 解析突发预算与实际注入节奏、请求时长、测量窗口分开 |

`chipLanes=144/chipPorts=9`是UI默认值，不是唯一支持的产品配置。
硬件页没有定义物理3-Slice/3MiB/16-bank；这些只能作为可选microarchitecture
profile，不能覆盖`poolKiB=128`等UI输入或成为验收前提。

### 5.2 自动资源推导

`lab_reference`必须复现`sizeHardware()`，包括其向上取整和power-of-two
规则。以下为实现索引；运行输出应包含每项推导的输入、中间值与有效配置。

- 端口位宽：根据协议吞吐对应的Cell服务量与链路raw需求取较大者，除以
  频率、利用率、`channels`，按32 bit向上取整。
- Fabric位宽：`max(chipRaw, protocol对应双向Cell总流量)`乘裕量，除以
  频率、利用率、`slices`，按32 bit向上取整。
- H/D：按每链路share、目标事务率、credit RTT、裕量和VC均分计算；D至少
  可容纳最大单TLP数据。自动H/D取整数，不额外强制power-of-two。
- Replay：`ceil(rawGBps × ackRttNs / 256 × safety)`后向上取2次幂。
- VOQ/egress：最大单TLP Cell数与解析突发积压取最大，再向上取2次幂。
- 活跃VOQ：不超过`chipPorts*(chipPorts-1)*3*vc/chipPorts`。
- Payload pool：非零`poolKiB`为每入口显式容量；0按活跃队列×深度×
  Cell物理字节推导。`sramFactor`用于附加预算，不能冒充可存payload空间。
- Fabric slots：按每并行通道带宽×槽位返回时长/Cell物理字节×裕量推导。
- 跟踪表：Read在途数×裕量向上取2次幂；entry位宽及byte对齐按页面公式。

`pcie6_timing`导入这些建议值后还要验证协议backing与实现拓扑；自动
sizing值不是协议合规证明。不满足时输出诊断，不能静默扩大显式容量。

## 6. 计算、输出和时间口径

### 6.1 解析计算

必须输出每笔应用传输的MemRd/MemWr数量、每请求CplD分段、正反向TLP
字节/Cell/credit、在途MemRd、每Function Tag、理论payload上限，以及
按每发起端口折算的限制项排序。

参考公式：`ops/ns = targetGBps / payloadB`；
`N_memrd = ceil(ops × requests_per_transfer × rttNs)`。
Tag在对应MemRd最后CplD到达后释放，不在首包或应用入队时释放。

限制项至少包含：FLIT/TLP与链路cut、Tag、请求率、Completion率、端口
数据面、Fabric Cell服务、credit窗口、replay窗口。不能只报一个峰值。

RTT分`derived/fixed`：前者按往返传播/Retimer、Switch和端点预算计算；
后者是输入预算/测点，不抹掉物理路径。参考仿真保留HTML对`extraEndpoint`
的处理与额外逐跳串行化，故模拟延迟可能大于输入RTT。timing模式分项
计时并报告实际RTT，不用负残余抵消路径，不重复叠加完整测量RTT。

### 6.2 资源输出

必须对应页面`specView/memoryView`：SerDes/逻辑端口、每方向内部通道及
位宽、端口与Fabric有效带宽、Cell/metadata、VC/class、Fabric slots、
出口暂存、逻辑/活跃VOQ、descriptor SRAM、每入口/全芯片payload pool、
解析MTLP/s与每周期槽位、Tag与可选跟踪表。各链路分别输出credit/ACK
RTT、目标/配置H/D、突发积压、Replay需要/配置与TX replay SRAM。

### 6.3 事件结果

必须报告每发起端口payload GB/s、样本数、P50/P99/P999 ns、Tag/credit
stall次数、峰值Tag、VOQ/pool/egress/active-VOQ峰值及其容量，申请/实际
时长、预热、截短、已完成/未完成数量。零样本明确显示，不能伪造百分位。

HTML当前样本是**单个分段MemRd/MemWr**从首链路launch到最后CplD或写
到达的时间，不含入队前等待，亦非完整多请求应用传输时延。参考模式保留
此口径；timing模式另报含排队的事务及应用完成时延，禁止混合比较。

参考事件窗口前20%预热，存在30000 packet预算导致的时长截短；容量检查
是峰值比较而非阻止入队。timing模式则必须以有限容量实际反压，允许吞吐/
尾延不同，报告差异原因。两种模式分别显示“参考检查通过”和“有限资源
模型通过”，不能用前者证明后者。

### 6.4 性能目标矩阵：数值、适用profile与验收

性能需求同时包括“必须正确计算/测量”的基础交付和“架构应努力达到”的
数值目标。此前P-ID只描述输出、数值隐藏在COMP表，无法直接指导实现；
本表将二者统一。数值保留TARGET属性，不冒充规范要求或已验证结果；
COMP来源/条件不变。P-ID仍为P-01–P-13，不另换一套编号。

| ID | 性能目标/门槛 | 适用条件、测量与来源 | 架构/验收关联 |
|---|---|---|---|
| P-01 | Gen6 x16 raw单向128 GB/s；144-lane全部port TX raw合计1152 GB/s | 规范线速预算，不是payload或单个cut；详细模型扣实际协议开销 | 各方向Link服务与独立B_ref；V-23/35 |
| P-02 | 单跳空载L_core <100ns【TARGET】 | SW-LAT profile，普通MemRd、64/256B MemWr/CplD；入口合法单元至出口适配器接受，含排队/服务、不含PHY；COMP-05业务/架构目标 | core逐阶段预算；同/跨Slice分别测；V-23/35 |
| P-03 | 分项得到实际L_pin；90/100/125/150ns仅为探索档位 | ARCH扫描；含所选PHY/FEC/FLIT/CDC的模型或测量合同；无PHY证据不得输出pin实测承诺 | L_core之外的边界预算与未标定项明确，不能与P-02相加重复计费 |
| P-04 | RTT_XPU <250ns【挑战TARGET】 | RTT-64 profile、单hop/x16/64B Read、无retimer/错误/竞争；实际endpoint服务和测点固定；COMP-05 | 请求+返回+端点+wire预算闭合才可PASS；V-23/30/35 |
| P-05 | ≥4KiB大块payload达到独立合法协议/端点上限B_ref的≥95%【TARGET】 | SW-BULK；MPS/MRRS/RCB/packing与outstanding固定、无出口争用/重试；COMP-06，非95% raw | Link/Tag/Cpl/FC/replay与内部服务全部预算；V-23/24/35 |
| P-06 | 64B普通Write/Read各达到其独立B_ref的≥95%作为有限窗口工程门槛【TARGET】；理论追求合法线速 | SW-SMALL；分别报告IOPS/TLP/s/GB/s、请求/Completion两方向；95%为本项目ARCH验收容差，不是厂商保证；COMP-07 | 解析/descriptor/grant/cell处理率与字节率同时满足；V-23/24/35 |
| P-07 | 满端口合法置换无额外core瓶颈；整芯片payload≥95%对应B_ref总和【TARGET】 | SW-PERM；真实port映射、合法路由、无incast；COMP-07，不能用“Fabric纸面容量足够”代替实际吞吐 | VOQ/order/bank/Fabric/staging联合验证，含跨Slice置换；V-14/15/24/35 |
| P-08 | 70%独立可持续参考负载下P99 L_core≤2×同profile空载基线【TARGET】 | SW-TAIL，均匀非过订阅；COMP-08工程目标；热点/突发另报不外推 | 固定R_ref/分布/窗口/样本，报告排队分项和删失；V-12/23/24 |
| P-09 | 广播源只注入1份payload，扇出扫描2/4/8/16/32【可选TARGET】 | COMP-09；出口总量和慢成员占用计入，不承诺免费N倍BW | 多分支descriptor/refcount及实际复制服务；M5专门场景 |
| P-10 | 选定collective有净收益>1，扫描1.2/1.5/2×【可选TARGET】 | COMP-10/11；同拓扑合理软件算法/数值合同及同步成本，非全模型2×保证 | INC/端点/CCL服务与数据正确性先通过，M4/M5分层评估 |
| P-11 | 面积/功耗/温度目标待产品输入，当前无可冻结数值 | 不用Gen5典型功耗外推本项目Gen6；COMP-16为背景 | 只输出资源估计及独立PPA证据；当前指标未闭合 |
| P-12 | RAS恢复SLA/失败率待客户输入，当前无可冻结数值 | COMP-12/13功能方向不自动给出恢复时间指标 | 注错/隔离/恢复分别测；当前数值指标未闭合 |
| P-13 | x16→x16 cut-through ≤90ns【TARGET】 | CT-PIN，COMP-04对标；必须对齐包型/首末bit/PHY边界；S/F结果不冒充C/T | 安全C/T、RX/TX/PHY预算及实测/VIP证据，单独验收 |

所有目标报告`PASS/FAIL/UNSUPPORTED/NOT_COMPARABLE/NOT_RUN`及原因。
配置合法、RTL接口存在或输出了P99，均不表示数值目标达成。当前M0只
做配置合同，P-02/P-04–P-10/P-13尚无运行证据；P-11/12仍缺业务数值。

### 6.5 性能验收profile与独立分母

| Profile | 必须固定的实验输入 | 主要检查 |
|---|---|---|
| SW-LAT | 1GHz core探索点、单颗x16路径、64/256B包组合、同/跨Slice、无竞争/重试、时钟与CDC | P-02；baseline含实际stage和等待，不用固定switchNs证明目标 |
| RTT-64 | 一个64B Read、端点角色/内存服务、MRRS/MPS/RCB、链长、单hop、无retimer、单outstanding、无竞争 | P-04；起止为源提交至数据可消费，单独报告端口loop/core/endpoint |
| SW-BULK | 4KiB及更大应用、MPS至少256B档位、MRRS512/4096、合法RCB、充足窗口、Read/Write分列 | P-05；协议/端点独立B_ref，不以被测实现峰值作分母 |
| SW-SMALL | 64B Read/Write、合法header/packing、解析/完成率、Tag/FC/replay窗口 | P-06；每方向class混合与单源/多源分列 |
| SW-PERM | 9×x16候选、全部port实际映射、同/跨Slice的合法一对一置换、宽度/频率/Cell/metadata/ordering策略 | P-07；无目的竞争，所有参与端点服务能力明确 |
| SW-TAIL | 固定合法均匀矩阵、70% R_ref、与空载相同包型/路由/策略、相同时间戳 | P-08；先证明无积压增长再判断P99 |
| CT-PIN | C/T已实现，pin测点、合法转发阈值、PHY/FEC/packing/CDC及包型固定 | P-13；缺测点/PHY不能PASS |

R_ref/B_ref从合法协议开销、物理cut和固定端点能力独立推导，不包含
被测core达到的峰值；若core提前饱和，必须判目标失败/未闭合，而不是
降低分母后自证达标。profile必须保存完整实例；以上表格是必填约束，
未填端点/PHY/路由参数不作为已冻结、可直接运行的实例。

95%门槛用于有限观测窗口的工程验收，不等于允许协议丢5%数据：已接收
事务数据/身份/顺序必须100%正确，未完成/失败另报。P99工程采样门槛
至少10000个同口径完成样本，P999至少100000；低于门槛只报告样本分位
及不足标记，不宣传可靠尾延保证。此门槛是ARCH测试策略，不是统计置信
证明；warmup/measurement/drain与arrival cohort在运行前固定。

### 6.6 可行性门：输入目标不等于可达payload

HTML默认`target=128 GB/s`是用户需求输入。按其当前64GT/s x16、填充率
100%、236/256、请求16B/Completion12B假设，TLP区服务为118 GB/s：
64B Write参考上限94.4 GB/s；64B Read单对端返回受限上限约99.37 GB/s；
4KiB Read、MPS256B参考上限约112.72 GB/s。该默认输入已超这些上限，
正确输出是“目标受限”，不是让架构声称实现128 GB/s应用payload。

以上仅复核页面公式，不是严格FLIT模型/芯片实测。Read并发双向混合时
还要扣同一方向的请求/Cpl共享资源，不能把单对端99.37外推到所有port
同时双向发Read。严格B_ref使用实际header/OHC/packing/空闲等规则重算。

RTT也要先算残余：页面默认2×60ns switch预算+120ns endpoint+10ns
往返传播已经是250ns，未留额外序列化/对齐/排队空间。即使两次switch
各90ns，配合同样120ns端点和10ns传播已达310ns。P-04不能由P-02或
P-13推出；端点与其他项不降到剩余预算内，就明确标为不满足挑战目标。

## 7. 硬件页校验与容量验收

五个preset保留：64B Read、4KiB Read、4KiB/MRRS512、64B Write、8→1。
自动测试采用`runBench()`实际diff与阈值；不可用preset替代同名bench。

| ID | HTML校验用例 | `lab_reference`判据 |
|---|---|---|
| S-01 | 64B基线 | 在途500；实际吞吐>95%协议上限 |
| S-02 | 4KiB、MRRS4KiB | 在途8、CplD 16个；吞吐>95%协议上限 |
| S-03 | 4KiB、MRRS512 | 在途63、CplD 16个；吞吐>95%协议上限 |
| S-04 | 64B、32 Tags | tagStall>0、吞吐<20%协议上限 |
| S-05 | 64B、H Credit=1 | creditStall>0、吞吐<20%协议上限 |
| S-06 | 64B、256-bit端口 | 首限制为端口数据面，吞吐<20%协议上限 |
| S-07 | 8→1、egressCells=1 | queuePass=false，egress峰值超过配置；是参考容量失败检查 |
| S-08 | timing对应实验 | 缩小资源产生实际反压；无静默溢出；停发后可排空 |

参考判据依赖当前HTML默认值及其RTT/packing近似，不能写成所有合法
PCIe6配置的规范值。固定当前源文件版本/hash，输入变化后重新计算。

内部资源要求：A-01区分FLIT/Cell/sector；A-02端口和Fabric双重服务限制；
A-03外链信用与内部额度独立；A-04显式容量/自动建议/峰值分开；A-05复制
引用可选；A-06归约上下文可选；A-07拥塞原因、公平性与排空可观察。

## 8. JSON、校准与页面其他工作区

### 8.1 交换格式

保留`pcie6-hardware-calibration-v1`的字段：`inputs`、`readRttNs`、
`memRequests`、`protocolPayloadGBpsPerRequester`、
`estimatedPayloadGBpsPerRequester`、`switchSpec`、`sim`、`notes`。
`switchSpec`保留通道/位宽/Fabric/VC/VOQ/pool/egress/slots/tracking和
逐link的raw/creditRTT/ackRTT/replay/forward/reverse H/D。

导入器保留原始快照，逐字段映射；增加版本化metadata保存mode、来源hash、
backend版本、seed、单位和测量合同。旧字段中的估算和浏览器仿真不能
标成实测校准。`readRttNs=null`和`sim=null`是合法状态。

硬件页目前只导出该JSON，不提供gem5结果导入；这是新增adapter交付项。
顶栏workspace `{schema:1, config, versions, evidence}`是另一格式，
不得声称其现有导入能接收hardware-calibration或gem5 ResultBundle。

### 8.2 需求分析与辅助页面

需求分析保留Train/Prefill/Decode/Diffusion，TP/EP/DP/PP/SP、rank放置、
MoE热点、KV/权重流量、star/split/direct/mesh/ring/clos与原生互联模板。
遵循页面`GPU=TP×DP×PP`、`DP%EP=0`、`TP%SP=0`合同；端口/lane预算、
host staging和未知容量必须显式报告。graph模式连通不等于PCIe透明层次
合法；native链路不自动拥有PCIe事务语义。

平均Token/s需求和页面尾延估计先作为L0参考；只有加入完整依赖与compute
profile后才报告闭环TTFT/TPOT/step时间。方案对比、验证记录、历史版本
与实验hash绑定；网页已有数据不能被后端结果覆盖成不同配置的证据。

## 9. 实现顺序与完成标准

1. T-01：固定HTML/Base Spec版本，建立字段schema、规范条款与近似清单。
2. T-02：实现参考sizing与JSON适配；字段/自动值/限制项对照页面。
3. T-03：复现参考事件口径与S-01–S-07；证明差异可解释。
4. T-04：实现gem5有限资源timing及S-08，检查字节/Tag/信用/引用守恒。
5. T-05：按条款完善FLIT/shared FC/replay和ordering测试；只声明实测覆盖。
6. T-06：在gem5周期验收后接入SimAI/SimCCL闭环，再对接需求分析、对比
   与结果导入；通过SIM-01–SIM-16，保留现有离线能力。
7. T-07：需要时增加databook接口与配置/枚举细化；不倒置主次。
8. T-08：物理Slice/bank/缓存等microarchitecture扫描，可校准但无预置PPA。
9. T-09：多域、AI/CXL等扩展另立协议及验收合同。
10. T-10：RTL/VIP/实测校准保存测点、输入和误差范围，不等同gem5行为验证。

IP-01/02/03/04分别对应lane预算、PHY分组、逻辑端口和角色；IP-05域数；
IP-06级联/系统规模；IP-07 Tag/跟踪；IP-08数据面接口；IP-09管理/统计；
IP-10 PHY责任；IP-11 MPS/MRRS；IP-12诊断；IP-13故障；IP-14大radix。
这些ID均为参数/扩展索引，不预置竞品型号、Host数、功耗或量产承诺。

## 10. 证据与文档同步

Base Spec条款决定协议，HTML内嵌`hardware-sizing.mjs`、`hardware-sim.mjs`、
`hardware-page.mjs`决定参考功能和行为；`data/engine/topology`相关模块
决定需求分析合同。DWC补充章节见§4.3，原图/RTL不复制到本项目。

[Switch架构](PCIe6_Switch_Architecture.md)与
[gem5仿真架构](PCIe6_Simulation_Architecture.md)须遵循本文的来源顺序、
模式和验收范围。竞品驱动规格纳入§11及附录A作为补充需求，以独立profile进入扫描和评估；
不覆盖规范、HTML输入合同或首版验收范围。
本文件没有宣称已实现、已运行bench或已通过PCIe6合规。

## 11. 竞品驱动的补充规格与目标

本节保留此前竞品驱动的spec，作为产品候选目标和仿真探索输入，不删除
其数值方向。来源分为COMP（原厂规格/官方记录）、BIZ（业务目标）、
ARCH（架构自定门槛）、SIM（待仿真冻结）；不能把所有目标都称为竞品值。
同一项可同时有多个来源，但必须分别记录其含义。

以下新增COMP-ID避免改变§4/§6中F/P/IP的基础验收含义，相关ID仅为映射。
引用W/C/R编号与历史核查记录见附录A/B。本次恢复既有资料，未重新联网
核验供货状态、网页修订或产品性能，不将历史“已核实”表述延伸到当前。

| 补充ID | 候选spec / 扫描目标 | 来源属性与边界 | 仿真实验 / 基础需求映射 |
|---|---|---|---|
| COMP-01 | 64/104/144 lanes，144-lane为重点；9×x16、36×x4或72×x2等价预算 | COMP/BIZ；逻辑port/controller/PHY分叉限制另核对，不因lane数自动支持72端口 | lane/端口预算与fanout扫描；F-04、IP-01至IP-04 |
| COMP-02 | 144-lane版本4 Host、4 NTB；4虚拟分区为探索点 | COMP W12 / ARCH；Host、NTB、partition独立；作为多域产品目标保留，不是透明controller现成能力 | 独立NTB/地址转换/隔离profile；F-05、IP-05 |
| COMP-03 | 产品MPS候选1KiB；x2/x4/x8/x16及x1配置候选 | COMP W12；不是PCIe或DWC上限，HTML仍接受128B–4KiB与x1–x16 | MPS/宽度敏感性；F-02/04、IP-11 |
| COMP-04 | x16→x16 cut-through 90ns对标；本项目候选目标≤90ns | COMP W12 / TARGET；原厂测点、包型、PVT边界未完整给出，不能等同core latency | 后续C/T与PHY预算、同测点比较；P-13 |
| COMP-05 | 单跳L_core<100ns；RTT_XPU<250ns挑战目标 | BIZ/ARCH；不是由90ns竞品值推出；必须分别冻结测点 | 短包Read/Write与RTT分解；P-02至P-04 |
| COMP-06 | 大块payload吞吐≥95%独立协议模型上限 | ARCH TARGET；非95% raw、非被测实现自测峰值 | ≥4KiB、固定MPS/MRRS/packing、充足outstanding；P-05 |
| COMP-07 | 64B payload达到该事务组合合法协议速率；合法置换无额外Fabric容量瓶颈 | BIZ/COMP趋势、ARCH；Ethernet 64B帧不能直接当PCIe 64B payload | IOPS/TLP rate、Read/Cpl双向与全端口置换；P-06/07 |
| COMP-08 | 70%可持续均匀负载时P99 L_core≤2×空载基线 | ARCH/SIM；此前工程试验门槛，非竞品保证；热点另报 | 稳态矩阵、burst/incast、统计窗口与样本；P-08 |
| COMP-09 | 广播源注入1份，扫描扇出2/4/8/16/32；动态组256/1K/4K | BIZ/ARCH/SIM；组数与扇出不是竞品披露；消耗真实复制资源 | 慢成员、引用、地址权限与出口争用；F-10/11、P-09 |
| COMP-10 | INC组合ReduceScatter/AllGather；收益扫描1.2/1.5/2× | COMP W4的最高2×为厂商内部分析，其余为探索点；非端到端模型加速承诺 | 同拓扑合理算法基线、数值/同步/资源开销；F-12/13、P-10 |
| COMP-11 | INT32、FP32、BF16→FP32候选；context64/256/1024、chunk256B–4KiB | ARCH/SIM；不是Astera数据格式或容量披露 | 吞吐/累加器/数值/超时与并发扫描；A-06 |
| COMP-12 | DPC/eDPC、surprise-removal read tracking、FLIT计量/注错/trace | COMP W2/W12；具体代际/型号能力逐项核查，未实现不得显示已支持 | 故障域、在途请求回收及RAS扩展；F-18/19、P-12 |
| COMP-13 | 带内/I2C/MCTP管理；I3C/OpenBMC、安全升级、管理与数据面隔离候选 | COMP W11/W12与ARCH；软件/硬件责任及版本另立合同 | 管理接口与复位/更新隔离；F-09/20/21、IP-09/12/13 |
| COMP-14 | CXL3.1 Type-3 pooling候选、FM/CCI；PCIe KV appliance路径 | BIZ/COMP；PCIe6 Switch本身不证明CXL.mem/cache或pooling | 独立Host/Type-3/FM/OS矩阵；F-15/16、B-07 |
| COMP-15 | 16/32/64 XPU系统探索，160/320-lane后续radix | BIZ/SIM、COMP W11；不是当前单芯片规模或标准透明Clos承诺 | 合法树、独立fabric/NTB模式及端口预算；IP-06/14 |
| COMP-16 | Gen5 PEX89144/89104的115ns、49W/38W作代际背景 | COMP C1；典型功耗不是TDP；不能外推本项目Gen6 PPA | 明确型号/工艺/负载的后续PPA对照；P-11 |
| COMP-17 | Tomahawk Ultra 250ns、51.2Tb/s、64B帧线速作邻近对标 | COMP W8；Ethernet语义，不作为PCIe RTT、payload或信用合同 | 小消息/交换服务/可靠性结构对比，结果分协议 |
| COMP-18 | 众星微C82-4G：144 lanes、64GT/s、100ns级与CXL方向待核实 | A0历史业务输入；未取得一手手册/同口径测量，不列为已确认spec | 收集原厂来源与测点；仅显式假设扫描，不作硬约束 |

### 11.1 测量合同与冻结门

- `L_core`：入口协议适配器交付合法路由单元至出口适配器接受对应单元；
  含core阶段/仲裁/排队，不含外部PHY和端点。
- `L_pin`：RX输入指定首bit至对应TX首bit；含PHY/PCS/FEC/FLIT/CDC，
  必须声明包型、gen/width、packing和负载。90ns对标先补齐其测点。
- `RTT_XPU`：XPU提交请求至数据/状态可消费；与link loop或core往返分开。
  两次switch穿越、链路、端点、序列化和queue预算互斥，不重复计时。
- 每项附型号/stepping、固件、Host/端点、MPS/MRRS/RCB、class/VC、方向、
  首/末bit、窗口、负载、重试/加密、样本/P50/P99/max及PVT适用范围。

冻结记录必须包含：补充ID、来源及版本/页码、输入快照、模式、假设、
测量合同、验收方法、结果/误差、责任人和冻结状态。先进入显式
`competitor_target`实验profile，再经资源模型、RTL/VIP或实测证据评估；
只有产品负责人明确采纳并满足冻结条件后，才升级为对应版本的验收规格。

HTML默认配置与补充profile分别保存；加载竞品profile须显式选择，并显示
相对默认输入的变化。不把90ns、250ns、4 Host/4 NTB、1KiB MPS等隐式
覆盖到lab基线，也不通过固定输入延迟“证明”性能目标已达到。

## 12. gem5周期模型后的SimAI仿真需求

### 12.1 依据与必需闭环

依据SimAI侧
[PCIe6_Simulation_Architecture.md](/workspaces/SimAI/docs/pcie_switch/PCIe6_Simulation_Architecture.md)
的统一实验描述、WorkloadIR/FlowIR、完成依赖与API集成方案，以及当前
[AstraNetworkAPI.hh](/workspaces/SimAI/astra-sim-alibabacloud/astra-sim/system/AstraNetworkAPI.hh)
和[SimCCL架构](/workspaces/SimAI/SimCCL/docs/design/architecture.md)。
协议规则仍以§1的Base Spec为准；SimAI资料定义集成功能，不替代协议依据。

必须实现：SimAI compute/communication DAG → SimCCL或显式collective
算法 → 有依赖的FlowIR → 同一gem5 DMA/TLP/Link/Switch/Endpoint → 完成
callback → 后续任务。SimAI调度workload，gem5执行PCIe时序；不复制
SimAI提案中的第二套PCIe事件核心。集成实现见本地仿真架构§11。

### 12.2 功能与验收矩阵

所有SIM项均为集成阶段必需，不等于目前已实现。M3入口门与M4验收
分别记录，未支持M5扩展不阻塞基础PCIe-only闭环。

| ID | 必须满足的仿真要求 | 验收场景 / 阶段 |
|---|---|---|
| SIM-01 | SimAI执行前先验收gem5周期profile：解析TLP/cycle、bank/RAM、仲裁/Fabric/staging、时钟/CDC、Link/所选FC/replay/ordering；记录未支持项 | V-35及M3协议场景通过，M3-06冻结profile/hash；预算模型或仅编译不算通过 |
| SIM-02 | 微基准和SimAI流量共用同一gem5端点/Link/Switch状态与实现，集成层只作匹配、流量映射、事件和结果适配 | V-30同配置/实际注入对照，无无法解释的路径/计数/时间差异；M4-06/08 |
| SIM-03 | 实现PCIe AstraNetworkAPI backend，覆盖init/comm/rank/send/recv/time/schedule/finish及backend分支 | V-27/28/33/34，构建与运行明确选中PCIe；API调用与count/type到byte规则冻结 |
| SIM-04 | 所有rank共用一个gem5时间与event queue；相对schedule、时间单位/分辨率、Tick换算与同Tick排序确定 | V-27；无逆因果、负delta/溢出静默处理或独立NS3/AnaSim时间；不以wall-clock推进 |
| SIM-05 | 匹配区分communicator、src/dst、message tag、flow/channel/chunk或序号；支持recv先到/data先到与重复tag并发 | V-28，匹配恰好一次且不串流；消息tag不作PCIe Tag |
| SIM-06 | 明确send源可复用、recv完整目的可见/匹配及应用同步完成；callback恰好一次、生命周期安全 | V-29/33，posted write不虚构Cpl；Read-pull方向正确且全部Cpl齐后完成；禁止callback过早推进依赖 |
| SIM-07 | 有限DMA/待注入/匹配状态与上游注入窗口明确；不能假定SimAI会按sim_send返回值自动重试 | V-33容量与恢复场景；无无限payload队列、静默拒绝/丢流或无依据的提前完成 |
| SIM-08 | FlowIR保存algorithm/protocol/channel/chunk、communicator、placement及predecessors；一flow到多请求/TLP可追溯 | V-28/31/32，flow→transaction→cycle trace身份/字节/依赖一致 |
| SIM-09 | PCIe-only算法选择拒绝NVLS/NVSwitch专属路径；软件归约在端点计服务，不隐式启用Switch INC/广播 | V-31；2/8-rank Ring及显式Tree的阶段和字节正确；缺能力明确拒绝 |
| SIM-10 | PCIe路径不继承NS3的RDMA/PFC/重传或NVLink/NET send_lat；如有端点启动成本须显式profile且避免重复计费 | V-30/31；独立空载/单流预算与逐段时延可核算；旧backend自身行为保留 |
| SIM-11 | 网络排队/资源停顿反馈SimAI DAG；任务仅在其完成合同满足后释放，不能以入队替代完成 | V-32；受控拥塞/降带宽延迟相关通信及后继compute，字节/依赖守恒；M4-10 |
| SIM-12 | 工作负载范围包括显式P2P/collective、MoE Dispatch/Combine及Train/Prefill/Decode/Diffusion的有依赖样例 | V-31/32各自fixture；先小规模再扩展；缺计算profile只输出通信指标，CSV无依赖只能称replay |
| SIM-13 | 单透明star/树先验证，rank→endpoint/Function与地址窗口显式；级联/P2P按合法域转发，Host staging生成两段DMA及Host服务 | V-09/15/30/32；非法预算/不可达拒绝；mesh/Clos另有专用转发合同才启用 |
| SIM-14 | 输出通信/collective/模型时间、吞吐、逐link热点与硬件stall/占用，并绑定两仓库/SimCCL版本、profile/seed/窗口/测点 | V-20/26/30/32；有compute才报step/TTFT/TPOT；输入Token/s不作达成值，重试字节不计有效量 |
| SIM-15 | finish由共享runtime统一管理，全部rank及在途消息/callback/硬件资源按合同drain后退出；未匹配/超时显式报告 | V-33：错时finish不提前exit、无重复回调/幽灵完成，未完成量和失败原因可审计 |
| SIM-16 | 独立PCIe构建入口与两仓库bridge可重现；保留Analytical/NS3原有选择/行为，不修改临时复制目录实现持久功能 | V-34；指定版本build/link/smoke及原backend基线回归，所有结果记录版本与dirty状态 |

### 12.3 验收范围与场景补充

先运行同硬件profile的P2P Read/Write、64B/4KiB和incast，证明adapter复用
周期数据面；再运行2/8-rank Ring、显式Tree和小规模MoE Dispatch/Combine。
Ring每rank有效发送量按`2×(N−1)/N×tensor_bytes`核对，Tree使用独立
expected；数据/算法策略和端点归约成本固定，不混为INC硬件收益。

Train、Prefill、Decode、Diffusion分别提供最小可重放DAG fixture，包含
适用的compute、通信与场景依赖。MoE按token/专家/rank映射核对Dispatch/
Combine总量；热点不能仅靠平均倍率替代。每个DAG以无争用/受控争用两组
输入验证通信完成影响后继时刻；统计窗口/误差与compute输入来源预先声明。

V-27–V-34是SimAI接口/闭环场景，V-35是其前置cycle验收。全部适用必需
case无FAIL/skip/xfail、required bins全命中、数据/身份/资源错误为0；
超时/缺能力/测点不可比单列，不静默删除未完成样本。参考HTML结果不得
充当严格cycle模型或SimAI闭环的正确性oracle。

### 12.4 交付状态与规模边界

当前所有SIM项均未实现/未验收，文档更新不表示backend可用。交付包括
两仓库版本化API/FlowIR/结果合同、可执行TDD场景、gem5 runtime facade、
SimAI PcieNetwork/构建入口、collective/workload fixtures及回归报告。

大规模可后续使用经周期模型校准的加速profile，但须独立标精度和适用
范围，按消息大小/并发/Read-Write/拓扑/热点给出误差；只导出单一BW/
latency标量不满足本节。首版无需1024卡全程逐FLIT，也无需先完成NTB、
CXL、INC、完整OS或PHY电气模型才能交付基础SimAI闭环。

## 附录A：历史竞品资料记录（补充依据）

本附录恢复此前文档内容，保留型号、原始链接和证据边界；其中“可信/可确认”
均指历史核查记录，不表示本次已重新核实。§11决定如何转为本项目补充需求。


### A.1 此前记录的竞品规格与性能指标

| 厂商 / 产品 | 协议 / 单lane速率 | lane数 / 端口数 | 交换时延 | 典型功耗 | 其他明确规格 / 性能 | 原始来源 |
|---|---|---|---|---|---|---|
| **Broadcom PEX89144** | PCIe 5.0 / **32GT/s** | **144 lanes / 72 ports** | **115ns** | **49W** | **8个NT ports；47.5×47.5mm封装** | [C1 官方选型表第1页](https://docs.broadcom.com/doc/BC00-0445EN) |
| **Broadcom PEX89104** | PCIe 5.0 / **32GT/s** | **104 lanes / 52 ports** | **115ns** | **38W** | **8个NT ports；42.5×42.5mm封装** | [C1 官方选型表第1页](https://docs.broadcom.com/doc/BC00-0445EN) |
| **Broadcom PEX90144** | PCIe 6.x / **64GT/s** | **144 lanes / 72 ports** | **系列cut-through 90ns（x16→x16）** | 未确认 | PCI-SIG有**x16、64GT/s**互操作记录，条目日期**2026-08-03**；72 ports来自Broadcom目录；时延来源W12 | [W12 系列简报](https://docs.broadcom.com/doc/PEX90000-PB)；[W3 PCI-SIG名录](https://pcisig.com/developers/integrators-list?field_il_comp_product_type_value=All&keys=amd&order=field_il_vendor_name&sort=desc)；[Broadcom目录](https://www.broadcom.com/?articleId=487&page=presspage) |
| **Broadcom PEX90104** | PCIe 6.0 / **64GT/s** | **104 lanes / 52 ports** | **系列cut-through 90ns（x16→x16）** | 未确认 | **4个NT ports；x16原始带宽128GB/s单向、256GB/s双向合计**；时延来源W12 | [W12 系列简报](https://docs.broadcom.com/doc/PEX90000-PB)；[W2 官方产品页](https://www.broadcom.com/products/pcie-switches-retimers/expressfabric/gen6/pex90104)；W1 |
| **Astera PF63201L** | PCIe **6** | **320 lanes** | 未确认 | 未确认 | **62.5×62.5mm** | W11第3页 |
| **Astera PF61601L** | PCIe **6** | **160 lanes** | 未确认 | 未确认 | **45×45mm** | W11第3页 |
| **Astera PF60641L** | PCIe **6** | **64 lanes** | 未确认 | 未确认 | **29×29mm** | W11第3页 |
| **Astera PF60481L / PF60321L** | PCIe **6** | **48 / 32 lanes** | 未确认 | 未确认 | **21×21mm** | [W11 2026年5月简报第3页](https://www.asteralabs.com/wp-content/uploads/2026/05/FB_Scorpio_Smart_Fabric_Switches.pdf) |
| **Astera Scorpio P-series** | PCIe **6系列** | **32–320 lanes系列范围** | 未确认 | 未确认 | 2026-05-05公告；各型号端口配置分别确认 | [W4 官方公告](https://www.asteralabs.com/news/astera-labs-extends-leadership-in-open-ai-scale-up-networking-with-new-320-lane-scorpio-x-series-smart-fabric-switch/) |
| **Astera Scorpio X320** | Memory-semantic Fabric；具体端口协议按型号确认 | **320 lanes** | 未确认 | 未确认 | **Hypercast＋INC；AllReduce最高2×加速【厂商内部分析】** | [W4 官方公告及脚注1](https://www.asteralabs.com/news/astera-labs-extends-leadership-in-open-ai-scale-up-networking-with-new-320-lane-scorpio-x-series-smart-fabric-switch/) |
| **Broadcom Tomahawk Ultra【增强Ethernet邻近对标】** | Ethernet | **64×800GbE / 128×400GbE / 256×200GbE** | **250ns** | 未确认 | **51.2Tb/s交换容量；64B帧线速；LLR、CBFC、硬件INC、最小12B AI Fabric Header** | [W8 官方产品页](https://www.broadcom.com/products/ethernet-connectivity/switching/strataxgs/bcm78920-series) |

【指标边界】

- **90ns**为PEX90000系列x16→x16 cut-through公开值；不将其无条件指定为某一型号所有配置或全PVT最大值，测量边界待补。[W12]
- **115ns**为Gen5原厂选型表的Latency字段；原表未完整披露包型、负载、首末bit测点和PVT，不作为Gen6或最大pin-to-pin指标。
- **49W / 38W**为原表典型功耗，不是最坏工况或TDP。
- **128 / 256GB/s**为Gen6 x16原始线速，不是持续应用payload吞吐；双向合计不能当成单向带宽。
- **AllReduce最高2×**相对传统Ring AllReduce，来自厂商内部对ReduceScatter与AllGather卸载的分析；不代表模型整体、任意工作负载或All-to-Allv均加速2×。
- **Tomahawk Ultra 250ns**为Ethernet switch latency，不是PCIe或XPU端到端RTT；64B是Ethernet帧口径。
- **32–320 lanes**是系列范围，不表示任意lane/端口组合均可配置或订购。PF60641L的2025年历史状态不代表当前供货状态。

【证据分级】A1＝原厂产品规格；A2＝官方互操作记录；A3＝厂商分析；A0＝未取得一手证据。A1/A2不等于本项目独立实测，A3保留原分析条件。

历史文档记录的核对日期：2026-10-28；本次仅恢复原记录，未重新验证。GB/s均为十进制；未确认字段保留“未确认”，不补估计值。国内竞品待核实项见A.4。

### A.2 PCIe Switch容量、接口与产品特性

| 产品/资料日期 | 可确认参数 | 性能字段/口径 | 证据与限制 | 对应需求 |
|---|---|---|---|---|
| Broadcom PEX90000，PB105（2026-08-27） | 32/64/80/104/144-lane系列；PCIe6.3；4 Host/4 NTB | x16双向raw 256GB/s；系列raw 9.2Tb/s；90ns cut-through（x16→x16） | A1，W12；9.2Tb/s保留厂商raw口径，不再乘二，不等于应用payload或bisection | F-04/05、IP-01、P-13 |
| Broadcom PEX90144，PCI-SIG条目2026-08-03 | 144 lanes；Gen6、64GT/s、x16测试记录；Broadcom公开目录列72 ports | 系列90ns cut-through（x16→x16），W12；型号级边界、功耗与payload吞吐待补 | A1/A2，W3；x16测试记录不说明所有端口组合均通过，也不证明供货状态 | IP-01/03、P-01 |
| Broadcom PEX90104，官方产品字段 | 104 lanes、52 ports、4 NT ports；DPC/read tracking、embedded CPU、Shared I/O、Serial HPC、SRIS/SRNS/CIKS | x16 raw 256GB/s双向；系列90ns cut-through（x16→x16），W12 | A1，W2；仅适用该型号，不推给PEX90144 | F-05/08/09 |
| Broadcom PEX89144，Gen5 Selection Guide | Gen5 32GT/s、144 lanes、72 ports、8 NT ports | Latency栏115ns；typical power 49W；package 47.5×47.5mm | A1，C1第1页；原表未完整给出包型/首末bit/负载/PVT，不能标作pin最大时延，也不能当Gen6值 | 时延代际背景、T-02/08 |
| Broadcom PEX89104，同一Gen5表 | 104 lanes、52 ports、8 NT ports | Latency栏115ns；typical power 38W；package 42.5×42.5mm | A1，C1第1页；同上，不能用其38W估算Gen6功耗 | 容量/PPA对比背景 |
| Astera Scorpio PF60641L，April 2025简报 | PCIe6.x、64 lanes、29×29mm；当时列Pre-Production | P2P line-rate/non-blocking为厂商特性描述；没有精确ns或GB/s payload实测 | A1，R01第2页；该简报X-series为Customer Specific，不能从该PDF读出X320规格 | F-03/08/09 |
| Astera Scorpio P-series，2026-05-05公告 | 扩展系列32–320 lanes | 精确latency、功耗及每型号端口组合：未确认 | A1，W4；系列跨度不是任意lane组合都可订购的保证 | IP-01、后续radix |
| Astera Scorpio X-series 320，2026-05-05公告 | 320 lanes；memory-semantic Fabric；Hypercast与INC | AllReduce最高2×属于A3，条件见A.3；精确交换ns未确认 | A1/A3，W4；X与P的协议与特性分别核查 | F-10/11/12 |

### A.3 性能参数专项

| 指标 | 可信来源及明确数值 | 测试/分析边界 | 本需求中的使用方式 |
|---|---|---|---|
| Gen6 PCIe Switch cut-through | PEX90000系列：**90ns，x16→x16**；W12第2页 | 原厂公开规格；未完整定义包型、时戳与负载 | 对应P-13；与L_core/L_pin/RTT分别计量 |
| Gen5 PCIe Switch latency | PEX89144/PEX89104：115ns，C1第1页 | 厂商选型表；测点和流量条件未展开 | 背景基准；不证明Gen6<100ns，更不证明RTT<250ns |
| Gen6 x16带宽 | PCIe标准raw 128GB/s单向、256GB/s双向，W1/W2 | 协议线速；未扣TLP/FLIT/空闲/重试 | 带宽上限记账，不写作应用持续吞吐 |
| INC/Hypercast收益 | Astera X320：AllReduce最高2×，W4脚注[1] | 厂商内部分析；相对传统Ring AllReduce，将ReduceScatter与AllGather卸载；结果随workload/配置/规模变化 | 验证P-10，不能将2×移植到整个模型或任意All-to-Allv |
| Ethernet switch latency | Tomahawk Ultra：250ns，W8 | 厂商switch latency；不是XPU端到端RTT；包型/时戳需实测补全 | 增强Ethernet邻近基准，与PCIe单独列口径 |
| Ethernet小帧吞吐 | Tomahawk Ultra：51.2Tb/s、64B帧线速；端口64×800GbE/128×400GbE/256×200GbE，W8 | Ethernet帧定义，非64B PCIe payload事务；厂商公开特性 | 形成小消息吞吐专项P-06，不直接复制数值 |

### A.4 国内竞品与未确认字段

众星微C82-4G纳入重点对标对象【A0】。已有输入涉及144 lanes、64GT/s、100ns级和CXL池化；在取得原厂产品手册或可复现实测之前，不归入A.2/A.3可信数值表。需闭合：最大端口组合、CXL版本及mem/cache范围、池化FM、64B定义、100ns测点/包型/负载、INC/广播是否属于该型号，以及量产/样片状态。

同口径测量记录包含：型号/stepping、固件、Link速率/宽度、MPS/MRRS、P/NP/Cpl组合、FLIT packing、报文大小、Host/XPU、方向、负载、FEC/重试/加密状态、首末bit定义、P50/P99/max、温度与供电。未填写的字段视为不可直接比较【待测】。


## 附录B：竞品原始来源索引（历史记录）

原编号保留以便追溯；本次不宣称链接内容和产品状态仍与历史记录相同。


- [W1] PCI-SIG，PCI Express 6.0 Specification及FAQ：64GT/s、PAM4、256B FLIT、FEC/CRC、x16双向256GB/s、FLIT/non-FLIT及Tag说明。[规范概览](https://pcisig.com/pci-express-6.0-specification)；[FAQ](https://pcisig.com/faq?field_category_value%5B%5D=pci_express_6.0&keys=)。完整合规条款须以项目获得的正式规范与ECN为准。
- [W2] Broadcom，PEX90104官方产品页：104 lanes/52 ports/4 NT ports与管理/RAS特性。[产品页](https://www.broadcom.com/products/pcie-switches-retimers/expressfabric/gen6/pex90104)
- [W3] PCI-SIG Integrators List，PEX90144 144-lane PCIe6、64GT/s x16，条目日期2026-08-03；Broadcom官方公开目录列144 lanes/72 ports。名录说明互操作记录，不代替所有性能和供货认证。[PCI-SIG名录](https://pcisig.com/developers/integrators-list?field_il_comp_product_type_value=All&keys=amd&order=field_il_vendor_name&sort=desc)；[Broadcom目录](https://www.broadcom.com/?articleId=487&page=presspage)
- [W4] Astera Labs，2026-05-05，Scorpio X-Series 320 Lane公告，含P-series 32–320与脚注[1]的AllReduce内部分析条件。[官方公告](https://www.asteralabs.com/news/astera-labs-extends-leadership-in-open-ai-scale-up-networking-with-new-320-lane-scorpio-x-series-smart-fabric-switch/)
- [W5] UALink Consortium，2025-05-15，UALink 200G 1.0 Specification Overview；1024 endpoints、station和内存语义。[官方概览](https://ualinkconsortium.org/blog/ualink-200g-1-0-specification-overview-802/)
- [W6] CXL Consortium，CXL Fabric Management；pooling、FM/CCI与Host资源绑定。[官方说明](https://computeexpresslink.org/blog/cxl-fabric-management-1089/)。该文章不代表所有CXL3.1可选能力均已选入本IP。
- [W7] OCP，Scale-Up Ethernet Transport工作组与ESUN介绍。[SUE-T](https://www.opencompute.org/projects/semi-private-workstreams/scale-up-ethernet-transport/)；[ESUN](https://www.opencompute.org/projects/semi-private-workstreams/ethernet-for-scale-up-networking/)
- [W8] Broadcom，Tomahawk Ultra / BCM78920官方产品页；250ns交换时延、64B帧能力及LLR/CBFC/INC。[官方产品页](https://www.broadcom.com/products/ethernet-connectivity/switching/strataxgs/bcm78920-series)
- [W9] Alibaba Cloud SimAI官方仓库、ASTRA-sim项目主页；仅用于说明工具来源和模型方法，不声称其原生具备本IP全部协议模型。[SimAI](https://github.com/aliyun/SimAI)；[ASTRA-sim](https://astra-sim.github.io/)
- [W10] Astera Labs，Why Your Mixture-of-Experts Model Is Only as Good as Your Fabric；动态多播、专家映射与组配置的需求依据，厂商分析非通用性能保证。[官方技术说明](https://www.asteralabs.com/resources/blog/why-your-mixture-of-experts-model-is-only-as-good-as-your-switch/)

- [W11] Astera Labs，Scorpio Smart Fabric Switch Family Brief，May 2026，5页。[官方PDF原页](https://www.asteralabs.com/wp-content/uploads/2026/05/FB_Scorpio_Smart_Fabric_Switches.pdf)。第2页应用/管理；第3页型号/封装；第4页八卡/KV；第5页运维诊断。型号规格以本简报更新，R01保留为历史版本。
- [W12] Broadcom，PEX90000 Series Product Brief，PEX90000-PB105，2026-08-27，2页。[官方PDF原页](https://docs.broadcom.com/doc/PEX90000-PB)。第1页NTB/Host/Fabric；第2页配置、90ns cut-through、1KiB MPS及诊断。厂商系列规格不等于本IP实测或所有配置保证。


- [C1] Broadcom，[PCI Express Switches Selection Guide, BC00-0445EN](https://docs.broadcom.com/doc/BC00-0445EN)，第1页Gen5参数；原表可确认115ns等数值，测试边界未完整给出。

R01历史来源：

| 编号 | 原尾注 | 文献与入口 | 边界 |
|---|---|---|---|
| R01 | 63 | Astera Labs, Scorpio Smart Fabric Switch Portfolio Brief, April 2025，[PDF](https://www.asteralabs.com/wp-content/uploads/2025/05/PB_Scorpio_Smart_Fabric_Switches.pdf)，第2页 | 混合I/O与P2P/RAS/telemetry；历史产品状态不代表2026当前状态 |
