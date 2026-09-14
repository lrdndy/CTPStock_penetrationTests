# SDK 来源与版本记录

本项目当前使用用户提供的 `traderAPI_3.7.5_CP_20251125(1).zip` 中的 Windows 64 位头文件、DLL、LIB 及 `error.dtd` / `error.xml`。公开仓库的 `sdk/win64` 保留原始头文件、LIB 和错误码文件；两只原厂 DLL 不提交到公开 Git，需由用户从同一 x64 目录复制到本地 `sdk/win64`。`sdk/include` 是四个头文件的 UTF-8、LF 换行转码副本，仅用于兼容中文注释与 `/utf-8` 编译，接口声明不变。纳入仓库的 LIB 没有改动。

原包内部来源目录为：

```text
3.7.5_CP_api_20251125_win/20251125_traderapi64_windows_se/
```

两只 Windows x64 DLL 的 `GetApiVersion()` 返回值均应为：

```text
v3.7.5_CP_20251125  9:30:02.f7e78374
```

本地必须放置以下文件，且文件名不得修改：

```text
sdk/win64/soptthosttraderapi_se.dll
sdk/win64/soptthostmduserapi_se.dll
```

`build_windows.bat` 和 CMake 均会在编译前检查它们是否存在。两只 DLL 已通过本地附件核验，但因公开仓库发布策略而被 `.gitignore` 排除。

接口命名空间是 `ctp_sopt`，链接文件带 `sopt` 前缀，不能换用期货 CTP 的 6.x 库。原包还包含 Windows 32 位和 Linux x64 子包；本工程只纳入并支持上述 Windows 64 位依赖，不以包内其他平台文件宣称已经完成对应平台验证。

## 3.7.0 到 3.7.5 的配套要求

3.7.5 的行情 API 类声明以及登录请求、登录响应结构与本工程原用的 3.7.0 一致；随包《375 API 变更说明》也没有列出行情登录变更。因此升级本身不能预先证明某次柜台登录超时一定由版本造成，仍需在指定实体机和评测前置做真实复测。

但完整头文件对比显示，3.7.5 已调整 Trader API/SPI 的虚函数接口，并变更若干共享结构和字段，例如新增证券现货持仓查询、组合删除所需字段，以及将行情 `Volume` 改为 64 位并移除原尾部 `BigVolume`。这些变化会影响 ABI；不得混用 3.7.0 与 3.7.5 的头文件、LIB 或 DLL，也不得只在运行目录热替换单只 DLL。

构建后应先执行 `run_windows.bat --version`，以两套 DLL 的运行时返回值核对实际加载版本。正式评测仍需由中信确认 SDK、前置环境、账号和 APPID 的匹配关系。

## 原始上传 ZIP 校验

```text
MD5     c78ec5960ea23bb3f817b354c14c10d2
SHA256  a0f891c1eb413a56d34a4450ebcb0ff7ba05e6dc814ebc778ea975fad7153cf1
```
