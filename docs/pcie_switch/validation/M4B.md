# M4B 原生 SimAI 闭环验收

gem5 以 M4A 的配置构建透明 PCIe 拓扑，在同一进程载入
`libSimAI_pcie.so`。SimAI 的 `Sys`、`Workload` 和 SimCCL v2.30
驱动原生训练 workload。显式任务输入使用相同的 PCIe 匹配层，描述
Read pull、Tree、Host staging 和不均匀的 MoE token 路由。
gem5 负责全部 Tick、DMA、TLP、FLIT、Link、Switch 与 Endpoint。

## 复现

先构建 `scons build/NULL/gem5.fast -j 8`，然后从 gem5 根目录执行：

```sh
python3 util/pcie_lab/validate_simai.py --simai-root /workspaces/SimAI \
    --output-dir output/pcie-simai
```

脚本构建 PCIe 客户端和原生回调夹具，运行 SimAI、独立 replay、错误
和 HTML 回导用例。可用 `--library <libSimAI_pcie.so>` 复用已构建库。
完整 V-34 同时传入 `--analytical-bin` 和 `--ns3-bin`，两者均使用当前
SimAI 与 SimCCL 源码构建。Analytical 使用
`astra-sim-alibabacloud/build/simai_analytical` 的 CMake 入口及
`-DUSE_ANALYTICAL=ON`。NS3 使用现有构建流程或其独立副本，采用
`debug`、`NS3_MTP=ON`、`NS3_WARNINGS_AS_ERRORS=OFF`。
M0–M4A 独立回归仍使用 [一键验证](../validation.md)。
本阶段的配套版本为 SimAI `c0ad1dd` 与 SimCCL `de18eaa`。

单次重放使用脚本生成的实验配置与 workload。请传入实际路径：

```sh
build/NULL/gem5.fast -d <result-dir> configs/example/pcie/cycle.py \
    <ring-8.json> --simai-library <libSimAI_pcie.so> \
    --simai-workload <ring-8.txt>
```

`ExperimentSpec.simai` 保存按 rank 排列的端点 ID、分段字节、
每目标 slot 数、待匹配消息上限、整轮记录上限和 workload SHA-256。
`workload.flows` 为空，动态流量由客户端释放。生成的 `cycle-result.json`
可导入离线 HTML 的「导入 gem5 结果」，原始 workload、拓扑、驱动库
和 trace 的来源可由输入、producer 与 artifact hash 复查。

## 显式任务输入

首行写 `PCIE_DAG_V1`，后续每行按顺序获得从 0 开始的 ID。
前驱用更早的行 ID 表示，满足全部前驱后任务方可运行。

| 行 | 字段顺序 | 完成条件 |
|---|---|---|
| `C` | rank、计算时长 ps、前驱 ID 列表 | 同队列中的计算事件结束 |
| `T` | rank、Astra 时间单位、时长、前驱 ID 列表 | 经过 Astra 相对调度接口后事件结束 |
| `M` | src、dst、scope、channel、tag、字节、Read 标记、接收注册延迟 ps、前驱 ID 列表 | 接收注册与完整到达都满足，交付 callback 后释放后继 |

`M` 的 Read 标记为 0 时使用 Write push，为 1 时由逻辑 dst 在
接收注册后向逻辑 src 发 MemRd。消息需是 4 B 对齐的正字节数。
同一匹配键中的重复 tag 按 FIFO 交付。scope 区分显式 communicator
命名空间；原生训练入口使用一个 world placement，并保留 SimCCL 的
channel/chunk/tag。

## 验收口径

回归先在 M4A 上确认动态提交入口缺失。原生 SimAI 首次进入
SimCCL 时复现了空 NVSwitch 通道访问，修正能力判断后又复现了
第一层传输完成但发送回调缺少 flowTag、第二层无法释放的问题。
上述行为分别由 native fixture、强制 NVLS 偏好和两层 Ring
依赖检查覆盖。显式 DAG 与 Astra 时间单位用例也先在原有入口
复现了输入拒绝，再执行完整通过路径。

| 退出门 | 执行证据 |
|---|---|
| V-27 | 原生夹具用嵌套同 Tick callback 检查 FIFO 和无 inline 重入。Astra API 通过 fs/ns/µs 用例，负值和溢出有明确错误。 |
| V-28/29 | 两级级联验证重复 tag、不同 scope/channel、两种接收顺序、Read pull。逐跳核对 MemRd/CplD。4 KiB 原生夹具校验实际数据、独立 Write 源响应和可见时刻。 |
| V-30 | 对真实 2/8 rank Ring、级联 P2P、8→1 incast、Host staging 的实际释放时刻重放，逐段对照接管/响应/完成、逐跳 TLP、整轮计数及排空。 |
| V-31 | 两层各 32 KiB 的原生 Ring 检查每层每 rank `2 × (N−1) × 32768 / N` B 和第二层释放依赖。4 rank 显式 Tree 经过 6 条 4 KiB 消息，归约在端点计时。 |
| V-32 | 原生 Train 与 Prefill、Decode、Diffusion、MoE 的显式计算与通信 DAG 在正常和慢链路上执行。MoE 的不均匀 Dispatch/Combine 共 14,336 B，按每条消息核对。 |
| V-33 | 原生 rank 错时完成。延迟注册接收产生可诊断截短，超过待匹配上限显式报错。正常场景在所有 rank、callback 和硬件排空后均通过。 |
| V-34 | 新 PCIe 库、已有 Analytical 与 NS3 分别构建运行。两套旧后端输出 CSV 与各自基线逐字节一致，NS3 每 rank 双向各 146,800,640 B。 |

在本轮验收 profile 下，8-rank Ring 两层共 1,792 笔 transfer。把链路周期
乘以 8 后，最后 rank 的完成从 22.716 µs 变为 28.776 µs。
时间为 gem5 Tick 经 1 THz 时间基换算，始于仿真 tick 0。
这些任务是小规模通信与计算依赖 fixture。计算时长是显式输入，
不代表测量的模型算力。输出包括 `simai-messages.tsv`、
`simai-tasks.tsv` 或原生 `EndToEnd.csv`，保留通信和任务时刻；
未定义 token 边界的实验不输出 TTFT/TPOT。
