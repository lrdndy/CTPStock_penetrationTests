# 股票期权 API 连接测试第一阶段

本项目是为附件六股票期权接入测试准备的第一步：在 Windows 64 位实体机上验证交易前置连接、客户端认证、账户登录、资金查询，以及行情前置连接和登录。源码版本为 `0.1.0`。本包包含源码和原始 Windows SDK 依赖，需要在 Windows 上编译生成 EXE。

本阶段没有报单、撤单、密码修改、结算确认、策略或风控交易功能，也不订阅行情。行情登录成功只能证明行情登录链路；要证明行情推送，需要下一阶段指定有效合约并收到行情回调。附件六后续范围见 [REPORT_ROADMAP.md](docs/REPORT_ROADMAP.md)。

当前交付不是已通过的正式测试报告。实际柜台登录结果、Windows 编译与运行结果，以你的实体机执行结果为准；没有预先生成或冒充成功的截图和录屏。代码说明见 [CODE_GUIDE.md](docs/CODE_GUIDE.md)，离线验证范围见 [VALIDATION.md](docs/VALIDATION.md)。

## 最快运行方式

1. 将工程解压到例如 `D:\projects\CTPStockConnectivity`。建议初次使用短的英文路径。
2. 在 Windows 实体机安装带“使用 C++ 的桌面开发”和 Windows SDK 的 Visual Studio 或 Build Tools。使用 x64 工具链。
3. 以管理员身份打开 PowerShell，进入工程根目录后执行：

```powershell
cd D:\projects\CTPStockConnectivity
.\build_windows.bat
.\run_windows.bat --mode all
```

`build_windows.bat` 自动寻找 Visual Studio 并调用 x64 编译器，不依赖 CMake。成功后生成 `build\bin\ctp_stock_connect.exe`，同时复制两只 DLL。源码中包含中文注释，编译采用 UTF-8 头文件副本。

程序会要求输入交易密码和 AuthCode。输入时不显示字符，这是正常现象。使用本次账号对应的密码和邮件中箭头右侧的认证码；不要把箭头或 APPID 一起输入。当前包不保存这两项。日志保留完整账号以便登录截图核验，所以向他人发送日志前仍需检查内容。

如果找不到编译器，用 Visual Studio 的 **x64 Native Tools Command Prompt** 再执行 `build_windows.bat`。若报缺 Windows SDK 或 C++ 工具，则在 Visual Studio Installer 中补齐组件。

## 配置与模式

`config/connection.ini` 已填入本次提供的公开连接参数与账号标识：

| 配置项 | 当前值 | 用途 |
| --- | --- | --- |
| broker_id | 1000 | 柜台经纪公司代码 |
| user_id | 887120202987 | 交易与行情登录账号 |
| investor_id | 887120202987 | 资金查询投资者代码，暂按与登录账号一致配置 |
| app_id | client_shunjingsf_v1.0.0 | 邮件中的 APPID 原文 |
| trader_front | tcp://101.226.254.157:32205 | 交易评测前置 |
| md_front | tcp://101.226.254.157:32213 | 行情评测前置 |

AppID 中的 `v1.0.0` 是已申请的标识组成部分，不能因为样例源码版本是 `0.1.0` 就随意修改。投资者代码若与登录账号不同，应按中信提供的值修改 `investor_id`。第一阶段程序只允许上述评测前置和 BrokerID，不支持将配置直接切到生产环境。

```powershell
# 分别排查交易与行情
.\run_windows.bat --mode trader
.\run_windows.bat --mode md

# 仅测试交易认证与登录，暂时跳过资金查询
.\run_windows.bat --mode trader --skip-query

# 单个步骤最长等待 60 秒
.\run_windows.bat --mode all --timeout 60

# 查看帮助与本机实际加载的 API 版本，不发起登录
.\run_windows.bat --help
.\run_windows.bat --version
```

默认每一步等待 30 秒，整个程序可能经历多个步骤，因此总用时不等于 30 秒。程序不会在失败后自动重试认证或登录。`all` 顺序运行交易和行情链路，具体执行与返回状态以日志为准。

也支持从进程环境变量 `CTP_PASSWORD`、`CTP_AUTH_CODE` 读取凭据，以便后续自动化。初次调试推荐交互输入；不要把包含密码的命令写入共享脚本、PowerShell 历史、截图或录屏。环境变量不是加密保险箱，使用完毕应清除。

## 怎么判断结果

交易链路的顺序是：前置连接 → APPID 与 AuthCode 认证 → 账号与密码登录 → 资金查询。行情链路是：前置连接 → 行情登录；本包行情接口没有 `ReqAuthenticate`，不能照搬交易认证函数。

`Req...` 返回 0 只说明 API 接受了请求，不等于柜台已接受登录。应继续看 `OnRsp...` 响应中的 `ErrorID`、请求编号和最后一条响应标志。资金查询收到正常结束但没有记录，会注明空结果；它不等于“资金为零”。各阶段和最终结果会同时写入屏幕和 `logs` 下的独立日志。API 内部流文件在 `flow` 下，交易与行情分开存放。

| 停在哪一步 | 优先检查 |
| --- | --- |
| EXE 尚未进入主程序 | EXE 与 DLL 是否完整、是否 x64、DLL 是否位于 EXE 同目录 |
| 前置连接超时 | 本机网络、评测服务开放时间、IP/端口、白名单与防火墙 |
| 前置连接后认证失败或超时 | APPID、AuthCode、BrokerID、账号绑定以及 API 与环境匹配；有错误码时保留原文 |
| 认证成功但登录失败 | 交易密码、账号状态、登录权限；不要把 AuthCode 当成交易密码 |
| 登录成功但资金查询失败 | investor_id 是否正确、查询权限、柜台状态；用 `--skip-query` 单独验证登录 |
| 连接中断 | 保留断开原因与时间，检查链路后手动重跑；SDK 底层重连不代表本程序会重复登录 |

必要时在该 Windows 电脑单独检查 TCP 端口：

```powershell
Test-NetConnection 101.226.254.157 -Port 32205
Test-NetConnection 101.226.254.157 -Port 32213
```

`TcpTestSucceeded=True` 仅说明 TCP 端口可达，不代表认证、登录或穿透式监管校验通过。

## 保存这次真实测试证据

运行前确认使用实体机并以管理员身份启动终端，按中信要求在评测 API 和评测环境中测试。管理员状态可以检查；实体机性质不能单凭程序打印的一行文字证明。

可在同一台机器执行以下只读脚本，记录本机当前时间、管理员状态、活动网卡 IPv4/MAC 和到评测地址的路由选择：

```powershell
.\scripts\collect_environment.ps1
```

若本机策略不允许运行脚本，可以直接用 `Get-NetIPConfiguration` 与 `Find-NetRoute -RemoteIPAddress 101.226.254.157` 查看，无需为此改变机器级策略。存在多块网卡时，以实际到前置的路由所选接口为准，不凭第一条网卡记录填写报表。本地采集结果用于人工核对，不能替代柜台对终端信息的核验。

登录成功后保存包含账号、APPID、API 版本、时间及登录成功结果的屏幕截图，并按用户提供的要求当天及时告知中信核验。本阶段不交易；后续两所交易发生后也需当天通知。不要在通知中填写未实际发生的交易。

此阶段以代码和连接说明为主。正式图文手册、功能截图与录屏，需要实际运行并在对应功能实现后补充；不能拿离线验证或说明文字代替实测证据。

## 运行包与 MD5

Windows 编译成功后，可以制作此阶段运行包：

```powershell
.\scripts\package_release.ps1
```

脚本在 `dist` 下创建带时间的独立运行包和对应 MD5/SHA256 文本，不收录 `logs`、`flow` 或环境变量。运行包保留 `build\bin` 布局，解压后从根目录执行 `run_windows.bat`。这只是第一阶段的运行快照；该脚本不负责病毒检测，也不证明其满足全部评测要求。

最终附件六的 MD5 必须针对最终提交的那一份 ZIP 或安装包。备份应保留同一文件；软件或包内容改变后须重新打包、重算 MD5，不能沿用本次源码包的值。建议同步保留 SHA256。

## 工程文件与后续扩展

| 文件 | 作用 |
| --- | --- |
| src/main.cpp | 核心连接程序与中文注释 |
| config/connection.ini | 账号标识、APPID 和评测地址 |
| sdk/win64 | 用户附件的原始 Windows 64 位 SDK |
| sdk/include | 编译使用的 UTF-8 头文件副本，声明不变 |
| build_windows.bat | 自动查找 Visual Studio 并编译 |
| run_windows.bat | 设置工作目录与控制台编码后启动 |
| CMakeLists.txt | 已安装 CMake 时可选的构建入口 |
| scripts/collect_environment.ps1 | 只读采集本机环境证据 |
| scripts/package_release.ps1 | 生成运行快照和哈希值 |
| docs/CODE_GUIDE.md | 按真实类与函数解释代码 |
| docs/REPORT_ROADMAP.md | 对照附件六安排后续范围 |
| docs/SDK_PROVENANCE.md | SDK 来源、版本差异与校验记录 |
| docs/VALIDATION.md | 本次已验证和未验证的边界 |

可选 CMake 构建应在 x64 开发者终端执行：

```powershell
cmake -S . -B build-cmake -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build-cmake
.\build-cmake\bin\ctp_stock_connect.exe --config config\connection.ini --mode all
```

后续先根据这次真实日志解决连接问题，再接有效合约行情、两所基础交易和风控。每日/每秒最大报单阈值以报备表为准，目前尚未提供，不能自行填一个数作为正式验收值。

## 构建依据

自动发现 C++ 工具链采用微软公开的 [vswhere 查找 VC 方法](https://github.com/microsoft/vswhere/wiki/Find-VC)。源码和头文件副本统一使用 UTF-8，编译参数含 [/utf-8](https://learn.microsoft.com/en-us/cpp/build/reference/utf-8-set-source-and-executable-character-sets-to-utf-8?view=msvc-170)。业务接口依据本次用户提供的头文件，而非期货 CTP 6.x 示例。
