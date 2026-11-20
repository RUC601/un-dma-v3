# UNDMA V3 Agent Context

`01_PRODUCTS/un-dma-v3` 是 UniCore 的独立 Git 工作区，分支 `v3/unicore`。V2 继续在 `01_PRODUCTS/un-dma-v2` 负责日常功能、版本和库更新。V3 按具体任务吸收所需的 V2 变更，记录来源提交。

V3 的状态、实验资料和恢复入口见 [V3 进度](../docs/un-dma-v3/V3_PROGRESS.zh-CN.md)。产品文档位于 `../docs/un-dma-v3`，研究位于 `../../02_RESEARCH/un-dma-research/v3-product-research`，验证和运行证据位于 `../../04_SCENARIO/un-dma-v3`。各产品的配置、构建和运行产物各自维护。

## 用户指定保留的直接 CD 读取研究（2026-11-05）

`include/Game/DirectCooldownResearch.hpp` 是用户明确要求长期保留、便于分享的完整注释代码档案，记录已跑通的 CN154905N 非本地技能 CD 读取实现、映射及证据。正式版采用 active 状态计时器；该档案不参与编译或运行。禁止 AI 以死代码、实验已退役、清理或精简为由自行删除、缩写或迁出。只有用户明确要求删除此研究成果时才能删除；维护工具或后续重构必须保留注释正文与溯源说明。
