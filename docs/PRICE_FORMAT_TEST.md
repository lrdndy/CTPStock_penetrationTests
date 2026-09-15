# 价格包含字母的提示截图测试

本次仅新增独立测试文件，主程序源码、版本、SDK、价格上下限配置、每日和每秒风控均不修改。

在项目目录执行：

```powershell
git pull origin master
.\build_windows.bat
.\run_price_windows.bat trigger 0.0427abc
```

如果已有当前版本的 EXE，可以省略编译。输入 `abc` 也可测试纯字母。Windows 会显示中文“价格异常提示”，包含原始输入、错误原因、没有发送报单、每日／每秒计数不增加。请在关闭弹窗前截图；点击确定后应看到 `RESULT PASS`。日志位于输出的 `EVIDENCE log=...` 路径。

脚本确实调用现有 `ctp_stock_connect.exe` 的参数解析。先用合法数字 `0.0427` 做对照，再传入你指定的错误价格；两次都带一个末尾的未知参数作为停止点。现有程序在解析阶段就退出，不读取连接配置、不请求密码、不创建 API 对象、不连接柜台，也不调用报单或计数代码。只有错误价格被程序明确拒绝，才显示中文错误弹窗；不会靠脚本自行认定错误并伪造成功结果。

正常数字负向对照：

```powershell
.\run_price_windows.bat trigger 0.0427
```

此时应返回 `RESULT FAIL`，不显示价格错误弹窗，因为没有触发待测错误。程序不兼容、DLL 缺失、未编译或未收到预期拒绝信息也都会失败，不生成通过结论。示例数字只用于格式测试，不是报单价格建议。本测试沿用主程序原有数字语法，不能证明数值价格上下限、偏离行情或最小变动价位风控。

脚本面向 Windows PowerShell 5.1，文件使用 UTF-8 BOM 保存中文。Linux 可运行 `python3 tests/run_price_format_parser_test.py` 验证同一主程序解析及停止点；该检查不等同于 Windows 原生弹窗验证。Windows 上须实际运行脚本并截图。
