# 最小行情登录对照项目

此目录是一份独立、单文件的 CTP 个股期权行情登录测试。它不调用主项目的配置解析、状态机、请求号过滤或日志封装，只执行：

1. 创建行情 API；
2. 注册原始回调；
3. 连接 `tcp://101.226.254.157:32213`；
4. 用 BrokerID `1000`、用户 `887120202987` 和隐藏输入的交易密码调用一次 `ReqUserLogin`；
5. 原样打印连接、登录、错误、断线和心跳回调。

它不使用 APPID/AuthCode，因为该 SDK 的行情接口没有 `ReqAuthenticate`；它也不订阅行情、不查询账户、不报单。

## 需要准备的文件

在本目录新建 `sdk` 文件夹：

```powershell
cd minimal_md_test
mkdir sdk
```

从升级分支根目录的 `sdk\include\` 复制三个 UTF-8 头文件到 `minimal_md_test\sdk\`：

```text
ThostFtdcMdApi.h
ThostFtdcUserApiStruct.h
ThostFtdcUserApiDataType.h
```

从原始压缩包的 Windows x64 目录复制一只 LIB 和一只 DLL 到同一个 `minimal_md_test\sdk\`：

```text
traderAPI_3.7.5_CP_20251125(1).zip
└─ 3.7.5_CP_api_20251125_win
   └─ 20251125_traderapi64_windows_se
      ├─ soptthostmduserapi_se.lib
      └─ soptthostmduserapi_se.dll
```

准备完成后的目录必须是：

```text
minimal_md_test\
├─ main.cpp
├─ build_windows.bat
├─ run_windows.bat
└─ sdk\
   ├─ ThostFtdcMdApi.h
   ├─ ThostFtdcUserApiStruct.h
   ├─ ThostFtdcUserApiDataType.h
   ├─ soptthostmduserapi_se.lib
   └─ soptthostmduserapi_se.dll
```

必须使用 `20251125_traderapi64_windows_se`，不能使用 Windows 32 位目录、Linux `.so` 或 3.7.0 文件。`sdk` 整个目录已被此子项目的 `.gitignore` 排除，不会上传 DLL 或本地 SDK 副本。

## 编译和运行

在 Windows PowerShell 中执行：

```powershell
cd D:\projects\CTPStock_API\CTPStockConnectivity_v0.1.0\CTPStockConnectivity\minimal_md_test
.\build_windows.bat
.\run_windows.bat
```

程序会提示输入交易密码，输入过程不回显字符。密码只保存在当前进程内存中，不写源码、命令行或日志。

启动时必须先看到：

```text
API: v3.7.5_CP_20251125  9:30:02.f7e78374
```

关键输出的含义：

| 输出 | 含义 |
| --- | --- |
| `CALLBACK OnFrontConnected` | SDK 已建立前置连接 |
| `RETURN ReqUserLogin immediate_rc=0` | 本地 SDK 接受了登录请求，但不代表柜台登录成功 |
| `CALLBACK OnRspUserLogin ... error_id=0` | 柜台返回登录成功 |
| `CALLBACK OnRspUserLogin ... error_id!=0` | 柜台明确拒绝，可按错误码排查 |
| 60 秒超时且没有登录/错误回调 | 请求已提交但柜台没有给本程序最终响应 |

如果主程序和这个最小项目在同一电脑、同一时间、同一账号、同一 3.7.5 SDK 下都表现为 `OnFrontConnected` 后登录超时，那么主程序的配置解析、状态过滤和复杂流程基本可以排除，应该让券商查询行情前置的服务端日志、账户行情权限和 SDK/柜台匹配关系。该结论是强对照证据，但不能仅凭客户端现象数学意义上证明任何代码都绝对无误。
