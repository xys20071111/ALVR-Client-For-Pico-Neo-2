# PicoStreamer — ALVR Client for Pico Neo 2

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

### NDK 路径配置

NDK 路径通过环境变量 `ANDROID_NDK_HOME` 传入。构建前确保已设置：

```bash
export ANDROID_NDK_HOME=/path/to/your/ndk
```

如果未设置，cargo 会使用自身的 NDK 发现机制。

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

### 6DoF 位置追踪可能未生效

- **症状**：晃动头显时 SteamVR 界面跟随移动，而非固定在空间中
- **原因**：`PvrClient.setTrackingMode(1)` 虽然被调用，但 `HmdState.getPos()` 可能仍返回零值。`PvrClient.getTrackingData()` 是空方法（返回 null），`getTrackingDataExt()` 通过 `PvrServiceManager.mManager` 查询真实数据但 mManager 可能为 null（SteamVR 重启后会导致 NPE 崩溃）
- **当前状态**：已移除 `getTrackingDataExt()` 调用避免崩溃，依赖 `setTrackingMode(1)` + `HmdState.getPos()`，但效果未确认

### 手柄可能在 SteamVR 重启后消失

- **原因**：PicoVR SDK 的 `PvrServiceManager.mManager` 在 PVR 服务断开后变为 null，导致后续追踪数据查询 NPE
- **当前状态**：已移除会触发 NPE 的代码路径，但 SteamVR 重启后手柄可能需要重新配对

### FOV 需要手动校准

- 当前使用 55° 半角（总 110°）作为默认值，但最适宜的 FOV 值需要根据实际佩戴体验调整
- 值不匹配时表现为画面"太近"（FOV 偏小）或"太远"（FOV 偏大）

### 编码分辨率与眼缓冲区

- ALVR 编码分辨率 1856×1856（正方形）
- PicoVR SDK 眼缓冲区 1664×1664（正方形）
- 两者宽高比均为 1:1，匹配正确，但分辨率差异由 GPU 线性过滤处理

### NDK 路径配置

NDK 路径通过环境变量 `ANDROID_NDK_HOME` 自动读取，确保构建前已设置。
