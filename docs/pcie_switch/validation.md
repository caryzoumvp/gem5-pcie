# PCIe 6.0 Switch 一键验证

在仓库根目录执行一条命令，构建 NULL gem5 和 PCIe 单元测试，然后
运行 M0–M4A 回归：

```sh
python3 util/pcie_lab/validate.py
```

已有与当前源码匹配的 `build/NULL/gem5.fast` 和
`build/NULL/dev/pci/pcie/pcie_*.test.opt` 时，可跳过构建：

```sh
python3 util/pcie_lab/validate.py --skip-build
```

需要 Python 3、Node.js 和仓库中的 Base Specification PDF、离线
HTML。默认构建还需要 gem5 的 SCons 和编译依赖。
`-j N` 设置 SCons 并行度和 TestLib 线程数，默认 4。
如 Conda 的 `protoc` 与链接库版本不匹配，按 [M1 构建记录](validation/M1.md)
配置匹配的 `PROTOC` 和 `LINKFLAGS_EXTRA` 后运行同一命令。

| 阶段 | 脚本执行的检查 | 详细证据 |
|---|---|---|
| M0 | Python 合同测试、两份 CLI 示例、来源锁定 | [M0](validation/M0.md) |
| M1 | 从 HTML 重新生成 oracle 并比对 fixture，TestLib 对照 S-01–S-07 | [M1](validation/M1.md) |
| M2 | C++ 单测和 TestLib 覆盖请求、重试、有限资源、反压和排空 | [M2](validation/M2.md) |
| M3 | FLIT/FC/replay、注错、周期 trace、置换、26 组性能用例及两种回程的 SW-TAIL | [M3](validation/M3.md) |
| M4A | 配置建图、独立 workload、结果回导和离线兼容 | [M4A](validation/M4A.md) |

默认在仓库根目录为每次运行新建一个 `gem5-pcie-validation-*` 子目录。
用 `--output-dir` 指定其他父目录。相对路径以启动命令时的工作目录为准。

```sh
python3 util/pcie_lab/validate.py --skip-build --output-dir /tmp/pcie-validation
```

脚本启动时打印本次输出目录。各阶段日志、TestLib XML、模拟器 trace
和性能 JSON 都保存在该目录。
`performance-verdicts.json` 汇总数值目标判定，原始报告保留测点和数据
正确性结果。

脚本退出 0 表示回归、数据正确性和规定的验收判据通过。读吞吐的已知
未达标项仍报告 `FAIL`，RTT-64 缺 PHY 和端点标定时报告
`NOT_COMPARABLE`。三通道写置换作为容量反例必须报告 `FAIL`，
Shared FC TX 缺能力负例必须返回指定配置错误。其他意外结果使脚本
非零退出，并打印对应日志位置。

## M4B SimAI 闭环

```sh
python3 util/pcie_lab/validate_simai.py --simai-root /workspaces/SimAI \
    --output-dir output/pcie-simai
```

先构建 gem5，再用上述命令构建 SimAI PCIe 库并执行原生闭环、
同路径重放和离线 HTML 回归。完整旧后端回归的参数、测点与产物见
[M4B 验收](validation/M4B.md)。

### 查看实际拓扑和流量

脚本打印的 `gem5-pcie-simai-*` 目录中，根目录下的 `<case>.json`
是实验输入，`<case>.txt` 是 workload，`<case>/cycle-result.json`
是运行结果，`summary.json` 汇总校验。以下以
[gem5-pcie-simai-176jvcjx](../../output/pcie-simai/gem5-pcie-simai-176jvcjx)
为例。

| 要核对的内容 | 文件和字段 |
|---|---|
| 物理拓扑 | `cycle-result.json` 的 `topology.nodes`、`topology.links`。每条 link 给出 A/B 节点、端口和方向参数。 |
| rank 到硬件 | `input.simai.rank_endpoints` 的数组下标是 rank，数组值是 gem5 节点 ID。 |
| 业务流量矩阵 | 在 `simai-messages.tsv` 仅统计 `event=delivered`，按 `src`、`dst` 对 `bytes` 求和。 |
| 消息如何下沉到物理路径 | 用 TSV 中相同的 `message` 找全部 `submit` 行。`submit.transfer=N` 对应结果里的 `pcie-transfer-N`，其 `path` 和 `observed_links` 分别给出节点路径和每跳 TLP。 |
| 每条链路方向上的数据量 | 按 `topology.links` 下标取 `edge_<i>_forward_data_bytes` 和 `edge_<i>_reverse_data_bytes`，计算 `counters.end − counters.start`。 |
| 任务何时推进 | 显式 DAG 查 `simai-tasks.tsv` 的任务开始和完成时刻，前驱定义见同名 workload。原生任务的分层统计见 `EndToEnd.csv`。 |

打开离线 HTML，在「验证记录 → 导入 gem5 结果」选择
`cycle-result.json`，展开「输入快照、拓扑、流与计数」。
`config.dot.svg` 展示 gem5 对象结构。物理连线及端口以
`topology.links` 为准。逻辑 Ring 和逻辑归约 Tree 均可映射到
一颗交换机的物理星形。

用 `jq` 从仓库根目录列出 incast 的物理拓扑和双向 payload 字节。
把路径换成别的用例即可查看其结果：

```sh
jq -r '
  .counters as $c |
  (["link","A","B","A→B (B)","B→A (B)"] | @tsv),
  (.topology.links | to_entries[] |
    .key as $i | .value as $e |
    "edge_\($i)_forward_data_bytes" as $f |
    "edge_\($i)_reverse_data_bytes" as $r |
    [$e.id,$e.a.node,$e.b.node,
      ($c.end[$f]-$c.start[$f]),($c.end[$r]-$c.start[$r])] | @tsv)
' output/pcie-simai/gem5-pcie-simai-176jvcjx/incast/cycle-result.json
```

每跳 payload 相加会多次计入跨多跳传输的同一批字节。
业务完成量看 `measurement.completed_payload_bytes`。
FLIT 开销和重放另看对应方向的 `flit_original`、`flit_control`
及 `flit_replayed`。

### 对照 M4B 验收场景

| 场景 | 本轮可以看到的证据 |
|---|---|
| V-27 | `clock`、`negative-delay`、`overflowing-delay` 分别覆盖跨单位计时和错误诊断。 |
| V-28/29 | `matching` 的物理路径是 `root → sw0 → sw1 → dst0`。4 KiB Read 拆成四笔 1 KiB，逐笔在三条链路上各有反向 1 个 MemRd、正向 4 个 CplD。 |
| V-30 | `matching`、`incast`、`host-staging` 与 `ring-2/8` 有独立 replay。8→1 incast 的源侧每条链路为 4,096 B，汇聚的 `link0` 反向为 32,768 B。 |
| V-31 | `ring-8` 的两层 32 KiB AllReduce 每 rank 发送 114,688 B，整体 896 KiB。1,792 笔 transfer 与独立 replay 一致。`tree` 的 4 rank 逻辑归约及广播共传输 24 KiB。 |
| V-32 | 链路周期乘以 8 后，`ring-8` 的第二层最早发流时刻由 11.475 µs 推迟到 14.494 µs。`moe` 的 rank 2 在 Dispatch 汇聚 6 KiB，再按原分布 Combine。 |
| V-33 | `unmatched-timeout` 已在硬件层完成 64 B，SimAI 仍缺一个接收注册，结果按 `truncated` 保存。`capacity-exceeded` 明确拒绝超过上限的消息。 |
| V-34 | 该目录包含 PCIe 的构建、执行及版本记录。运行时附带两个旧后端二进制参数，才会生成本轮的 Analytical 和 NS3 回归证据。 |

完整判据见 [验证场景](PCIe6_Validation_Scenarios.md#6-simai需求覆盖)。

### Llama 7B TP2 模型 workload 配单交换机星形

源文件位于 SimAI 的 AICB 子模块 `23eec3c48ca2`，
`workload/simAI/model_workload/L7B-M1-C04_Llama7B_megatron_tp2_pp1_mbs1_A100.txt`。
使用原文件第 7–8 行的第一个 attention/MLP block。两行的计算输入、
collective 类型、每次 16 MiB 的通信大小均保留。实验头明确采用
TP2、PP1、DP1，仅执行一个 TP 组。原始 AICB 训练 workload 配套较大
的数据并行组，本例是模型派生的单 block 仿真。配套 physical CSV
没有对应的实机 elapsed-time 数据。

物理拓扑中心为 `sw0`，三个 Gen6 x16 端口分别连到 `root`、
`dst0` 和 `dst1`。两张逻辑 GPU 分别占用 `dst0` 和 `dst1`。
`root` 作为上行根节点保留，在本次 P2P 路径上无 payload。

本次结果为 `pass`，全部排空。前向与输入梯度合计 4 次
AllReduce，32 条 SimAI 消息，共 **128 MiB** 业务 payload。
每 rank 发送 64 MiB，两个 GPU 端口各方向的链路 payload 都是
64 MiB。8,192 笔 16 KiB transfer 的接管到可见 P50 为
**6.432 µs**。两个 rank 均在仿真起点后 **16.441882 ms** 结束。
200 ms 是采样窗口，完成时刻及每层统计见 `EndToEnd.csv`。

本次产物在 [llama7b-tp2-star](../../output/pcie-simai/llama7b-tp2-star)，
包括 [实验配置](../../output/pcie-simai/llama7b-tp2-star/experiment.json)、
[裁剪后的 workload](../../output/pcie-simai/llama7b-tp2-star/workload.txt)、
[周期结果](../../output/pcie-simai/llama7b-tp2-star/cycle/cycle-result.json)、
[消息 TSV](../../output/pcie-simai/llama7b-tp2-star/cycle/simai-messages.tsv)
和 [分层统计](../../output/pcie-simai/llama7b-tp2-star/cycle/EndToEnd.csv)。
从仓库根目录重新准备输入并运行，库路径可换成最近一次 M4B
生成的 `libSimAI_pcie.so`：

```sh
python3 - <<'PY'
from pathlib import Path
from util.pcie_lab.validate_simai import session_config, window_spec, write_json

source = Path("/workspaces/SimAI/aicb/workload/simAI/model_workload") / (
    "L7B-M1-C04_Llama7B_megatron_tp2_pp1_mbs1_A100.txt"
)
out = Path("output/pcie-simai/llama7b-tp2-star")
out.mkdir(parents=True, exist_ok=True)
rows = source.read_text().splitlines(keepends=True)[6:8]
assert [row.split()[0] for row in rows] == ["attention_layer", "mlp_layer"]
workload = out / "workload.txt"
workload.write_text(
    "HYBRID_TRANSFORMER_FWD_IN_BCKWD model_parallel_NPU_group: 2 "
    "ep: 1 pp: 1 vpp: 1 ga: 1 all_gpus: 2 checkpoints: 0 "
    "checkpoint_initiates: 0\n2\n" + "".join(rows)
)
spec, _ = session_config(window_spec(3), 2, workload)
spec["simai"].update(
    rank_endpoints=["dst0", "dst1"], segment_bytes=16384, flow_limit=32768
)
for node in spec["topology"]["nodes"]:
    if node["kind"] != "switch":
        node.update(base=node["requester_id"] * 65536, size=65536)
spec["measurement"].update(window_ps=200_000_000_000,
                           timeout_ps=200_000_000_000)
write_json(out / "experiment.json", spec)
PY

pcie_gem5_root="$PWD"
pcie_client=../gem5-pcie-simai-176jvcjx/build/libSimAI_pcie.so
(
    cd output/pcie-simai/llama7b-tp2-star &&
    "$pcie_gem5_root/build/NULL/gem5.fast" -d cycle \
        "$pcie_gem5_root/configs/example/pcie/cycle.py" experiment.json \
        --simai-library "$pcie_client" --simai-workload workload.txt \
        > run.log 2>&1
)
```

## 运行一个导出的周期实验

在离线 HTML 的硬件页生成或载入 ExperimentSpec，再导出 JSON。
在仓库根目录执行：

```sh
build/NULL/gem5.fast -d m5out configs/example/pcie/cycle.py \
    util/pcie_lab/examples/cycle_star.json
```

也可将示例路径换成页面导出的文件。仿真接受以 ps 和 B 表示的
单域透明树配置，输出 `m5out/cycle-result.json`。结果包含配置
hash、实际连接和逐流链路计数、测量窗口、完成与排空信息。
用硬件页的 gem5 结果入口导入，历史快照和独立参考实验分别保存。
配置错误在创建 gem5 对象之前给出字段诊断。失败或截短的仿真仍保存
结果文件，并以非零状态退出。
