Mocha UEFI 是一个面向 Xiaomi Mi Pad 1（设备代号：Mocha）的 UEFI 启动与 Windows 移植实验项目。

本项目旨在探索为该设备建立独立的 UEFI 启动环境，并尝试通过 UEFI 引导 Windows，逐步研究启动流程、系统交接以及设备兼容性。

Project Status

<badge color="success">ExitBootServices reached</badge>

目前项目已经在实体设备上完成了初步启动测试，并成功执行至 "ExitBootServices()"。

已观察到的返回结果：

ExitBootServices = 0x0000000000000000

返回值 "0" 对应 "EFI_SUCCESS"，表明本次 UEFI Boot Services 退出操作成功。

Current Progress

- [x] UEFI 启动程序运行
- [x] 文件加载阶段
- [x] 执行 "ExitBootServices()"
- [x] 成功返回 "EFI_SUCCESS"
- [ ] 后续启动阶段验证
- [ ] Windows Boot Manager 启动验证
- [ ] Windows 内核启动验证
- [ ] 设备硬件兼容性测试
- [ ] Windows 系统运行测试

Development

目前项目仍处于早期实验阶段。

虽然已经成功推进至 "ExitBootServices()"，但后续启动流程尚未完成实践验证。

接下来还需要一定时间进行研究、代码开发和实体设备测试，逐步解决 UEFI 与后续系统启动阶段之间的交接问题。

Disclaimer

本项目目前仅完成部分 UEFI 启动流程验证

所有尚未完成的功能均以实际测试结果为准。

项目进展将随着后续开发持续更新。
