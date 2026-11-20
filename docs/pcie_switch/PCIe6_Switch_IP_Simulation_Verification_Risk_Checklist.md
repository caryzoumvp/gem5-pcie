# PCIe 6.0 Switch IP 仿真与验证风险清单

**版本** v1.0　 **日期** 2026-11-18　 **状态** 验证计划，所有条目初始为待验证

## 1. 验证范围与判据

首版基线为单颗透明 Switch，**1×x16 USP 接 Host，8×x16 DSP 接 GPU/加速器**，验证同一 Root 下的 8 卡 P2P、CPU 到设备和混代设备。端口的 x8 等配置以对外声明为准。规范要求沿用项目的 SW-01～SW-27 编号，最终以冻结的 Base Spec 修订版、适用 ECN 和产品能力表为准。[项目规范要求与原文索引][spec-index]

| 项目 | 验证口径 |
|---|---|
| 带宽 | Gen6 x16 单方向原始带宽 **128 GB/s**，随后依次扣除 FLIT、TLP、空闲、流控与 Replay 等开销。单独报告应用有效 GB/s、每端口方向和负载类型。单条 x16 USP 的总对 Host 带宽仍受该链路限制。 |
| 时延 | 保留项目目标 **单颗 Switch 交换 <100 ns、端到端指定读事务 RTT <250 ns**。建议分别以芯片外部入/出链路相同参考事件、Requester 发读至收到最后一个匹配 Completion 为口径。评审前冻结参考点、负载、读大小、速率/宽度、是否包含 PHY 或 Retimer 及目标对应的统计量。 |
| P99/P999 | 每种负载分别报告直方图、样本数、预热、种子、观察窗与队列水位。对带宽、P99/P999、恢复时间、持续压测时长和容许 Replay 水平尚未确认的数值标为 **TBD**，由需求责任人评审冻结。 |
| 扩展能力 | 多 Host、NTB、16 卡 Ring、32 卡 Spine、多播和 INC 分别按已批准的能力范围启动验证。多芯片标准透明级联与厂商 Fabric 的地址域、枚举和路由模型分别建模。 |

**P0** 表示会阻塞对应阶段放行的风险。**P1** 表示需要在目标配置验收或对应功能交付前关闭。协议判据采用规范与对外声明的能力，性能与恢复阈值采用已冻结的产品需求。

## 2. 风险矩阵

### A. PHY、封装与集成

| ID / 优先级 | 风险与影响 | 验证场景与方法 | 通过判据 / 证据 / 建议责任方 |
|---|---|---|---|
| A01 / P0 | PHY 或 Controller 的 silicon-proven 证据与选型配置不匹配，集成后需要改版 | 对照工艺及 PDK、IP revision、PIPE/原生接口、x16 lane 组合、跨宏组时钟方式、USP/DSP 角色、封装通道和 PVT 范围 | 有可追溯硅测报告、互操作记录和已知勘误。以逐项矩阵明确参考配置与产品的差距、责任人和补测计划。**IP 集成 + PHY** |
| A02 / P0 | Gen6 PAM4 最差 lane 的电气裕量不足，升速失败或频繁 Replay | 签核阶段用目标封装和板级通道模型扫损耗、反射、串扰、电源/时钟噪声和 PVT。硅后测 Tx、Rx stressed input、Link EQ，覆盖长短通道及有/无 Retimer 的批准配置 | 按适用的 PCI-SIG 测试规范通过，并满足产品规定的最差 lane 裕量。保存逐 lane SI 仿真与硅测结果。**PHY + SI/PI** |
| A03 / P0 | 单端口能跑通，全端口并发时出现时钟、供电或热耦合问题 | 全端口双向同时发流量，比较单口与多口下的 BER/FEC/Replay、链路速率宽度、功耗、结温和最差 lane。扫最差电压、温度及批准的板卡设计 | 满足目标并发配置的协议、吞吐及稳定性要求。保存功耗/温度、各端口与 lane 统计及实际物理链路配置。**PHY + SI/PI + 平台** |
| A04 / P0 | Controller/Fabric 跨时钟、跨复位或 SRAM 时序错误 | CDC/RDC、STA、异步 FIFO 形式属性，仿真随机时钟比、独立停止/复位和拥塞中的恢复。覆盖数据、Credit、状态及管理寄存器跨域 | 无未处置的重大 CDC/RDC、时序问题。FIFO 与握手无重读、漏读或复位残留。交付报告、豁免依据和断言结果。**RTL + 后端 + DV** |

### B. PCIe 协议与交换正确性

| ID / 优先级 | 风险与影响 | 验证场景与方法 | 通过判据 / 证据 / 建议责任方 |
|---|---|---|---|
| B01 / P0 | Type 1 Bridge、Bus/地址窗或 BDF 解码错误，导致枚举失败或 misroute | RC + 8 EP 的冷枚举和重枚举。覆盖总线/地址窗边界、Type 1→0、64-bit BAR、桥自身与下游资源、空端口和无效目标。检查 BIOS/OS 资源分配 | 寄存器模型、配置 Completion 和实际拓扑一致。保留 VIP trace、枚举日志及配置空间快照。**协议 RTL + DV + 平台**。SW-01～04 |
| B02 / P0 | Request 与 Completion 选择不同的错误路径，或 TLP 在交换中被误拆分 | 所有合法入口→出口路径覆盖。混合 MWr/MRd、Msg/MsgD、中断消息、分段 Completion、多 Requester 与重叠 Tag。变化 MPS、MRRS、RCB 和地址边界，区分规范允许的 Completer 分段与 Switch 转发 | Request/Completion 路由、状态、payload、Byte Count 等与逐事务参考模型一致。按规范处理异常，保留端到端 scoreboard 和入口×出口×类型覆盖。**Fabric RTL + DV**。SW-04～08 |
| B03 / P0 | 混接 Gen6/旧代设备时 FM/NFM 翻译错误或丢失字段 | 在同一 Switch 的相邻端口组合测试 FM↔FM、FM↔NFM、NFM↔NFM。覆盖多个 TLP 共用 FLIT、跨 FLIT 的 TLP、OHC/Prefix、转换被阻塞的情况。速率与模式分别记录 | 合法转换保留事务语义。不可转换的 TLP 按规范报告 TLP Translation Egress Blocked。保留入口/出口两侧 trace 和字段比对。**端口 RTL + DV**。SW-14～16 |
| B04 / P0 | FEC/CRC/Replay 边界问题引发漏包、重复提交或 Replay Buffer 失效 | 注入可纠错和不可纠错的 FLIT 错误、连续错误、ACK/NAK 与序号回绕，叠加 Replay Buffer 接近满、流量和恢复。NFM 另测其序号、LCRC 和重传 | 可恢复事务正确提交一次。不可恢复路径产生规定的状态和处置。端口独立处理链路 Replay。保存 replay buffer 水位、序号 trace 和端到端数据校验。**链路 RTL + DV**。SW-14、16 |
| B05 / P0 | P/NP/Cpl 的 Credit 或 ordering 错误，出现溢出、饥饿或死锁 | 分开扫描 Header/Data、P/NP/Cpl、TC/VC、Credit 初始化/消耗/回收和回绕，覆盖适用的 Shared/Scaled FC。构造出口 Credit 耗尽、双向读写、Completion 回流和 RO/IDO 等并发条件 | 无超信用发送、Credit 泄漏或违规重排。形式验证 Credit 守恒及关键顺序属性。在对端最终释放 Credit 等假设下检查前进性，并保留反压恢复波形。**Fabric RTL + DV**。SW-09～13 |
| B06 / P0 | 链路降级或复位传播错误，遗留旧事务或误伤其他端口 | 覆盖 LTSSM 升速、降宽、重训、Hot Reset、Secondary Bus Reset 及 USP/DSP DL_Down。分别在 P/NP/Cpl 在途、Replay 活跃和窗口重配边界触发。按承诺覆盖 L0p、PM 与 Hot-Plug | 确认规定的复位传播范围，清理或处置在途事务和 Credit，重训后无旧 Completion 串入。保留 LTSSM 时间线、事务快照和软件恢复日志。**端口 RTL + DV + 固件**。SW-17、19、21 |
| B07 / P0 | 错误隔离或 ACS 策略错误，导致静默损坏、错误扩散或 P2P 绕行 | 注入 Malformed、Poison、UR/CA、Unexpected Completion。验证适用的 AER/DPC 上报与恢复。验证 ACS 各适用控制组合、正常直达、受控重定向和合法隔离边界 | 错误状态、来源以及恢复范围符合声明。直达、重定向或阻断路径与配置一致，并用端口计数和 trace 证明。**协议 RTL + DV + 平台**。SW-18、20、22～24 |
| B08 / P1¹ | 选配能力的配置空间声明与实际行为不符 | 按对外声明的能力验证 ARI、AtomicOp、DMWr、PTM、FPB、IDE 等适用正反例，检查能力组合和全端口约束。传统 Locked Request 和其他基础义务纳入 P0 回归 | 能力位、寄存器和事务行为一致。逐条保存能力匹配矩阵及测试结果。**协议 RTL + DV**。SW-07、24、26～27 |

¹ 功能一旦对外交付，相关规范义务进入该配置的放行判据。

### C. Scale-up 性能与缓冲

| ID / 优先级 | 风险与影响 | 验证场景与方法 | 通过判据 / 证据 / 建议责任方 |
|---|---|---|---|
| C01 / P0 | 模型过于理想，低估端口、缓冲或尾延迟需求 | 用 SimAI/Astra-sim 输入实际 TP/EP/PP 等配置和 collective trace。以模型版本、并行组、Batch、Seq、active token、每层/每步及预填充/解码明确统计口径，回核 AllReduce、AllGather、ReduceScatter、All-to-All、PP、KV/权重流量及方向 | 手算、trace 注入字节和模型输出守恒。每种并行配置有可复现输入及各端口压力。用 RTL 或实测吞吐、Switch 延迟和 Credit/队列参数校准。**系统仿真 + CCL + 性能 DV** |
| C02 / P0 | 用原始线速代替有效吞吐，或读取在途深度不足 | 单流 MWr/MRd 与全双工流，扫 64 B 至大块的实际读取尺寸、Tag、MPS/MRRS、RCB 及请求端发射能力，统计 payload GB/s、FLIT 利用率与所有层的 Credit/缓冲瓶颈 | 成功闭合目标吞吐及对应测量条件。用带宽时延积作 sanity check，在 250 ns RTT、128 GB/s 理想目标下需约 32 KB 有效数据在途，64 B 请求约 500 笔，4 KiB 约 8 笔。实际阈值以可用 Tag、Completion 分段和有效带宽为准。**架构 + 性能 DV** |
| C03 / P0 | 8 卡突发导致 Incast、HOL、SRAM 端口冲突或尾延迟超标 | 使用全速置换、均匀 all-to-all、多个入口争抢一个出口、EP 热点/倾斜、TP 与 EP/KV 混合等负载。在拥塞口叠加 Credit 暂停或 Replay，对比 Crossbar/VOQ 和 FIFO 配置 | 在冻结负载下满足吞吐、P99/P999 和水位预算。检查受阻流与无关流各自的尾延迟及公平性，输出每端口/VC/队列的时序数据。**Fabric 架构 + 性能 DV** |
| C04 / P0 | 单跳延迟和系统 RTT 的测试边界混用，无法定位超时来源 | 空载及规定背景负载分别测 ingress→egress 和端到端 MRd RTT。分别记录 PHY/端口、交换队列、串行化、传播和目标设备响应，在 1 跳和适用的级联配置中交叉核对 | 按冻结口径分别核对 <100 ns 与 <250 ns。提交明确的测量参考点、逐段分解、直方图及满载结果。**性能 DV + 实验室** |
| C05 / P1 | 单 Host 上行争抢或 ACS/IOMMU 配置把 P2P 变成绕 Host 流量 | 并发运行 8 卡 P2P、Host↔GPU 搬运与背景控制流。对批准的 ACS/IOMMU/驱动配置分别量测各 DSP 和 USP 的方向计数 | 直达场景的 GPU payload 走目标 DSP。配置要求重定向时，路径和数据量符合预期。量化 USP 汇聚对带宽和尾延的影响。**平台 + 性能 DV** |

### D. 扩展与整机互操作

| ID / 优先级 | 风险与影响 | 验证场景与方法 | 通过判据 / 证据 / 建议责任方 |
|---|---|---|---|
| D01 / P0 | 整机团队用单板、单设备的合规结果替代目标 BOM 互操作 | 按 CPU/RC、GPU/NIC/SSD、Retimer/板卡、BIOS/OS/驱动/固件建立组合矩阵。覆盖 Gen6 与混代、冷启动/热复位、满载长稳及有/无 Retimer 的批准路径 | 所有首发必需组合通过功能、稳定性和恢复要求。保留协议 trace、Linux/BIOS 日志与配置，合规及互操作结果分别归档。**平台 + 实验室 + 固件** |
| D02 / P0（启用时） | 多级拓扑的端口、Root 与地址域假设错误，或发生跨跳拥塞死锁 | 为 16 卡 Ring、32 卡 Spine 各自先冻结每条链路的端口角色、lane 配置和转发机制。标准透明级联检查逻辑树。对启用的 NTB/厂商 Fabric 另测路由、环路控制、资源依赖和掉链恢复 | 按设计完成枚举，路径无环回，故障后可恢复。用逐跳事务 trace、依赖图、端口利用率及队列水位验收。**Fabric 架构 + 系统 DV** |
| D03 / P0（启用时） | 多 Host 分区、NTB 地址窗或 VF 归属错误，接管时重复访问 | 用两套独立 RC 测分区、地址窗口翻译、Doorbell 和边界访问。只在范围包含设备共享时验证 VF 分配/撤销。叠加备用路径接管、在途 I/O、重连和独立复位 | 分区与窗口范围正确，跨域通信与设备归属符合设计。接管按项目策略处理在途 I/O，无双主和静默损坏。**固件 + 平台 + DV** |
| D04 / P1（启用时） | 权重多播被慢端口拖住，组更新造成错误复制 | 源端一次提交，Switch 向目标 DSP 扇出。覆盖组更新、出口阻塞、不同速率、重传和故障。声明标准 PCIe Multicast 时，覆盖对应的 Window/Group/Receive/Block 与适用限制 | 每个合法目标收到预期副本，未加入组的端口无副本。报告源端与各出口实际字节量、尾延和缓冲生命周期。**Fabric RTL + CCL + DV**。SW-25 |
| D05 / P1（启用时） | INC 归约结果、完成同步或恢复不正确 | 用逐元素参考模型覆盖数据类型、累加精度、分片、乱序和 Replay 后交付、参与者缺失和任务取消。以代表性 TP 或其他 collective 对比端侧算法 | 数值满足冻结的精度判据。每个 shard 只计入每个参与者有效贡献一次，完成和超时可追踪。输出同负载的延迟、带宽及结果比对。**INC RTL + CCL + DV** |

## 3. 执行顺序与放行证据

| 阶段 | 优先输入与放行证据 |
|---|---|
| **架构评审** | 冻结端口能力、拓扑/地址域、性能测量定义和首发设备矩阵。先做 A01、C01 和 C02，确定 PHY 证据差距、首批流量和基准参数。 |
| **RTL / VIP / Formal** | 基础协议和主要复合故障场景通过，尤其是 **混代转换 + Replay + 反压**、**复位 + 在途 NP/Cpl**、**ACS 切换或受控重配 + P2P**。在覆盖数据库中按端口、方向、速率/宽度、模式和 TLP 类型追踪。 |
| **流片前签核** | 归档 PHY/封装/通道仿真、STA、CDC/RDC、覆盖结果、已批准豁免与未关项。对性能指标同时留存系统模型和 RTL 验证结果。 |
| **硅后与首发验收** | 分别完成 Gen6 Tx/Rx/Link EQ、协议/合规、目标机型互操作和多端口长稳。FPGA 或硬件仿真可提前验证枚举、固件和数字数据面，Gen6 电气裕量以目标 ASIC 硅片与目标通道实测为准。 |

风险条目在评审台账中至少记录 **ID、实测配置及版本、Owner、状态、预期值/实测值、证据链接、缺陷或豁免编号和关闭日期**。建议状态为待验证、验证中、通过、失败、已批准豁免。性能仿真报告、RTL 验证结果与产品硅测结果分别作为不同阶段的证据。

## 4. 参考资料

- [PCIe 6.0 Switch 规范要求与原文索引][spec-index]，用于 SW-01～27 的适用性和条款追踪。
- [PCIe Switch 技术研究报告][risk-paper]，第 3 节 IP 化风险，用于 PHY 和协议栈 silicon-proven、Gen6 PAM4 签核和互操作风险。
- [AI Fabric PCIe 6.0 Switch 架构评审稿][architecture]，第 1.7 节的 9×x16 部署与前序架构分析。
- [PCI-SIG PCIe 6.0 Specification](https://pcisig.com/pci-express-6.0-specification)；[FLIT/FEC/CRC/Replay Q&A](https://pcisig.com/blog/pcie%C2%AE-60-specification-webinar-qa-deeper-dive-flit-mode-pam4-and-forward-error-correction-fec)。
- [PCI-SIG 6.x Workshop 的测试分项](https://pcisig.com/events/pcie-6x-preliminary-fyi-workshop)；[PCI-SIG Integrators List](https://pcisig.com/developers/integrators-list)。
- [SimAI](https://github.com/aliyun/simai)；[ASTRA-sim](https://github.com/astra-sim/astra-sim)。

[spec-index]: https://chatgpt.com/library?file_id=libfile_e2cdc1ece8948191af536258af468dba&account_id=personal
[risk-paper]: https://chatgpt.com/library?file_id=libfile_32ce717460ac8191b5ad8efc014ed102&account_id=personal
[architecture]: https://chatgpt.com/library?file_id=libfile_8a972a829f3881918ff1fa42eac3fc88&account_id=personal
