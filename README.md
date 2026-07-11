# PicoStreamer — ALVR Client for Pico Neo 2

由GLM-5.2参考ALVR和PhoneVR的源代码编写的给Pico Neo 2使用的串流客户端

## 项目用途

PicoStreamer 是一个基于 [ALVR](https://github.com/alvr-org/ALVR) 的 VR 串流客户端，运行在 **Pico Neo 2** 头显上。它将 PC 上 SteamVR 渲染的 VR 画面通过网络传输到头显显示，同时将头显和手柄的追踪数据（6DoF 位置 + 旋转）回传给 PC 端的 SteamVR。

### 技术架构

- **ALVR client_core（Rust）**：负责网络通信、视频解码（H.264 硬解）、帧管理
- **PicoVR Native SDK**：负责头显渲染（VRActivity）、头部追踪（HmdState）、手柄输入（CVControllerManager）、6DoF 追踪（PvrClient）
- **C++ 桥接层**（`alvr_pico_main.cpp`）：连接 ALVR client_core 与 PicoVR SDK，处理 OpenGL ES 纹理渲染、追踪数据转发、手柄按钮映射

### 目标设备

| 参数 | 值 |
|---|---|
| 设备 | Pico Neo 2 |
| 系统 | Android 8.1 (API 27) |
| 屏幕 | 3840×2160 总分辨率，单眼 1920×2160 |
| 刷新率 | 72Hz |
| 对角 FOV | 101° |
| 架构 | arm64-v8a |

## 项目结构

```
PicoStreamer/
├── ALVR/                          # ALVR 上游源码（含 client_core Rust 库）
│   └── build/alvr_client_core/    # 构建产物：alvr_client_core.h + libalvr_client_core.so
├── picosdk/
│   └── PvrSDK-Native-release.aar  # PicoVR Native SDK
└── pico_alvr_client/              # Android 客户端项目
    ├── app/
    │   ├── build.gradle.kts       # 构建配置（含 ALVR 库构建任务）
    │   └── src/main/
    │       ├── AndroidManifest.xml
    │       ├── cpp/
    │       │   ├── CMakeLists.txt
    │       │   └── alvr_pico_main.cpp   # C++ 桥接层（核心代码）
    │       └── java/top/playtbsxys/picostreamer/
    │           └── PicoALVRActivity.java # Java Activity（SDK 桥接）
    └── gradlew
```

## 构建方法

### 前置要求

- **JDK 17**
- **Android SDK**（compileSdk 36）
- **Android NDK**（r25 或兼容版本）
- **Rust 工具链**（用于构建 ALVR client_core，含 `cargo`）
- **CMake 3.22.1+**

### 构建步骤

#### 1. 构建 ALVR client_core 库（Rust → .so）

```bash
cd ALVR
ANDROID_NDK_HOME=/path/to/ndk cargo run --package alvr_xtask -- build-client-lib --no-stdcpp
```

产物输出到 `ALVR/build/alvr_client_core/`：
- `alvr_client_core.h`（C 语言头文件）
- `arm64-v8a/libalvr_client_core.so`

#### 2. 构建 APK

```bash
cd pico_alvr_client
JAVA_HOME=/path/to/jdk-17 ./gradlew :app:assembleDebug --no-daemon
```

> `preBuild` 任务会自动触发 ALVR 库的 Rust 构建。如果只需要构建 APK（ALVR 库已存在），可使用 `assembleDebug`。

产物：`app/build/outputs/apk/debug/app-debug.apk`

#### 3. 安装到设备

```bash
adb install -r app/build/outputs/apk/debug/app-debug.apk
```

## 关键配置参数

所有核心参数在 `app/src/main/cpp/alvr_pico_main.cpp` 开头定义：

| 参数 | 位置 | 说明 |
|---|---|---|
| `FOV_H_HALF_DEG` | ~第 28 行 | 水平半角 FOV（度），当前 55° |
| `FOV_V_HALF_DEG` | ~第 29 行 | 垂直半角 FOV（度），当前 55° |
| `IPD_HALF` | 第 24 行 | 瞳距一半（米），当前 0.030015 |
| `STANDING_HEIGHT` | 第 32 行 | 站立高度偏移（米），当前 1.5 |
| `default_view_width/height` | `sendCaps()` 中 | 编码分辨率，当前 1920×1920 |

### FOV 调整

ALVR 的 `AlvrFov` 字段是**弧度制角度**（非正切值），服务端内部自行调用 `tan()` 构建投影矩阵。调整时直接修改度数值即可：

```cpp
#define FOV_V_HALF_DEG 55.0f   // 修改这个值
#define FOV_H_HALF_DEG 55.0f   // 修改这个值
```

- 值越大 → 视野越广 → 画面感觉越远
- 值越小 → 视野越窄 → 画面感觉越近
- 正方形眼缓冲区（1664×1664）下，水平垂直应设为相等

## 已知问题

- Home键会直接返回启动器
- 画面设置怎么调都很糊
- 导致SteamVR重启，重启后手柄消失
- FOV奇怪

