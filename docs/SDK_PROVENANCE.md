# SDK 来源与版本记录

本项目使用用户提供的 `traderAPI_3.7.0_T_20231127.zip` 中的 Windows 64 位头文件、DLL、LIB 及 error.xml。`sdk/win64` 保留全部原始文件；`sdk/include` 是头文件的 UTF-8 转码副本，仅用于兼容中文注释与 `/utf-8` 编译，接口声明不变。DLL 和 LIB 没有改动。

内部路径为 `3.7.0_20231127_winapi.zip/3.7.0_20231127_winapi/20231127_traderapi64_windows_se/`。注意接口命名空间是 `ctp_sopt`，链接文件带 `sopt` 前缀，不能换用期货 CTP 的 6.x 库。

外层文件名含 `_T_`，变更记录显示 `v3.7.0_CP_20231127`。仅凭命名不能替代柜台对评测版适用性的确认；运行时会记录 DLL 返回的 GetApiVersion。正式评测需由中信确认这套包、环境和 APPID 匹配。

原包还含 Windows 32 位包和 Linux 包；此次仅打包 Windows 64 位依赖。Linux 子包仅发现交易 `.so`，未提供行情 `.so`，不宣称可用它完成行情测试。

变更记录说明行情结构新增 64 位 `BigVolume`，用于原 `Volume` 超出 int 范围的情况。本阶段不订阅行情；后续处理成交量需注意该字段。

原始上传 ZIP 校验：

```text
MD5     1b7cd3a203d3b59eabe0f389b7ca8cd3
SHA256  796942c151f3629dab980cc33a106ce07778cfebbbb10b8ca7fc18f934cc21b7
```
