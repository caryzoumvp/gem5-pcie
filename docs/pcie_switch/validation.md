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
