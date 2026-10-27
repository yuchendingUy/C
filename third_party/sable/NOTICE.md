# Sable 移植说明

`src/phys/` 下的物理模块是 Sable（https://github.com/ryanhcode/sable ，作者 RyanHCode 及贡献者）
服务端物理部分的 C 语言移植与修改版本，原项目以 PolyForm Shield License 1.0.0 发布，
许可全文见同目录 `LICENSE.md`（https://polyformproject.org/licenses/shield/1.0.0）。

主要修改：
- 物理引擎由 Rapier（Rust）替换为 Box3D（C，MIT 许可，见 `third_party/box3d/LICENSE`）
- 接入 mcc 的区域化多线程调度与通用任务线程池
- 物理坐标改为双精度大世界模式

许可证"非竞争"条款：本移植仅用于运行自己的服务器，
不得作为与 Sable 竞争的产品（例如作为 Sable 的替代品）对外提供。
