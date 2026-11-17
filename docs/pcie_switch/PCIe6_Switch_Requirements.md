# PCIe 6.0 Switch IP 需求定义

版本：v0.6（需求定义基线）｜日期：2026-10-28

## 1. 定位与规格结论

面向 AI 服务器板内与机箱内互连，开发可参数化的 PCIe 6.0 Switch IP：每 lane 64 GT/s，PAM4、256B FLIT、FEC/CRC，支持标准 PCIe 层次交换、P2P、多主机隔离与可管理 Fabric。IP 覆盖 64/104/144 lanes 的候选集成配置，144 lanes 作为目标集成配置【TARGET】。设计上预留广播复制、INC 与 CXL 协议适配接口；CXL 内存池化、多节点 Scale-Up、UALink 和 SUE-T 分别定义交付范围与验证条件。

**规格状态：**144 lanes 是产品候选容量；单跳片上 core latency <100ns 是架构目标；XPU 端到端 RTT <250ns 是受严格边界约束的挑战目标。三者均不是已实现规格。首代必须确保 PCIe 基本功能与互操作闭合；广播、INC 和 CXL 需明确版本、端点及软件依赖后进入相应版本的验收基线。

### 1.1 状态、优先级与来源

| 标记 | 含义 | 使用规则 |
|---|---|---|
| MUST | 对所选交付版本必须满足 | 按验证证据冻结；本文件不是硅验证证明 |
| TARGET | 候选工程目标 | 须经模型、RTL/PPA 或实测验证后才能承诺 |
| STUDY | 前瞻或可选能力 | 明确触发条件后另立验收范围 |
| STD | 标准要求 | 标准版本、ECN、勘误应进入合规矩阵 |
| COMP | 竞品基准 | 必须带型号、来源和测量边界；厂商宣称不等于独立实测 |
| BIZ | 业务输入 | 需求来源，合同状态另行记录 |
| ARCH / CALC | 架构选项 / 数学推导 | 不应写成来自厂商或既有项目决策 |
| SIM | 等待仿真或测量冻结 | 本文未运行 SimAI、ASTRA-sim 或芯片仿真，无模拟结果 |

### 1.2 与已有资料的继承关系

sharpfabric 中《锐联半导体_商业计划书.md》§3.3、§3.5、§4.1 与《OrbfabriX-v0.34-roadmap-architecture-v2.pptx》第6–7页把 E-series 定义为电分组交换，先从板内/机箱内进入，再向多协议、柜内互连与 INC 扩展。[I1][I3]

本需求继承阶段化路线，但做以下澄清：

- 原资料 E32/E64 中的数字是交换容量编码，不能解释为 PCIe lane 数；原 E32 有 64-lane 配置，不能把“144 lanes”误写为原方案已有配置。
- 原计划书含“3.2T/6.4T”及有效带宽假设，不能直接用于本 PCIe IP 的线速定义。本文件按每 lane 8 GB/s 单向原始线速重新记账。
- 144 lanes 是扩展配置【TARGET】；与原 64-lane 首代路线之间的面积、SerDes 数量、封装、功耗和时间差异列入T-08【待验证】。
- 统一 Fabric 内核不等于协议语义统一。PCIe、CXL、UALink、Ethernet 需要各自的协议适配与一致性/顺序处理。
- E-series 中的数据广播是电分组复制；不等同于后续 M-series 的 WDM 光广播 Mesh。[I1][I3]
- 历史报告包含不同代际、日期的竞品和带宽口径；本文件以当前官方来源核对可确认指标，不继承未验证数值。[I2]

## 2. PCIe Switch应用场景与需求拆解

### 2.1 应用场景

**SC-01：单Host八卡AI服务器。** CPU经一个x16上行口连接Switch，八个x16下行口连接XPU，共144 lanes。CPU执行枚举、控制与数据准备；XPU之间通过合法P2P搬运activation、参数分片或中间结果。Host方向的集中数据装载受上行带宽限制，XPU间通信受目的端口和内部Fabric限制。边界条件为Host/IOMMU允许P2P、设备BAR可映射、驱动支持。拆解为标准枚举/路由、P2P、ACS可配置、64-bit地址和混合流量仲裁，对应F-01/02/03/06及P-02/05/07。

![SC-01 应用场景](figures/sc01_eight_xpu.svg)

**SC-02：XPU/NIC/SSD混合I/O服务器。** Switch同时连接CPU、XPU、NIC和NVMe SSD；工作负载包括网络输入、存储读取、XPU间搬运与CPU控制。大块DMA与小消息并存，Gen5/Gen6设备可能共存。不能以全端口大包峰值代替小消息P99。拆解为混代与分叉、目的队列隔离、P/NP/Cpl信用、QoS、热插拔和DPC，对应F-01/04/06/08及P-05/06/08。

![SC-02 应用场景](figures/sc02_mixed_io.svg)

**SC-03：多Host可组合资源池。** 两个或四个独立RC/OS域接入同一芯片，各Host枚举分配给自身的设备；跨域通信通过NTB窗口或明确定义的Fabric接口进行。资源重新分配涉及设备停止DMA、地址权限撤销、复位和重新枚举。共享物理Switch不意味着设备可以同时被任意Host直接驱动。拆解为虚拟分区、NTB、地址窗口、租户隔离、域级复位与管理事务，对应F-05/07/08/09/17及IP-05。

![SC-03 应用场景](figures/sc03_multihost.svg)

**SC-04：TP/DP集合通信加速。** 多XPU执行AllReduce、ReduceScatter、AllGather与Broadcast。普通PCIe版本提供P2P单播；AI版本通过专用descriptor、数据复制与INC减少源端重复发送和中间数据往返。计算位置改变后必须重新验证数值、顺序和失败恢复。拆解为组成员、epoch、复制队列、归约算力、上下文和CCL插件，对应F-10/11/12/13及P-09/10；仅在相同拓扑与合理算法基线上计算收益。[W4][R02][R04]

![SC-04 应用场景](figures/sc04_collective.svg)

**SC-05：MoE推理与训练的EP通信。** dispatch按照每token选择的专家将activation发送到动态XPU集合，combine将专家结果返回并组合。专家不均匀分布会产生热点和突发；多个专家位于同一XPU时必须去重目的设备。通信主体为All-to-Allv，重复数据子集可以使用多播，其余仍为点到点。拆解为动态目的描述、组配置时延、队列公平性、热点隔离和多播慢成员策略，对应F-10/11/14、A-04/05及S-04。[W10][R03]

![SC-05 应用场景](figures/sc05_moe.svg)

**SC-06：CXL内存容量池与冷KV层。** 支持CXL的Host经CXL Switch使用Type-3设备，FM分配内存资源并维护Host域。GPU直接访问、CPU中转或DMA搬运形成不同数据路径，须分别建模；内存池不等于HBM扩容后的同等带宽/时延。拆解为CXL.io/mem、FM/CCI、地址映射、LD隔离、RAS和OS绑定流程，对应F-15/16、B-06及S-07。[W6][R05][R06]

![SC-06 应用场景](figures/sc06_kv_memory.svg)

**SC-07：16/32/64 XPU扩展系统。** 多颗Switch通过显式级联接口连接，系统同时约束每XPU带宽、跳数、Host域、布线与故障域。标准PCIe版本采用合法层次；多路径Clos需要独立Fabric模式、地址域与顺序规则。拆解为拓扑管理、级联带宽、NTB/分区、路径一致性和故障隔离，对应F-05/06/08/09、IP-06与T3/32-XPU拓扑。UALink/SUE-T属于后续协议扩展，不将其系统节点上限写成PCIe芯片端口数。[W5][W7]

![SC-07 应用场景](figures/sc07_scaleup.svg)

### 2.2 场景—业务需求—规格映射

| ID | 场景 | 所需能力 | 决定哪些规格 | 来源/状态 |
|---|---|---|---|---|
| B-01 | SC-01：4/8 XPU 服务器内部通信 | 标准 P2P、低时延、小包高吞吐 | x16 数量、lane 总数、内部带宽与仲裁 | BIZ、I1；MUST |
| B-02 | SC-02：CPU–XPU–NIC–SSD 混合 I/O | 扇出、隔离、Gen5/6 混合与 QoS | 上行配比、分叉粒度、buffer、错误隔离 | I1、W2；MUST |
| B-03 | SC-04：TP/DP 集合通信 | AllReduce、ReduceScatter、AllGather、Broadcast | 广播扇出、INC 算力、上下文、数据格式 | BIZ、W4；TARGET |
| B-04 | SC-05：MoE EP dispatch/combine | 不均匀 All-to-Allv、热点流量、动态目的集合 | 队列隔离、动态多播、组表更新机制 | BIZ、W10；TARGET |
| B-05 | SC-03/07：16/32/64 XPU 多节点系统 | 多主机、地址域隔离、通信库与拓扑管理 | NTB、域数、级联、跳数与带宽配比 | BIZ、I2；TARGET |
| B-06 | SC-06：内存容量扩展/池化 | CXL.mem 交换、FM、隔离与 RAS | CXL 版本、Host/LD 数、地址映射、内存端点 | BIZ、W6；独立 CXL 版本 |
| B-07 | SC-07：后续 rack-scale Fabric | 高 radix、多 plane、光电延伸、可扩展语义 | 下一代 lane 数、适配器与物理接口 | I1、W5/W7；STUDY |

### 2.3 系统与语义边界

“节点”统一拆为 Host（独立 RC/OS 域）、XPU（加速器）、设备端口、交换芯片和 Fabric 端点，所有数量均应指明对象。144 lanes 不能推出 144 节点，也不能推出单芯片无阻塞连接 144 个 XPU。

多品牌 XPU 接入仅保证已验证 PCIe 互操作范围；应用层协同还依赖 BAR/P2P、IOMMU、ACS 策略、驱动和通信库。PCIe Switch 不自动提供跨 XPU 硬件 cache coherence，也不自动让 GPU 直接使用 CXL.mem。[I2]

### 2.4 官方产品简报与应用映射

- **[Astera Labs Scorpio Smart Fabric Switches — Family Brief，May 2026，5页](https://www.asteralabs.com/wp-content/uploads/2026/05/FB_Scorpio_Smart_Fabric_Switches.pdf)** [W11]。应用与拓扑见第2–5页：P-series覆盖混合I/O、8/16 GPU服务器与KV Cache解聚；X-series覆盖高radix单跳/多级Scale-Up。分别关联SC-01/02/05/06/07。
- **[Broadcom PEX90000 Series — Product Brief，PB105，2026-08-27，2页](https://docs.broadcom.com/doc/PEX90000-PB)** [W12]。第1页为多Host与Fabric，第2页为协议、诊断、接口及90ns cut-through指标。关联SC-01/02/03/07。

【场景补充】SC-02/06增加经PCIe/NIC/SSD访问的KV Cache memory/storage appliance；该路径可以独立于CXL，访问协议和GPU/CPU中转分别定义。SC-03增加维护期间的局部端口重配置及资源变更隔离；SC-07以端点链路预算验证高radix，不能将X-series的最多80 GPU或P-series的16 GPU拓扑直接套入144-lane配置。[W11][W12]

## 3. 标准范围与对标依据

PCIe 6.0基线以PCI-SIG正式规范、适用ECN和勘误为准。[W1] 对标PEX90000 PB105所列PCIe 6.3，建立6.0→6.3差异与适用功能清单【STD/TARGET】；规范修订冻结后逐项验收，不将兼容64GT/s等同于已实现6.3全部功能。[W12] CXL、UALink与SUE-T分别保持协议版本、端点和系统边界。[W5][W6][W7]

竞品可确认规格、性能参数、原始来源、测试边界及证据缺口统一列于**附录A**。正文中的COMP表示来源于附录A的对标方向，不表示本IP继承竞品实现或已通过相同测试。

【证据属性】标准能力、芯片产品规格、厂商性能分析、第三方实测与系统部署规模分别记录。没有包型、时间戳和负载条件的latency数值不作为同口径验收阈值。

## 4. 交付版本与功能定义

### 4.1 版本边界

| 版本 | 交付范围 | 进入冻结的条件 |
|---|---|---|
| V1 PCIe通用交换 | Gen6/Gen5、标准层次交换、P2P、分区、NTB候选、RAS/管理 | 标准合规与目标CPU/XPU互操作闭合 |
| V1-A AI增强 | 硬件复制、动态目的选择、INC及CCL插件 | 端点协议、软件API、顺序/重试/数值正确性闭合；不能仅交付一块运算逻辑 |
| V1-C CXL版本 | 初始按CXL 3.1 Switch候选，CXL.io/mem、Type-3池化与FM；旧版本兼容矩阵单列 | 客户Host/内存端点支持、版本/功能矩阵与FM验收冻结 |
| V2多协议扩展 | UALink/SUE-T适配与高radix扩展预研 | 目标规范、端点、PHY和客户平台明确后独立立项 |

AI与CXL增强可能共Die，也可能为派生版本；这一实现选择须由PPA、协议验证成本和客户窗口决定。CXL.cache或一致性代理不在默认池化版本中隐含承诺；若客户要求，另列完整协议与一致性测试。

### 4.2 功能需求矩阵

功能需求按SC-01至SC-07及B-01至B-07拆解；MUST作用于表中指定交付版本，AI/CXL版本能力不隐含纳入通用PCIe版本。

| ID | 需求定义 | 验收重点 | 优先级/来源 |
|---|---|---|---|
| F-01 | 实现PCIe 6.0 64GT/s PAM4、FLIT、FEC/CRC及对应重放；兼容Gen1–5，各端口混代组合按矩阵验证 | 协议VIP、速率/宽度切换、混代互操作；FLIT与non-FLIT边界转换 | V1 MUST；STD W1 |
| F-02 | 支持配置、Memory Read/Write、Completion、Message及地址/BDF/隐式路由的适用行为 | 枚举、BAR、64-bit地址、路由更新、Unsupported Request/错误报文 | V1 MUST；STD |
| F-03 | 支持同层次合法P2P数据路径与可配置ACS | CPU不搬运数据条件下XPU–XPU/XPU–NIC传输；隔离策略生效 | V1 MUST；B-01/02 |
| F-04 | 端口宽度支持x2/x4/x8/x16；x1单列可选；明确分叉边界、各端口独立代际与任意合法端口上行角色 | 分叉/降宽/混代、lane reversal/polarity及角色配置矩阵 | V1 MUST；COMP W12；x1 TARGET |
| F-05 | 144-lane配置支持4个Host接入与4个NTB接口；Host、NTB和虚拟分区数独立定义 | 窗口/doorbell/scratchpad、枚举和域隔离；分区资源另核算 | V1 MUST；COMP W12；分区容量TARGET |
| F-06 | 保持PCIe ordering、TC/VC、P/NP/Cpl信用与前进性，支持适用的Relaxed Ordering/ID-based规则及设备级策略 | 多源混合、信用耗尽、长尾与死锁压力验证；不得全局随意重排 | V1 MUST；STD |
| F-07 | 支持SR-IOV设备透传及必要的ARI、ATS/PASID/PRI报文路由策略 | 主机IOMMU与虚拟化互操作；Switch不充当通用地址翻译代理 | V1 MUST适用行为；STD/B-02 |
| F-08 | AER、DPC、热插拔、复位、链路降级/恢复、ECRC/poison处理及日志 | 注错后影响域受限，无静默数据损坏；不承诺故障期间无暂停 | V1 MUST；COMP W2 |
| F-09 | 支持带内配置与带外管理、可观测性、安全启动/签名升级能力 | CSR/SDK、固件回滚、权限控制、拓扑/错误/性能导出 | V1 MUST；ARCH/COMP W4 |
| F-10 | 广播/多播为显式的组、域、合法地址窗口和数据操作 | 1→N仅给授权目的；屏蔽成员、慢接收者、组更新、重放均正确 | V1-A MUST；B-03/04 |
| F-11 | 定义动态多播路径：预配置组+报文选择，或明确的目的bitmap描述符 | 不能每token等待管理CPU改表；校验非法bitmap、代际号与更新原子性 | V1-A TARGET；W10/SIM |
| F-12 | INC先实现固定功能Reduce/ReduceScatter+复制/分发，CCL组合AllReduce/AllGather | INT32、FP32、BF16→FP32累加候选；顺序、溢出、NaN/Inf与误差合同明确 | V1-A TARGET；BIZ/COMP |
| F-13 | INC必须有上下文、参与bitmap、epoch/sequence、超时、重试去重与结果提交 | 同一分片不会重复累加；失败可中止/清理/回退；不宣称断电exactly-once | V1-A MUST；ARCH |
| F-14 | All-to-Allv支持不均匀路由与拥塞隔离；多播仅加速有重复数据的子流量 | 专家映射、token路由和combine路径均验证 | V1-A MUST；B-04 |
| F-15 | CXL.io/mem协议交换、端口角色、HDM相关路由、FM/CCI与资源隔离 | Type-3 SLD/MLD按版本列功能，绑定/解绑需满足OS安全流程 | V1-C MUST；W6 |
| F-16 | CXL pooling与sharing分开；多级/PBR及动态容量逐项可选 | Host数、LD数、地址范围与迁移流程分别验证；无FM不能称完整池化 | V1-C MUST/STUDY；STD |
| F-17 | 适用的IDE/DOE/SPDM、调试锁定与租户权限需给出能力矩阵 | 必须量化加密和调试模式对latency/功耗的影响 | TARGET；客户安全合同 |

| F-18 | 支持局部端口重配置、热插拔、DPC/eDPC与surprise-removal read tracking | SC-02/03；冻结在途事务处理，验证故障端口外的数据与隔离合同 | V1 MUST；COMP W12 |
| F-19 | 提供FLIT性能计量、注错、包生成、loopback与分层trace | SC-01/02/07；计数单位/时间戳/触发/溢出可读，注错仅在授权测试模式 | V1 MUST；COMP W12；ARCH验收 |
| F-20 | 提供带内、I2C管理与MCTP接口；I3C及OpenBMC集成单列能力 | SC-03；寄存器/消息、权限、超时、升级和管理失联处理 | V1 MUST；I3C/OpenBMC TARGET；COMP W11/W12 |
| F-21 | 管理处理器复位/固件更新与交换数据面隔离；无中断更新为单独目标 | SC-03/07；验证持续流量、配置保留与失败回滚；不扩大为整机无停机保证 | V1 TARGET；COMP W11；ARCH验收 |
| F-22 | 支持PCIe KV Cache appliance的数据通路与系统接口定义 | SC-02/06；冻结存储/内存访问路径、DMA主体、同步与错误反馈；CXL仍独立验收 | V1 TARGET；COMP W11；BIZ |

PCIe广播不是复制任意TLP。禁止复制具有副作用的MMIO、配置写及普通Read/Completion到多个设备。AI扩展应定义专用BAR/descriptor、组内地址映射、授权及缓冲区所有权；通用端点不支持该扩展时走普通单播。

## 5. 物理通道、端口、节点与带宽配置

### 5.1 IP参数与候选芯片配置

| ID | 参数 | 候选定义 | 来源/冻结方法 |
|---|---|---|---|
| IP-01 | lane总数 | IP支持64/104/144配置；144重点评估 | BIZ/COMP W2/W3；PPA决定首发配置 |
| IP-02 | PHY分组 | 144-lane可按9个x16等价bank规划；实际分叉受PLL/时钟/布局约束 | ARCH；PHY供应商接口确认 |
| IP-03 | 逻辑端口数 | 全x16为9口；全x4为36口；若允许全x2为72口 | CALC；72口需要独立MAC/LTSSM/资源支持，不由lane数自动获得 |
| IP-04 | 上下行角色 | 任意合法端口可配置为上行；速率/宽度/角色配置逐端口管理，遵循分区拓扑 | COMP W12；配置切换时的停流/复位边界独立验证 |
| IP-05 | Host/NTB/分区 | 144-lane版本4 Host、4 NTB；4虚拟分区为容量搜索点 | COMP W12；Host/NTB为MUST，分区容量TARGET，不由NTB数推导 |
| IP-06 | 系统节点规模 | V1先覆盖4/8 XPU；扩展模拟16/32/64 XPU | BIZ/SIM；不是单芯片接入承诺 |
| IP-07 | Tags/read tracking | 明确支持的扩展Tag能力和每端口实际outstanding；候选扫64/256/1024 | STD能力与物理资源分离；用BDP和completion行为冻结 |
| IP-08 | 数据接口 | 协议适配器后采用内部cell+metadata；宽度/频率/并行slice可参数化 | ARCH；不能直接套用Ethernet MAC带宽口径 |
| IP-09 | 管理/DFT | CSR、APB/AXI类管理口、JTAG、scan/MBIST、SRAM ECC、计数器接口 | ARCH；交付RTL、约束、模型、寄存器与验证包 |
| IP-10 | PHY责任 | PCS/FEC/LTSSM/SerDes归属、PIPE或专用边界、SRIS/SRNS、时钟复位与延迟分别签接口表 | MUST；数字core与含PHY芯片时延分别验收 |

| IP-11 | MPS/报文资源 | 最大支持1KiB payload【TARGET】；MRRS、Completion分段及buffer独立配置 | COMP W12；不得将MPS当成MRRS或应用消息大小 |
| IP-12 | 诊断与管理模块 | 分离FLIT计量/trace、注错与管理适配；提供MSI-X、watchdog和管理中断接口 | COMP W12；CPU软核/硬核/外置由集成版本选择【ARCH】 |
| IP-13 | 故障与在线维护隔离 | 端口独立复位/配置域，管理CPU与数据面分离，定义CSR持有与回滚状态 | COMP W11/W12；以F-18/21故障注入验证 |
| IP-14 | 大radix扩展 | 保留160/320-lane容量模型搜索；本代仍为64/104/144-lane目标配置 | COMP W11；STUDY，需新PPA/封装/端点预算，不作为当前交付范围 |

PCIe lane为一组TX和一组RX的全双工通道；144 lanes并非144根差分线。封装需容纳144对TX与144对RX，再加电源地、参考时钟和管理引脚；封装/PCB可实现性必须与数字IP同步评估。

### 5.2 统一带宽记账（十进制GB/s）

64GT/s PCIe6每lane单向原始线速为8GB/s；PAM4每symbol两bit不能再把64GT/s乘二。x16为128GB/s单向、256GB/s双向合计。以下都未扣FLIT、TLP、流控、空闲、重放及软件开销。[W1]

| 配置 | 端口数 | 单向每口raw | 全部端口TX容量之和 | TX+RX合计 |
|---|---:|---:|---:|---:|
| 64 lanes | 4×x16等价 | 128GB/s | 512GB/s＝4.096Tb/s | 1024GB/s＝8.192Tb/s |
| 104 lanes | 6×x16+1×x8示例 | x16 128GB/s | 832GB/s＝6.656Tb/s | 1664GB/s＝13.312Tb/s |
| 144 lanes | 9×x16等价 | 128GB/s | 1152GB/s＝9.216Tb/s | 2304GB/s＝18.432Tb/s |

“全部端口TX容量之和”不等于一次切割的单向bisection，也不等于应用payload。144 lanes若切为72:72，两侧各576GB/s，因此该切割每方向raw上限576GB/s、双向合计1152GB/s。若切为64:80，每方向上限受较小侧限制，为512GB/s。内部Fabric吞吐设计应同时验证全部端口并发合法置换、切割容量和不对称端口，不应只引用一个Tb/s数字。

### 5.3 144-lane系统配置示例

| 配置 | lane分配（总计144） | 直连设备示例 | 每方向raw/过订阅解释 |
|---|---|---|---|
| T1 八卡扇出 | 1×x16 Host＋8×x16 XPU | 1 Host＋8 XPU | Host 128GB/s、XPU合计1024GB/s；全XPU对Host为8:1；同层P2P不经Host上行 |
| T2 混合I/O | 2×x16 Host＋4×x16 XPU＋2×x16 NIC＋4×x4 SSD | 2 Host＋4 XPU＋2 NIC＋4 SSD | Host256GB/s、XPU512、NIC256、SSD128；所有下行到Host为3.5:1；双Host须分区/NTB等合法语义 |
| T3 对称级联 | 4×x16下行＋4×x16级联＋1×x16管理Host | 4 XPU＋相邻交换芯片 | XPU侧512GB/s、级联512GB/s为1:1；额外Host流量会争用，必须纳入负载模型 |
| T4 高扇出 | 1×x16 Host＋16×x8下行 | 1 Host＋16设备 | 每下行64GB/s；不是16个x16 XPU；Host聚合仍8:1 |

上行/下行是拓扑角色，PCIe链路本身均全双工。“8:1”仅针对所有下行同时流向Host的特定cut；对P2P、AllReduce、MoE不能照搬该比例。

### 5.4 多节点候选拓扑：32 XPU双层Clos

用于仿真的可核算示例，不作为已验证产品配置：8颗leaf，每颗使用4×x16 XPU＋4×x16 spine uplink＋1×x16 Host；4颗spine，每颗使用8×x16连接8颗leaf，余1×x16可保留。共12颗144-lane芯片、32个XPU，每个XPU单平面128GB/s单向raw。leaf下行/上行各512GB/s，端口预算1:1；跨leaf路径经过leaf–spine–leaf共3颗交换芯片，同leaf为1跳。

该拓扑在带宽上可构造，但普通PCIe透明层次不允许像Ethernet一样任意闭环/多路径转发。必须明确采用专用Fabric模式、NTB/分区、确定性路径、地址域和顺序策略；没有这些能力时只能使用合法PCIe树并接受相应过订阅。因此“多路径”“自适应路由”不能进入V1标准透明交换的默认承诺。

若XPU只有一个x16端口，换成多plane交换芯片不会凭空增加其带宽。多plane需要更多XPU链路资源或在固定总带宽下分拆；须同时给出XPU端口预算与交换芯片BOM。

## 6. 性能需求与测量合同

### 6.1 时间戳边界

| 指标 | 定义 | 包含/不包含 |
|---|---|---|
| L_core | 入口协议适配器向Fabric交付首个合法路由单元，到出口适配器接收对应首单元 | 包含分类/查表/仲裁/内部排队；不包含PHY、入口FLIT收齐/FEC、端点和PCB |
| L_pin | 入口指定FLIT首bit到达RX引脚，到对应输出FLIT首bit离开TX引脚 | 包含PHY/PCS/FEC、FLIT装配、时钟域与Fabric；须写明包型、packing、宽度与gen |
| RTT_pin_loop | 发起端口TX请求首bit，到同端口RX返回Completion末bit | 包含两向链路、2次switch穿越、响应端处理与序列化；不含发起端软件 |
| RTT_XPU | XPU提交指定read/atomic请求，到指定数据/完成状态可被该XPU消费 | 包含端点栈、NoC、内存访问与链路；必须固定同步和计时点 |
| T_collective / T_app | 整次collective或模型step/token的完成时间 | 包含消息数量、算法阶段、依赖、计算通信重叠；不能用L_core替代 |

所有latency必须注明首bit/末bit、空载/负载、典型/最大/P99、是否包含加密/FEC/重试。L_core以“合法路由单元可用”为起点，测量实现须暴露valid/ready事件，避免只测组合查表延迟。

### 6.2 性能需求矩阵

| ID | 指标与目标值 | 来源属性 | 条件与验证 | 状态 |
|---|---|---|---|---|
| P-01 | 64GT/s/lane；x16单向raw 128GB/s | STD W1 | 全lane、目标PVT、合规信道/电气规范 | MUST |
| P-02 | 单跳L_core<100ns | BIZ＋ARCH；W12的90ns cut-through为对标，测点不同，不等同L_core | Gen6 x16、单播、空载/无出口竞争、指定64B read与64/256B write；RTL+时序/CDC核算 | TARGET |
| P-03 | L_pin先扫90/100/125/150ns | ARCH/SIM | 相同包型下含实际PHY/FEC；最终以接口延迟和实测冻结 | TARGET，不预置保证 |
| P-04 | RTT_XPU<250ns | BIZ挑战目标，无一手竞品证明 | 单hop、Gen6 x16、短PCB、无retimer、单outstanding、64B读回、无重试；实际XPU与内存类型固定 | TARGET；预算不闭合不得发布 |
| P-05 | 大块payload吞吐≥95%协议模型上限 | ARCH；不是95% raw线速 | ≥4KiB应用数据、明确MPS/MRRS、充足outstanding、无竞争/重试；固定FLIT packing和TLP mix | TARGET |
| P-06 | 64B payload处理达到该报文组合的合法协议速率 | BIZ/COMP趋势，非照搬Ethernet64B | 分别测posted write、read request及completion、packing、IOPS与GB/s；全端口并发 | TARGET |
| P-07 | 满端口合法置换无额外Fabric容量瓶颈 | ARCH；COMP无阻塞方向 | 满注入、无目的争用、供给充分；与P-05协议模型比较；非任意incast均线速 | MUST功能/TARGET性能 |
| P-08 | 70%可持续负载下P99 L_core≤2×空载基线 | ARCH试验门槛，待BIZ校准 | 预定义均匀非过订阅矩阵；另报热点/突发，不把均匀结果推广 | TARGET/SIM |
| P-09 | 广播输入只注入1份payload；每出口受链路与协议上限约束 | BIZ/ARCH | 扇出2/4/8/16/32、含慢接收者；记录总出口量与源注入量，不写“免费N倍带宽” | V1-A TARGET |
| P-10 | INC在目标collective上有可复现净收益 | BIZ/COMP W4 | 对同拓扑同频率的最佳合理软件/算法基线；扫描收益1.2/1.5/2倍，不预承诺2倍 | V1-A TARGET/SIM |
| P-11 | 功耗/面积/温度无现成冻结数值 | ARCH/PPA | 逐项给PHY、SRAM、core、INC、CPU、封装余量；输出典型/满负载/最坏 | 冻结前必补 |
| P-12 | RAS恢复时延与失效概率单列 | STD+BIZ | 注错统计、恢复时间分布、错误域；用客户SLA冻结，不凭空指定MTBF | 冻结前必补 |

| P-13 | x16→x16 cut-through ≤90ns【TARGET】 | COMP W12；原厂给90ns，未完整公开测点/包型/负载 | 为本设计独立建立首末bit、入口/出口、空载与包型合同；同口径前不得比较，不能代替P-02/04 | TARGET；RTL/PHY/板级闭合 |

“95%协议模型上限”需由独立的报文级参考模型计算，不能以被测实现的自测峰值作为分母。必须报告raw、理论payload上限、实际payload、事务速率与有效利用率五项。

【竞品时延注】PEX90000 PB105第2页明确写出90ns、x16→x16 cut-through，已替代此前“Gen6精确时延未确认”的状态。该值属于系列公开指标，尚不能认定为本文件定义的L_pin、L_core或全PVT最大值。[W12]

### 6.3 RTT<250ns可行性预算

对同一路径两次经过一颗switch的请求/响应事务：

RTT = L_switch_req + L_switch_cpl + L_links_roundtrip + L_endpoint_response + L_endpoint_stack + L_serialization_and_alignment + L_retry_or_queue。

各项必须互斥：若L_switch使用L_pin，则switch内PHY/FEC不再计入别处；若使用L_core，则需额外补全部PHY/PCS/FEC/FLIT边界项。端点stack与response也要避免重计。

以下仅为反推约束的假设例，非测量结果：若两次L_pin各100ns，链路往返及剩余序列化/对齐合计20ns，则250ns只剩30ns给响应端点、内存及其他处理；若L_pin各125ns，两次switch本身已消耗250ns。因此<100ns core不能推出<250ns RTT，甚至100ns pin也不足以保证该RTT。

250ns拆分为两个独立验证项【验证范围】：①片上/板级快速响应器的RTT工程探索；②实际XPU到实际XPU内存的业务RTT。前者通过不代表后者通过；后者的候选探索区间可扫250/500/1000ns，但最终阈值必须来自工作负载与客户要求，不能直接升格为放宽后的合同。

## 7. 底层微架构与容量推导

### 7.1 Fabric、buffer与流控

| ID | 设计需求 | 初始搜索空间 | 冻结依据 |
|---|---|---|---|
| A-01 | cell化交换、端口适配器分离，逐级记录credit与ordering元数据 | 内部cell 64/128/256B | 包效率、切穿收益、SRAM粒度、ECC、PPA |
| A-02 | 内部服务能力不小于协议适配器可持续注入并留控制开销 | raw等价容量的1.0/1.2/1.5倍作搜索点 | 合法packing、小包、广播复制、银行冲突与时钟余量 |
| A-03 | P/NP/Cpl与业务队列独立；至少保证Completion前进性 | VC资源按端口需求参数化；不机械指定“两个VC足够” | PCIe/CXL/SUE语义分别建立依赖图；形式化+压力验证 |
| A-04 | 共享buffer+必要的目的隔离/VOQ候选 | 单port资源等效64/128/256KiB；总SRAM按共享策略扫描 | 信用往返、incast、突发和P99，不以静态总容量代替隔离验证 |
| A-05 | 广播引用计数、可回收copy descriptor与慢成员策略 | 实际端口扇出上限；多播组256/1K/4K候选 | 组生命周期、任务并发、模型并行与epoch；不是竞品组数 |
| A-06 | INC输入流控和缓冲独立核算 | 上下文64/256/1024；分片256B–4KiB搜索 | 并发job/communicator、层流水与超时占用 |
| A-07 | 拥塞可观测与QoS | 队列水位、stall原因、age、公平性、目的/分区占用 | 隔离bulk与latency流；死锁/饥饿/信用泄露验证 |

BDP推导示例：Gen6 x16 raw 128GB/s、假设credit闭环往返250ns，数据在途量约32,000B（31.25KiB）。这只是该假设下的链路数据占用尺度；真实信用按头/数据、流量类别、包型及规范分配。不能据此直接宣布每port 32KiB足够。

若64B读数据希望达到128GB/s的理想payload值，且事务RTT假设250ns，N_outstanding≥128×10^9×250×10^-9/64≈500。此计算故意忽略协议开销，只用于检查量级；需再核对端点请求率、completion打包、Tag能力与read tracking存储。对一个4KiB传输，理想在途请求量约8，但MRRS、Completion分段、MPS与RCB可能改变实际事务数。

内部接口例：一条1024bit@1GHz通道仅128GB/s理论吞吐，刚等于x16单向raw且没有控制/空闲余量。144-lane全入口raw 1152GB/s需要至少9条这种等价并行通道，真实数量或频率应按协议元数据、仲裁气泡和PPA增加。这个数字只说明资源下界，不强制采用9条中央总线。

### 7.2 INC算力不能只写“支持归约”

对G路、每路payload速率R（GB/s）、每元素w字节的归约，理想累加算力至少为(G−1)×R/w Gop/s，输出至少R GB/s；输入总量为G×R，另计重传、精度转换和复制带宽。

以8路、每路目标100GB/s（本文假设值）、FP32为例，归约需要至少175G add/s，输入800GB/s、输出100GB/s。若1GHz、每个运算单元每cycle完成一次add，至少175个等价add/cycle吞吐；流水延迟与状态存储另算。BF16输入转FP32累加会增加元素率和转换资源。不得以FP32理论算力同时承诺BF16吞吐，也不得忽略累加器读写带宽。

数值验收必须区分INT32精确规则与浮点误差。浮点归约顺序改变通常不能保证与任意软件ring逐bit相同，应冻结可复现模式、参考算法、绝对/相对误差、异常值及随机/对抗输入测试。

## 8. 工作负载仿真与规格反推计划

SimAI流量测量与规格提取工作项见**附录B**；本章定义实验矩阵与规格反推接口。参考引用统一见**附录C**。

系统级仿真工具为 **SimAI / ASTRA-sim**【工具范围】。SimAI资料显示由workload、训练框架、collective和网络仿真层构成，可用于结构和负载评估，但不应默认等价于完整PCIe/CXL周期精确模型。[I4][W9]

### 8.1 分层方法与工具能力边界

1. **业务层：**真实trace或AICB生成dense训练、dense推理和MoE通信；SimAI/ASTRA-sim用于并行策略、拓扑、通信阶段和计算通信重叠分析。推理须补充arrival、batch、prefill/decode与KV行为，不能把训练step时间直接当TPOT。
2. **协议/网络层：**构建或扩展PCIe交易模型，加入posted/non-posted/completion、FLIT packing、Tag/credit、ordering、重放和端点处理。CXL另加内存事务、Host路径和必要一致性模型。SUE-T的请求/响应及LLR/流控按目标版本单独建模。
3. **微架构层：**周期模型与RTL验证仲裁、SRAM冲突、buffer、multicast、INC和实际core延迟。
4. **物理层：**PHY供应商模型、SI/PI与布局时序核算pin延迟、信道、FEC/误码、retimer、PVT和功耗。
5. **校准闭环：**协议分析仪/FPGA/评估板测量→模型校准→业务trace复放。系统模拟器不能单独证明100ns core、250ns RTT、PCIe合规或SerDes BER。

SimAI与ASTRA-sim可能共享或派生部分实现，二者结果相近并非独立验证。提交中需固定commit、network backend、配置和修改补丁。

### 8.2 最小实验矩阵

| 实验 | Workload/规模 | 扫描变量 | 输出与决定项 |
|---|---|---|---|
| S-01 | 合成P2P 4/8 XPU，64B/256B/4KiB/1MiB | Gen5/6、x8/x16、1/2/3hop、MPS/MRRS、outstanding | payload曲线、RTT分布；P-02/03/05/06、IP-07 |
| S-02 | Dense训练，TP/DP/PP组合，8/16/32/64 XPU | 144-lane T1/T3/Clos、上行1:1/2:1/4:1/8:1 | step时间、暴露通信占比、MFU；lane与级联配比 |
| S-03 | Dense推理prefill/decode，TP=4/8/16 | 请求长度、batch、并发、250/500/1000ns事务时延 | TTFT、TPOT、tokens/s、P99；RTT和带宽投资优先级 |
| S-04 | MoE EP=8/16/32/64，trace或可复现routing | top-k、token skew、专家映射、动态多播/单播、热点 | dispatch/combine P99、All-to-Allv效率、组表/队列规格 |
| S-05 | AllReduce/RS/AG/Broadcast，1KiB–256MiB | INC on/off、ring/tree/合理硬件基线、扇出、组数、上下文 | collective时间、总字节、INC净收益、数值误差与算力 |
| S-06 | 混合XPU/NIC/SSD I/O | 流量占比0/10/30/50%、incast、慢成员、分区数 | latency隔离、HOL、公平性、buffer峰值；QoS策略 |
| S-07 | CXL冷KV/容量层 | hit/miss、Host中转/直接、读写比、内存池容量 | 容量收益、带宽和TPOT损失；是否应启用CXL版本 |
| S-08 | 故障与退化 | lane降宽、Gen降速、链路错误、成员退出、重试 | 恢复时间、可用吞吐、上下文清理、错误爆炸半径 |

以上规模、时延档位、比例和组数都是实验设计参数，没有模拟结果含义。全因子组合过大时先单因素灵敏度筛选，再对关键交互项做组合实验；保存未运行项而非补成估计结果。

### 8.3 从业务预算反推指标

如果某场景每token暴露通信C次，允许通信占用T_comm，则每次平均暴露预算约T_comm/C；复杂collective含多个阶段，不能把它全部分配给一次switch穿越。例如假设每token有60次不可重叠collective、总通信预算0.6ms，则平均每次10μs；此假设只能用于分析，不能推出RTT必须250ns。需要将collective分解为启动开销、传输量/B_eff、阶段数、拥塞与端点时间，再求switch延迟变化对TPOT的边际收益。

冻结方法：在同一组客户trace上寻找满足TTFT/TPOT/吞吐限制的最小lane/上行/INC/buffer配置，比较100→80ns和x8→x16、单跳→多跳的单位成本收益；输出Pareto前沿与失效条件。业务SLA、成本与功耗上限目前尚未给出，不能虚构最优点。

## 9. 待完善工作与冻结门

| ID | 待办/责任角色 | 必须形成的证据 | 阻断哪些承诺 |
|---|---|---|---|
| T-01 | 产品/客户：锁定至少一个8卡平台与一个扩展平台 | RC/XPU lane预算、Gen、P2P限制、CCL、采购/样片窗口 | 首发64还是144、Host域、级联能力 |
| T-02 | 竞品/系统：获取Broadcom、Astera、众星微数据手册和板卡 | 同条件包型、time boundary、吞吐/功耗、CXL与INC能力矩阵 | 竞品精确对标与宣传措辞 |
| T-03 | 架构/PHY：拆解90ns cut-through、100ns core与250ns RTT预算 | 每级pipeline/CDC/PHY/FEC/FLIT/端点预算及测量点 | P-02/03/04 |
| T-04 | 仿真/算法：完成S-01至S-06并先校准基础P2P | trace、commit、配置、误差、P50/P99与参数敏感性 | buffer、上行配比、INC和多播组数 |
| T-05 | 协议/DV：建立标准合规矩阵 | PCIe规范修订、ECN、每feature测试、VIP/形式化结果 | V1功能冻结 |
| T-06 | CXL/软件：冻结Host与Type-3组合 | CXL版本、FM/CCI、HDM/LD、OS流程、RAS与S-07 | “支持CXL内存池化” |
| T-07 | 架构/软件：定义AI扩展语义 | BAR/descriptor、地址映射、租户、重试去重、浮点误差与CCL插件 | 广播、INC、AllReduce加速 |
| T-08 | 后端/封装：64/104/144三个点的PPA/SI比较 | process、频率、SRAM、SerDes数量、封装引脚、热与测试成本 | 144-lane量产目标与价格/功耗 |
| T-09 | 标准/架构：跟踪UALink/SUE-T/ESUN | 明确规范版本、端点合作和PHY差距；Chiplet接口复用评估 | 多协议或标准兼容宣传 |
| T-10 | 系统/DV：故障与安全验收 | 注错分布、恢复时间、隔离矩阵、签名升级/回滚 | 生产可用性与安全能力 |

验证里程碑【GATE】：G0业务与端口预算→G1参考模型与竞品可比基线→G2微架构/PPA/性能目标冻结→G3RTL功能与性能闭合→G4硅/板级互操作与业务验证。G2前不能把SIM项写成确定spec；G4前不能称已达到实测性能。日期由客户平台节点倒排，本文件不虚构项目排期。

### 9.1 一条需求的冻结记录模板

需求ID｜版本/配置｜MUST/TARGET/STUDY｜来源类型｜原始来源与页码/URL｜业务原因｜数值与单位｜方向/层级｜时间戳边界｜流量/包型/温度/负载｜参考模型｜验证方法｜责任方｜冻结证据/日期｜变更影响。

示例：P-02｜144-lane V1｜TARGET｜BIZ+ARCH｜业务输入U1｜降低小消息穿越开销｜L_core<100ns｜单向单跳｜入口合法路由单元valid至出口accept｜Gen6 x16、64B read、空载、无retry、PVT角待定｜周期参考模型｜RTL性能回归+时序闭合｜架构/DV/后端｜未冻结。

## 10. 产品能力描述

“面向AI服务器板内与机箱内高速互连的PCIe 6.0 Switch IP，支持每lane 64GT/s PAM4与256B FLIT、FEC/CRC，提供可参数化lane/端口配置、P2P与多主机管理。最高144-lane配置及低于100ns的片上交换延迟为候选设计目标。AI增强版本规划硬件广播与集合通信卸载；CXL版本规划内存扩展/池化，具体能力以协议版本、端点适配和系统验证结果为准。”

在完成对应验证前，不使用“RTT<250ns”“原生支持CXL池化”“任意GPU互通”“广播/INC线速”“1024节点直连”作为无条件产品承诺。

## 11. 传统Ethernet IP与PCIe 6.0 Switch IP特性差异

### 11.1 比较范围

【范围】Ethernet侧分为MAC/PCS IP与L2/L3 Switch IP：MAC/PCS负责端口帧与编码，交换IP另含查表、队列、调度和复制。PCIe侧为含协议端口适配的Switch IP；只拿Ethernet MAC与完整PCIe Switch比较会遗漏交换功能。以下“传统Ethernet”指标准全双工有线Ethernet，PAUSE/PFC、LAG等为相应配置下的能力，不默认包含RDMA、SUE-T、LLR、CBFC或INC。

| 特性 | 传统Ethernet MAC / Switch IP | PCIe 6.0 Switch IP | 设计影响/需求映射 |
|---|---|---|---|
| 业务语义 | MAC提供帧服务；L2/L3按MAC/VLAN/IP转发，通用MAC本身不理解内存事务 | Memory/Configuration/Message与Completion等事务；posted/non-posted行为不同 | 不能只替换Ethernet parser；需事务路由和完成报文规则，F-02/06 |
| Flow control | PAUSE暂停链路方向；PFC按优先级暂停；未配置时可因拥塞丢帧 | 基于接收资源通告的credit；按VC及P/NP/Cpl的header/data资源约束发送 | 从阈值回压转换为资源账本，维护credit初始化、消耗、更新与防泄漏，A-03/04 [E1][E4] |
| 流控与拥塞控制 | PAUSE/PFC为逐跳回压；ECN+端点算法可另行调速 | credit解决接收buffer可用性；多源目的竞争仍需调度/限速/队列隔离 | credit不是全网拥塞控制，仍须验证incast、HOL和公平性，F-06、A-07 |
| 链路重传 | 标准全双工MAC不提供通用逐帧ACK/重传；坏FCS按IP接口丢弃或标错，上层负责恢复 | 链路协议原生可靠性机制；Gen6 FLIT结合CRC/FEC与FLIT重试，不能直接套Gen5逐TLP replay实现 | 增加序号、重放状态、发送副本存储和恢复验证，F-01/08 [E3][E4] |
| FEC | 取决于PHY速率/编码/媒介；FEC不等于MAC重传 | 64GT/s使用PAM4、FLIT、轻量FEC与CRC | FEC管纠错、retry处理可恢复检测错误，P-03/04 [W1] |
| 多lane绑定 | PHY可把多条PCS/PMA lanes组成一个Ethernet端口；这是PHY lane分布/对齐 | x4/x8/x16是同一PCIe逻辑Link的宽度，按协议跨lane分配与对齐 | PCIe需width negotiation、deskew和降宽；两者都可能多lane，不能写成Ethernet不支持lane绑定 |
| 多链路聚合 | 802.1AX/LACP把多个MAC links组成LAG，通常按conversation保持有序分配；单流不自动得到总带宽 | 多个独立PCIe links没有通用802.1AX式LACP；多个端口的聚合由上层软件或专用Fabric定义 | PCIe x16不是16个独立x1 links组成的LAG；F-04、IP-04 [E2] |
| 带宽扩展 | 多端口/LAG/ECMP按流或其他有序策略分担；一个高速Ethernet端口也可能由多条物理lanes实现 | 增加Link width、代际速率或多个独立端口；多plane需XPU侧匹配 | 每项同时给Link/端口/lane预算，第5章 |
| Ordering | 以帧/流为主要边界；无通用PCIe read/write/Completion依赖合同 | PCIe ordering与属性、TC/VC及事务类型相关；completion进度不能被普通写流永久阻塞 | Ethernet任意逐包喷洒/重排不能直接移植，F-06 |
| Buffer | Ethernet交换常按端口/优先级/目的管理共享池与headroom，丢包策略可配置 | 接收buffer、信用通告、事务类别和replay buffer分别核算 | retry副本与接收credit buffer不能当成同一容量，A-03/04 |
| Broadcast / Multicast | L2广播、多播组和复制为常见交换能力；MAC单口不等于组播交换机 | 不允许任意复制PCIe Memory/Config/Read事务；AI复制必须定义合法数据窗口和目的映射 | 复用复制引擎，不复用无条件flood语义，F-10/11 |
| 路由与拓扑 | L2/L3转发，LAG/ECMP/路由协议视系统配置 | RC枚举、BDF/地址/隐式路由，标准层次与分区；多路径需独立模式 | FDB/IP route不能等同PCIe路由表，F-02/05、第5.4节 |
| 训练与恢复 | 链路建立、PHY自协商/训练因标准与媒介而异 | LTSSM、速率/宽度协商、均衡、复位/恢复与热插拔 | PCIe端口状态机与RAS是独立工作包，F-01/08 |
| 安全/隔离 | VLAN/ACL/MACsec等由选定IP和系统提供 | ACS、分区、地址窗口及适用IDE/DOE/SPDM能力矩阵 | 隔离对象由帧/网络域转为Host与事务域，F-05/07/17 |
| 软件/管理 | MDIO、统计与交换SDK；RDMA/transport属于额外协议栈 | 配置空间、枚举、驱动、AER、NTB/Fabric SDK及设备兼容 | 交付含配置模型、驱动接口、故障日志与互操作矩阵，F-09 |

【流控细化】PFC在达到暂停条件后发出回压，需要为在途数据预留headroom；PCIe的发送许可来自credit，对应已通告接收资源。两者都受反馈往返时间、粒度、仲裁和错误恢复影响，不能推导为“PCIe天然没有拥塞”或“PFC本身提供可靠重传”。[E1][E4]

【可靠性边界】PCIe链路重试确认的是相邻链路交付，不等于业务read完成、INC结果持久化或跨故障exactly-once。posted write的业务完成需上层显式定义；Gen6 FLIT确认与重试粒度、状态机及定时参数以正式PCIe规范为准。[W1][E4]

### 11.2 从Ethernet交换内核复用到PCIe的模块边界

| 分类 | 模块 | PCIe新增或调整内容 |
|---|---|---|
| 可复用结构【ARCH】 | crossbar/cell fabric、SRAM/ECC、队列、仲裁、复制、计数器、DFT | 重新核算PCIe事务类别、吞吐、时钟边界、metadata和顺序合同 |
| 协议专用【MUST】 | TLP/FLIT适配、PCIe路由、配置空间、LTSSM接口、P/NP/Cpl credit、retry | 不通过简单替换Ethernet头部实现；明确PHY/PCS/Controller与Switch core归属 |
| 系统专用【MUST】 | RC枚举、ACS、NTB/分区、复位/DPC、Host隔离、软件兼容 | 标准PCIe拓扑验证，设备/驱动联测，F-02/05/07/08/09 |
| AI扩展【TARGET】 | 多播descriptor、INC上下文、数据格式、CCL插件 | 对应SC-04/05；重放去重、慢成员和数值正确性独立验收 |

### 11.3 SUE-T与增强Ethernet的适用位置

SUE-T/ESUN是Ethernet Scale-Up的演进方向，LLR、CBFC与INC可出现在特定增强芯片/协议中。[W7][W8] 这些能力应列为“增强Ethernet配置”，不能追溯成所有传统Ethernet IP的默认功能。SUE-T端点transport负责的内存语义、请求响应和恢复，与Ethernet Switch的转发/回压边界分别建模；跨PCIe/CXL/UALink的转换另需地址、顺序、权限与错误语义定义。

## 附录A：可信竞品规格与性能参数

### A.1 已核实规格与性能指标总表

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

核对日期：2026-10-28。GB/s均为十进制；未确认字段保留“未确认”，不补估计值。国内竞品待核实项见A.4。

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

## 附录B：SimAI流量测量与规格提取工作项

| ID | 工作项 | 交付物 | 对应需求 / 实验 |
|---|---|---|---|
| B-W01 | 固定模型、并行策略、数据精度、batch/sequence、rank映射及工具版本 | 可复现的workload、拓扑与配置基线 | B-01至B-07；S-01至S-08 |
| B-W02 | 使用AICB统计训练/推理通信操作；有实机时采集操作耗时与计算通信重叠 | collective类型/次数、消息大小分布、通信组及时间线 | SC-04/05；S-02/03/04 [M1] |
| B-W03 | 使用SimCCL将collective展开为逐流通信，核对算法、协议、channel/chunk与依赖 | src/dst通信矩阵、各rank发送量、跨节点流量与热点分布 | F-10/11/14；A-05/06 [M2][M3] |
| B-W04 | 在网络backend测量链路利用率、流完成时间、排队及突发；缺失计数先补采集 | 每link字节/带宽、P50/P99、队列水位与stall分布 | P-05/06/08；A-04/07；S-06 [M4] |
| B-W05 | 将逐流通信映射为PCIe事务，补充TLP/FLIT、P/NP/Cpl、Tag/credit、ordering及retry模型 | PCIe协议流量模型及与参考端点的校准记录 | F-01/02/06；IP-07；P-02至P-07 |
| B-W06 | 比较不同lane数、端口宽度、上下行配比、跳数和buffer配置 | 性能/容量敏感性及满足业务约束的配置集合 | IP-01至IP-08；A-01至A-04；S-01/02/03 |
| B-W07 | 对比单播、多播及INC，覆盖MoE不均匀流量、慢成员与并发上下文 | 源注入/出口流量、组表/上下文占用、collective净收益与正确性记录 | F-10至F-14；P-09/10；S-04/05 |
| B-W08 | 使用真实集群、端口计数器及PCIe分析仪校准，统一计时与字节口径 | 模型/实测误差、适用范围及可冻结参数清单 | T-03/04；G1至G4 [M5] |

【计量边界】操作消息量、逐流payload、物理link字节及algbw/busbw分别统计；TX与RX分列，多hop和复制流量按实际路径累计。[M6]

【能力边界】SimCCL导出不等于真实发流；SimAI-Physical的RoCEv2测量不等于PCIe6协议测量。<100ns core、<250ns RTT及协议合规须取得对应RTL、PHY和板级证据。[M3][M4]

## 附录C：参考引用

内部资料用于继承项目方向，不作为第三方竞品规格的独立证明。本文件中的新容量、队列、时延预算及实验档位均在正文标为ARCH/CALC/SIM。

### C.1 内部与业务来源

- [I1] sharpfabric/锐联半导体_商业计划书.md，v18.0，文件内数据截止2026-08-10；§3.3产品树、§3.5协议边界、§4.1规格、P3/P6/P9口径原则。[原文件](https://chatgpt.com/library?file_id=libfile_ecf8bfc4c7f081919ab00eb46741c187&account_id=personal)
- [I2] sharpfabric/AI_Interconnect_Report_v1.0_review_revised.md；CXL/PCIe边界、Tomahawk Ultra时延口径、§6.6节点规模与业务边界；历史指标需另核验。[原文件](https://chatgpt.com/library?file_id=libfile_95956507fc64819187f7e085d7370327&account_id=personal)
- [I3] sharpfabric/OrbfabriX-v0.34-roadmap-architecture-v2.pptx，第6–7页；E/O/M边界与E-series分阶段路线。[原文件](https://chatgpt.com/library?file_id=libfile_652ba2bac36081919a795277ded3e352&account_id=personal)
- [I4] sharpfabric/SimAI_Intro_Online.pdf，2024-11-26，第5–9页；仿真工具分层与workload输入。[原文件](https://chatgpt.com/library?file_id=libfile_33ddfd868a848191a63e28d9963c2ec6&account_id=personal)
- [I5] sharpfabric/UALink-Chiplet-Specification_v1.01_Final_evaluationcopy-1.pdf；作为Chiplet边界后续核查材料，不把Chiplet接口规范代替UALink网络协议规范。[原文件](https://chatgpt.com/library?file_id=libfile_df547090c0448191bfdda7883d1b58c9&account_id=personal)
- [U1] 业务输入U1：144 lanes、64GT/s、PAM4/FLIT、CXL池化、INC/广播、RTT<250ns、内部交换<100ns?及竞品方向。问号指标保留为待验证目标。
- [U2] 历史讨论检索：板内到柜内分层；区分switch latency与XPU RTT；SimAI/ASTRA-sim需补协议模型。历史助手数值不作为一手规格证据。

### C.2 外部一手来源（核对日期2026-10-28）

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

### C.3 待补充的一手证据

众星微C82-4G正式产品规格与测量报告；Broadcom Gen6 90ns的完整测量边界与各型号功耗资料；Astera对应配置的精确时延/端口组合/INC吞吐；客户XPU读回路径与端点实测；所选PCIe/CXL/UALink/SUE-T正式规范条款矩阵。上述缺口不以新闻报道或历史助手回答补成已确认spec。

### C.4 原报告尾部技术资料

【引用属性】原报告尾注编号保留用于追溯；产品说明只证明所述厂商能力，论文只支持研究方法和已限定实验，均不直接成为本IP数值承诺。优先链接原厂、作者或出版机构；二手市场估值、股价、未经核实专利统计和无关边缘AI资料不纳入需求依据。

| 新编号 | 原尾注 | 文献与访问入口 | 纳入用途/约束 |
|---|---|---|---|
| R01 | 63 | Astera Labs, Scorpio Smart Fabric Switch Portfolio Brief, April 2025，[PDF](https://www.asteralabs.com/wp-content/uploads/2025/05/PB_Scorpio_Smart_Fabric_Switches.pdf)，第2页 | 混合I/O与P2P/RAS/telemetry；历史产品状态不代表2026当前状态 |
| R02 | 77 | Rashidi等，FRED，ISCA 2025；[作者预印本2406.19580](https://arxiv.org/abs/2406.19580)，v2 2025-06-09 | 归约/分发与并行策略协同；wafer-scale实验不能直接套用到PCIe板级 |
| R03 | 78 | Zhao等，Insights into DeepSeek-V3: Scaling Challenges and Reflections on Hardware for AI Architectures，ISCA 2025；[作者版本](https://arxiv.org/abs/2505.09343) | MoE、计算通信协同与多plane工作负载依据；不迁入GPU集群吞吐倍数 |
| R04 | 79 | Jo Sanghoon、Hyojun Son、John Kim，Logical/Physical Topology-Aware Collective Communication in Deep Learning Training，HPCA 2023，56–68；[IEEE](https://ieeexplore.ieee.org/document/10071117/) | collective算法与物理拓扑匹配；元数据已核对，具体实验需查全文 |
| R05 | 81 | Park等，An LPDDR-based CXL-PNM Platform for TCO-efficient Inference of Transformer-based Large Language Models，HPCA 2024；[作者机构记录](https://experts.illinois.edu/en/publications/an-lpddr-based-cxl-pnm-platform-for-tco-efficient-inference-of-tr/) | CXL容量/近存计算与推理场景区分；PNM性能不等于Switch性能 |
| R06 | 82 | Liu等，Enabling Efficient Large Recommendation Model Training with near CXL Memory Processing，ISCA 2024，382–395；[DOI](https://doi.org/10.1109/ISCA59077.2024.00036) | 【原报告书目，全文待核验】近CXL处理的独立方向；原报告9.4×未迁入性能目标 |
| R07 | 33 | 摩尔线程，[平湖架构官方页面](https://www.mthreads.com/architecture/pinghu) | 国产端点侧通信卸载与软件栈协同；私有互联不等于PCIe兼容 |
| R08 | 37 | 壁仞科技，2026-08-03，[WAIC 2026 BLink2.0及超节点方案](https://www.birentech.com/news/odug5ugc29npl8m6slum8d9k/) | 内存语义、INC、拥塞控制与链路修复的系统能力拆分；系统规模不当作芯片radix |
| R09 | 87 | imec，Interfacing silicon photonics for high-density co-packaged optics，原载Chip Scale Review Nov/Dec 2024；[原机构说明](https://www.imec-int.com/en/articles/interfacing-silicon-photonics-high-density-co-packaged-optics) | 后续光I/O封装接口约束；不作为本代PCIe交换时延指标 |
| R10 | 9 | PCI-SIG规范概览及FAQ，见W1 | PCIe代际、FLIT/FEC与吞吐口径，替代第三方参数汇总 |
| R11 | 7、15、16 | UALink Consortium、OCP、Broadcom官方资料，见W5/W7/W8 | 对应协议/交换芯片分别引用，避免SUE、Tomahawk Ultra、Tomahawk6混写 |

### C.5 流量测量、协议比较与补充产品来源

- [M1] Alibaba Cloud，[AICB官方仓库](https://github.com/aliyun/aicb)，README的Workload/Physical Run/Result Visualization；消息类型、规模与真实运行日志。
- [M2] Alibaba Cloud，[SimCCL官方仓库](https://github.com/aliyun/SimCCL)，Quick Start与Supported Operations；standalone逐流导出。
- [M3] Alibaba Cloud，[SimCCL Architecture](https://github.com/aliyun/SimCCL/blob/master/docs/design/architecture.md)，版本、算法/协议选择、FlowModel与NS-3接口。
- [M4] Alibaba Cloud，[SimAI Tutorial](https://github.com/aliyun/SimAI/blob/master/docs/Tutorial.md)，Analytical / NS-3 / Physical；RoCEv2物理发流不等于PCIe协议测试。
- [M5] Wang等，SimAI: Unifying Architecture Design and Performance Tuning for Large-Scale Large Language Model Training with Scalability and Precision，NSDI 2025，541–558；[论文入口](https://www.usenix.org/conference/nsdi25/presentation/wang-xizheng-simai)。论文对齐精度仅适用于其评测，未移植为本项目精度保证。
- [M6] NVIDIA，[nccl-tests PERFORMANCE](https://github.com/NVIDIA/nccl-tests/blob/master/doc/PERFORMANCE.md)，algbw与busbw定义。
- [E1] IEEE 802.1，[802.1Qbb Priority-based Flow Control](https://1.ieee802.org/dcb/802-1qbb/)，PAUSE与按优先级回压边界。
- [E2] IEEE SA，[802.1AX-2020 Link Aggregation](https://standards.ieee.org/ieee/802.1AX/6768/)，多MAC links聚合；不等同PHY多lane。
- [E3] AMD，[PG051 FCS](https://docs.amd.com/r/en-US/pg051-tri-mode-eth-mac/FCS)及[Receiver Interface](https://docs.amd.com/r/en-US/pg051-tri-mode-eth-mac/Receiver-Interface)，MAC坏帧指示示例。
- [E4] PCI-SIG，[PCIe第六代演进与FLIT重试](https://pcisig.com/blog/evolution-pci-express-specification-its-sixth-generation-third-decade-and-still-going-strong)；旧代Data Link结构参见[Altera P-Tile Data Link Layer Overview](https://docs.altera.com/r/docs/683059/25.3/p-tile-avalon-streaming-ip-for-pci-express-user-guide/data-link-layer-overview?contentId=J5UuDi292_eViu_XAD~UDw)。旧代NAK/DLLP示例不替代Gen6 FLIT协议条款。
- [C1] Broadcom，[PCI Express Switches Selection Guide, BC00-0445EN](https://docs.broadcom.com/doc/BC00-0445EN)，第1页Gen5参数；原表可确认115ns等数值，测试边界未完整给出。
