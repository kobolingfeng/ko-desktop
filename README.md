# ko-desktop — 动态桌面壁纸

超轻量动态桌面壁纸应用，类似 Wallpaper Engine。基于 [QiangQiang](https://github.com/aspect-build/aspect) 框架（C++ Win32 + WebView2 + Bun + TypeScript）。

> 单 exe，播放 MP4/WebM 视频替代桌面壁纸。

## 原理

- C++ Win32 创建 WS_POPUP 窗口，设置 Progman 为 owner，使其位于桌面之上、普通窗口之下
- WebView2 渲染 HTML5 `<video>` 标签循环播放视频
- 桌面图标背景设为透明，壁纸窗口透过图标层可见
- 系统托盘图标控制（选择视频、暂停/播放、静音、退出）
- 自动记忆上次播放的视频
- 退出时自动恢复桌面图标背景

## 使用

1. 运行 `app.exe`
2. 右键托盘图标 → **选择视频** → 选一个 `.mp4` 文件
3. 视频即刻作为桌面壁纸播放
4. 右键托盘图标可暂停/播放/静音/退出

## 开发

### 前置要求

- [Bun](https://bun.sh)
- [Visual Studio Build Tools 2022](https://visualstudio.microsoft.com/downloads/#build-tools-for-visual-studio-2022)（勾选"使用 C++ 的桌面开发"）

### 安装 & 构建

```bash
bun install
bun run setup    # 下载 WebView2 SDK + JSON 库（如 deps/ 已存在则跳过）
bun run build    # 编译前端 + 原生壳 → dist/
```

### 开发模式

```bash
bun run dev      # 热重载开发（F12 打开 DevTools）
```

### 单 exe 打包

```bash
bun run build:single   # 所有资源嵌入单个 exe
```

## 项目结构

```
├── native/
│   ├── main.cpp        # C++ 壳（壁纸嵌入 + WebView2）
│   └── resource.h      # 资源 ID
├── src/
│   ├── ipc.ts          # IPC 通信桥
│   ├── api.ts          # TypeScript API 封装
│   ├── main.ts         # 壁纸控制逻辑
│   └── index.html      # 视频播放器页面
├── scripts/
│   ├── setup.ts        # 下载依赖
│   ├── build.ts        # 编译
│   └── dev.ts          # 开发服务器
├── app.config.json     # 应用配置
└── package.json
```

## License

MIT
