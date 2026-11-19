# PCIe 6.0 Switch 一键验证

在仓库根目录执行一条命令，构建 NULL gem5 和 PCIe 单元测试，然后
运行 M0–M3 回归：

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

该入口覆盖 gem5 独立模拟的 M0–M3。M4 SimAI 闭环和 M5 扩展的
验收边界见 [实施计划](PCIe6_Implementation_Plan.md) 和
[验证场景](PCIe6_Validation_Scenarios.md)。
