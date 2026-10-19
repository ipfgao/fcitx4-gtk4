# 编译与安装说明

fcitx4-gtk4 是 GTK4 输入法模块（IM Module），编译产物为 `libim-fcitx4.so`，需安装到 GTK4 的 IM 模块目录才能被加载。

## 1. 依赖

| 依赖 | 版本 | pkg-config 名 | 说明 |
|---|---|---|---|
| CMake | ≥ 3.10 | - | 构建系统 |
| GCC/Clang | C99 | - | 编译器 |
| GTK4 | ≥ 4.2 | `gtk4` | 目标 UI 框架 |
| fcitx | 4.x | `fcitx` | fcitx 主库 |
| fcitx-gclient | 4.x | `fcitx-gclient` | fcitx DBus 客户端封装 |
| xkbcommon | - | `xkbcommon` | xkb compose 表 |
| X11 | - | x11 | X11 事件转发 |

Debian/Ubuntu 一键安装构建依赖：

```bash
sudo apt install \
  cmake gcc \
  libgtk-4-dev \
  fcitx libfcitx-utils-dev libfcitx-gclient-dev \
  libxkbcommon-dev libx11-dev
```

> 若发行版发行包名不同，请用 `pkg-config --exists` 逐项检查：
> `pkg-config --exists gtk4 fcitx fcitx-gclient xkbcommon && echo OK`

## 2. 构建

```bash
cd fcitx4-gtk4
mkdir build && cd build
cmake -DCMAKE_INSTALL_PREFIX=/usr ..
make
```

产物在 `build/immodules/libim-fcitx4.so`。

## 3. 安装到 GTK 模块目录（关键步骤）

GTK4 只从编译时硬编码的 IM 模块目录加载模块，搜索路径为：

    <libdir>/gtk-4.0/4.0.0/immodules

其中 `<libdir>` 是 GTK4 库所在目录，可用以下命令查得：

```bash
pkg-config --variable=libdir gtk4
```

典型值：x86_64 Debian/Ubuntu 为 `/usr/lib/x86_64-linux-gnu`。

因此必须把 prefix 设为与 `<libdir>` 匹配的路径再安装：

```bash
sudo cmake --install build \
  --prefix "$(pkg-config --variable=libdir gtk4 | sed 's|/lib/x86_64-linux-gnu||' | sed 's|/lib/aarch64-linux-gnu||')"
```

简化写法（x86_64 Debian/Ubuntu 直接）：

```bash
sudo cmake --install build --prefix /usr
```

或手动拷贝：

```bash
sudo cp build/immodules/libim-fcitx4.so \
  "$(pkg-config --variable=libdir gtk4)/gtk-4.0/4.0.0/immodules/"
sudo chmod 755 "$(pkg-config --variable=libdir gtk4)/gtk-4.0/4.0.0/immodules/libim-fcitx4.so"
```

> ⚠️ 常见坑：默认 prefix 是 `/usr/local`，装到 `/usr/local/lib/gtk-4.0` 下 GTK4 **不会加载**（不在搜索路径）。

## 4. 验证安装

```bash
# 确认 .so 存在且可读
ls -la "$(pkg-config --variable=libdir gtk4)/gtk-4.0/4.0.0/immodules/libim-fcitx4.so"

# 用 GTK_DEBUG=immodule 观察模块加载（应出现 fcitx4 相关行）
GTK_DEBUG=immodule GTK_IM_MODULE=fcitx4 gedit    # 或任意 GTK4 应用
```

## 5. 使用

gtk4 应用通过环境变量启用：

```bash
export GTK_IM_MODULE=fcitx4
export XMODIFIERS=@im=fcitx
```

然后重启 GTK4 应用。fcitx 4 输入法后端需要已安装并运行：

```bash
fcitx -d          # 启动 fcitx daemon
fcitx-diagnose    # 检查运行状态
```

## 6. 卸载

```bash
sudo rm "$(pkg-config --variable=libdir gtk4)/gtk-4.0/4.0.0/immodules/libim-fcitx4.so"
```

## 7. 常见问题

- **英文无法直接输入 / 候选词框才出字**：确认安装的是**修复后**的版本（检查 .so 字节数/时间戳），并重启应用使模块重新加载。
- **模块装了但没生效**：`GTK_DEBUG=immodule` 查看是否加载；检查是否装到了 GTK 真实搜索目录（见第 3 节）。
- **GTK 应用是 Wayland 后端**：模块同样适用，但请确认 `GDK_BACKEND` 与 fcitx 的 XMODIFIERS 配置一致。
