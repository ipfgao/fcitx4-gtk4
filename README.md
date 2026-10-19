# fcitx4-gtk4

fcitx 4 GTK4 输入法模块（IM Module），基于 fcitx-4.2.9.8 的 GTK2/GTK3 前端移植，并修复了移植过程中引入的若干 bug（详见下文）。

支持 fcitx4 在 GTK4 应用中正常输入：拼音/五笔等引擎的候选词、preedit 高亮、surrounding text（以词查字）、xkb compose 组合键等。

## 特性

- GTK4 (≥4.6) 原生 IM 模块，通过 `GTK_IM_MODULE=fcitx` 启用
- 拼音/五笔等 fcitx 引擎的候选词与 preedit 显示
- surrounding text 支持（fcitx 拼音联想、以词查字）
- xkb compose 组合键（dead key，如 `^`+`e` → ê）
- 英文直接输入回退：fcitx 未处理的键由 GtkIMContextSimple 处理
- 同步/异步按键处理双模式（默认可通过源码开关切换）

## 编译依赖

- CMake ≥ 3.10
- GTK4 开发库（`gtk4`，≥ 4.2）
- fcitx 4 开发库（`fcitx`、`fcitx-gclient`）
- xkbcommon
- X11

Debian/Ubuntu:

```bash
sudo apt install cmake gcc libgtk-4-dev fcitx libfcitx-utils-dev libfcitx-gclient-dev libxkbcommon-dev libx11-dev
```

## 编译与安装

```bash
mkdir build && cd build
cmake -DCMAKE_INSTALL_PREFIX=/usr ..
make
sudo make install
```

> **注意**：GTK4 只从编译时硬编码的 `libdir/gtk-4.0/4.0.0/immodules` 加载 IM 模块。
> 本机 GTK4 的 libdir 为 `/usr/lib/x86_64-linux-gnu`，因此**必须用 `-DCMAKE_INSTALL_PREFIX=/usr`**
> （否则模块装到 `/usr/local` 下 GTK 不会加载）。
> 若你的发行版 GTK4 libdir 不同，请以 `pkg-config --variable=libdir gtk4` 为准。

## 使用

```bash
export GTK_IM_MODULE=fcitx
export QT_IM_MODULE=fcitx       # 非必填，与 GTK 无关
export XMODIFIERS=@im=fcitx
```

重启 GTK4 应用（模块只在启动时加载）。用 `GTK_DEBUG=immodule` 可观察模块加载是否成功：

```bash
GTK_DEBUG=immodule GTK_IM_MODULE=fcitx your-gtk4-app
```

## 修复记录（GTK3 → GTK4 移植引入的问题）

- **异步按键重注入死循环**：GTK3 用 `event->state` 打 IgnoredMask 防重入，GTK4 事件不可变，改为 `reinjected_events` 哈希表防重入
- **异步回调 use-after-free**：回调改为持有 `g_object_ref`
- **英文直接输入失效**：slave(Simple) 的 commit 桥接被错误丢弃，已修复
- **forward-key 掏空**：fcitx 转发的键通过 `forward_pending` 队列交给 slave 处理
- **preedit-start 信号缺失**：补全 start/changed/end 状态机
- **surrounding text 弱指针指向栈变量**：改为结构体成员
- **compose 死代码**：接入 xkb_compose_state 到 fallback 路径
- 清理大量死代码与调试残留

## 目录结构

```
├── CMakeLists.txt
├── config.h.in
├── src/
│   ├── fcitxim.c          # IM 模块入口
│   ├── fcitximcontext.c   # GTK4 IM context 实现
│   └── fcitximcontext.h
└── README.md
```

## 许可证

GPL-2.0-or-later。

基于 fcitx-4.2.9.8 的 gtk2/gtk3 前端移植，保留原作者版权：

- Copyright (C) 2010~2012 CSSlayer <wengxt@gmail.com>
- Copyright (C) 2026 ipfgao <ipfgao@hotmail.com>
