# updata

Windows 平台的**差异化自动更新**程序，外加一个可供**易语言**等宿主程序调用的更新检测 DLL。纯 C + Win32 API 实现，无第三方依赖。

| 文件 | 说明 |
| --- | --- |
| `updata.exe` | 带界面的更新程序：按服务端清单做 SHA-256 差异比对、多线程下载、校验后替换、占用检测与自动解锁，更新完成后可自动启动新版本 |
| `updata32.dll` | 更新检测模块（32 位，易语言用这个）：只判断"是否需要更新"，需要更新时释放内嵌的 `updata.exe` 并结束宿主进程，实现"启动即自更新" |
| `updata64.dll` | 同上，64 位宿主使用 |

## 特性

| 能力 | 说明 |
| --- | --- |
| 差异化更新 | 逐个文件比对服务端 SHA-256，只下载有变化的文件 |
| 多线程下载 | 多文件并发（最多 4 线程），单个文件失败自动重试（最多 3 次） |
| 校验后替换 | 先下载到 `*.updtmp`，SHA-256 校验通过才替换目标文件，不会写坏原文件 |
| 占用检测 | 更新前检测文件是否被占用，用 Restart Manager 定位占用进程，支持一键自动结束；结束失败会提示手动结束或重启电脑 |
| 自动运行 | 清单里 `auto_run=true` 的文件在更新完成后自动启动，并带上命令行附加参数 |
| 界面 | 表格化文件列表（文件名/目录/大小/进度/速度/状态/说明）、总体进度条、实时速度、剩余时间、耗时统计 |
| 不暴露接口 | 日志只记文件名、大小、错误码，不记录更新接口地址（避免泄露 `software_key`） |
| 单文件部署 | DLL 内已压缩内嵌 `updata.exe`，接入时只需要分发 DLL |

## 目录结构

```
src/
  main.c         GUI（Win32 自绘界面、文件列表、进度统计）
  update.c       更新流程（占用检测交互、多线程下载池、校验替换、自动启动）
  manifest.c     清单拉取与差异比对（exe 与 DLL 共用，保证判断一致）
  http.c         WinHTTP 封装（HTTPS、重定向、进度回调、可中断）
  json.c         轻量 JSON 解析
  sha256.c       SHA-256
  util.c         路径/URL/编码/日志等工具
  lockcheck.c    文件占用检测与进程结束（Restart Manager）
  dll.c          更新检测 DLL（导出 updata / updata_w）
  dll.rc         内嵌压缩后的 updata.exe
  app.rc         程序清单（comctl32 v6 + DPI 感知）与版本信息
tools/
  pack.c         构建工具：把 updata.exe 压缩成内嵌 payload（LZ，自写实现）
build.bat        构建 updata.exe
build_dll.bat    构建 updata32.dll / updata64.dll（自动压缩内嵌 exe）
```

## 构建

依赖：

- **MinGW-W64**（`gcc` + `windres`）—— 构建 `updata.exe`、32 位 DLL、`pack.exe`
- **MSVC 2022 + Windows SDK** —— 仅在没有 64 位 MinGW 时用于构建 `updata64.dll`
  （若安装了 `x86_64-w64-mingw32-gcc`，脚本会自动优先使用它）

```bat
build.bat        :: 生成 updata.exe
build_dll.bat    :: 生成 updata32.dll / updata64.dll（会先编 exe，再压缩内嵌）
```

产物在项目根目录。

## updata.exe 用法

```bat
updata.exe <更新检测地址> [附加命令行]
updata.exe "https://your-server.example/api.php?action=client.files&software_key=xxxx" "--login user1"
```

- 第 1 个参数：更新检测接口地址
- 第 2 个参数及之后：更新完成后启动 `auto_run` 文件时追加的命令行（含空格的参数会自动加引号）
- **被更新的目录 = `updata.exe` 所在目录**
- 不传参数时静默退出（返回码 2）

环境变量：

| 变量 | 作用 |
| --- | --- |
| `UPDATA_TARGET_DIR` | 指定更新目标目录（DLL 把 exe 释放到临时目录时，用它把目标指回宿主程序目录） |
| `UPDATA_NO_AUTORUN=1` | 只更新、不自动启动（调试/静默部署用） |

运行日志写在更新目标目录下的 `updata.log`（UTF-8，不含接口地址）。

## 服务端接口约定

`GET` 请求返回 JSON：

```json
{
  "ok": true,
  "data": {
    "id": "S0001",
    "name": "软件名称",
    "version": "1.0.1",
    "description": "软件描述",
    "changelog": "更新日志",
    "updated_at": "2026-01-01T00:00:00+00:00",
    "files": [
      {
        "path": "db",
        "name": "CURRENT",
        "size": 16,
        "hash": "0f1bad70c7bd1e0a69562853ec529355462fcd0423263a3d39d6d0d70b780443",
        "url": "/download.php?t=xxxx",
        "active": true,
        "auto_run": false,
        "force_overwrite": true,
        "note": ""
      }
    ]
  },
  "msg": ""
}
```

| 字段 | 说明 |
| --- | --- |
| `ok` | `false` 表示服务端返回失败，此时用 `msg` 作为错误提示；也支持 `data.error` |
| `path` / `name` | 相对路径与文件名，拼接后相对**更新目标目录**；`path` 为空表示根目录 |
| `size` | 文件字节数（用于进度与完整性校验） |
| `hash` | 服务端文件的 SHA-256（小写十六进制） |
| `url` | 下载地址，可为绝对地址，或以 `/` 开头的站点相对地址，或以接口地址为基准的相对地址 |
| `active` | `false` 表示该文件不下发（跳过） |
| `auto_run` | `true` 表示更新完成后自动启动该文件（可多个） |
| `force_overwrite` | `true`：本地文件 hash 不同就强制覆盖；`false`：本地已存在该文件就不更新 |

### 更新判断规则

| 本地情况 | 结果 |
| --- | --- |
| `active = false` | 跳过（服务端已禁用） |
| 本地不存在 | 需要更新 |
| 本地 hash 与 `hash` 一致 | 已是最新，不下载 |
| hash 不一致 且 `force_overwrite = true` | 需要更新（覆盖） |
| hash 不一致 且 `force_overwrite = false` | 跳过（文件已存在即可） |

## DLL 接入（易语言）

### 1. 声明

```
.版本 2

.DLL命令 检测更新, 整数型, "updata32.dll", "updata", 公开, 返回0=无需更新,1=已启动更新并结束本进程,负值=检测失败
    .参数 更新地址, 文本型
    .参数 附加命令, 文本型
```

> `updata` 接收 ANSI/GBK 文本（易语言默认即为此）；若宿主是 Unicode/宽字符环境，请改用导出函数 `updata_w`。

### 2. 调用

```
.子程序 __启动窗口_创建完毕

.局部变量 ret, 整数型

ret ＝ 检测更新 (“https://your-server.example/api.php?action=client.files&software_key=xxxx”, “--login user”)
' ret = 0 ：无需更新，继续正常启动
' ret = 1 ：不会执行到这里 —— 更新程序已启动，本进程已被结束
' ret < 0 ：检测失败（网络/服务器/参数/释放失败），按“无需更新”处理，继续运行
```

### 3. 返回值

| 返回值 | 含义 |
| --- | --- |
| `0` | 已是最新，未做任何操作 |
| `1` | 需要更新：已释放并启动 `updata.exe`，随后结束当前进程 |
| `-1` | 检测失败（网络不可达、服务端异常、超时） |
| `-2` | 参数为空 |
| `-3` | 更新程序释放或启动失败 |

### 4. 部署与执行流程

```
宿主程序目录/
  主程序.exe        ← 易语言程序
  updata32.dll      ← 只需分发这个（updata.exe 已压缩内嵌）
```

调用 `updata()` 时：

1. 从接口拉取清单，与宿主目录里的文件做 SHA-256 比对（与 `updata.exe` 共用同一份代码，判断不会不一致）；
2. 若无需更新 → 直接返回 `0`，**不释放任何文件、不做任何操作**；
3. 若需要更新 → 把内嵌的 `updata.exe` 解压释放到 `%TEMP%\updata_run\`，以
   `updata.exe "<更新地址>" <附加命令>` 启动，工作目录＝宿主目录，并通过环境变量
   `UPDATA_TARGET_DIR` 指定更新目标目录，随后结束宿主进程。

## 注意事项

- 32 位产物用 MinGW 编译，依赖系统自带的 UCRT（Windows 10 及以上自带；Windows 7 需已安装 KB2999226）。64 位 DLL 用 MSVC 静态 CRT，无运行库依赖。
- `updata.exe` 运行期可能出现 `*.updtmp` 临时文件，属正常现象（校验通过后会替换掉目标文件）。
- 更新 `updata.exe` 自身所在目录的文件时，正在运行的程序只需通过 DLL 结束自身进程即可解锁。

## 许可

本项目采用 [MIT](LICENSE) 许可。
