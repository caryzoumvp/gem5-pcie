# M4A 配置驱动的 gem5 与离线 HTML 验收

M4A 在 NULL/fast 下使用 M3 已验收的 FLIT 周期数据面。输入是
`pcie6-cycle-experiment-v1`，时间与载荷分别用 ps 和 B。
`compile_cycle` 在实例化前校验结构、端口/lane、地址窗口、
资源预算、链路时序和 workload 依赖。`cycle.py` 将配置编译成
实际的叶节点、Switch 和双向 Link，运行后产出
`pcie6-cycle-result-v1`。

## 退出场景与运行结果

| 场景 | 验收证据 |
|---|---|
| V-20 格式兼容 | 63 个硬件字段与七组旧 HTML oracle 数值逐项相同；本地 Node 页面夹具导出旧 hardware-calibration 并验证其结果，gem5 导入入口拒绝该格式。原 workspace 导入/导出处理器逐字节一致。 |
| V-26 配置和结果回导 | 页面夹具导出 64 B Read，原生 gem5 在 41,000 ps 完成，两个链路各记录一笔 MemRd 和一个反向 CplD。将真实 ResultBundle 导回页面，核对单位、配置 hash、窗口、模式、历史与同配置对照。错版本/单位/hash 负例拒绝，历史记录仍绑定原快照。真实 `bulk-read-wire` 性能报告的目标 FAIL 与正确性 PASS 分别显示。 |
| V-36 配置建图 | 原生测试覆盖单颗、双颗和空闲分支，逐链路匹配请求/Completion 方向、分类和数量。4 KiB 双颗场景在每个活动链路上取得 16 MemWr、8 MemRd、16 CplD。空闲分支零数据 TLP。方向拍宽/周期变体按 `ceil(256/width) × cycle_ps` 核对所有 FLIT 的串行化占用。 |
| V-37 独立 workload | 64 B/4 KiB 读写、8→1、显式 collective 和依赖 DAG 均检查字节及实际完成；8→1 完成 16 笔 64 B 事务。目标服务减慢时，Write 目的可见与依赖 Read 接管由 54,000 ps 后移至 151,000 ps。缩短期限时报告截短、零样本及未完成；坏 Read 数据阻止依赖流。完成场景的等待图为空。 |

测试驱动记录包括以下语义负例：未实现的树路由返回错误端口和
接受无效连接；缺少 HTML 交换模块时页面夹具失败；错误 COMP
测点假定导致真实吞吐报告测试失败。修正后用独立逐跳计数和
真实报告复验。Write 源端的本地响应与目的可见分别记录，DAG
后继依赖实际可见或 Read 完整组装。

## 复现入口

```sh
python3 util/pcie_lab/validate.py --skip-build --output-dir output/pcie-validation
```

已有构建时执行上述命令。省略 `--skip-build` 可用同一入口构建
所需目标。归档结果包含 70 项 C++ 检查、44 项 Python 检查、
76 项 TestLib 检查、真实 HTML 配置/结果往返，以及 M3 原有的
28 项性能判定。各结果保存原配置、规范化参数、源文件哈希和
window/start/end/final 等计数，便于复查和在 M4B 重放。

可在离线 HTML 的「硬件规格与仿真」或「验证记录」中选择
「导入 gem5 结果」。支持 CLI 的 `cycle-result.json` 和
TestLib 拓扑案例目录里的 `result.json`。页面校验文件内输入
快照的哈希，并将结果存入浏览器的「验证记录 → 仿真」。

独立端点和内存是合成模型，适用范围为单域透明树、VC0、所选
对齐 Memory TLP、已初始化 Gen6 和 store-and-forward。配置中
任意 graph、跨域和未覆盖的协议能力由输入校验诊断。页面
验证在 Node DOM 夹具内执行实际离线脚本。M4B 的接入边界见
[实施计划](../PCIe6_Implementation_Plan.md)。单次运行用法见
[一键验证](../validation.md)。
