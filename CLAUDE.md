# 项目构建说明

## ESP-IDF 环境设置

本项目使用 ESP-IDF v6.1，需要在正确的环境中构建。

### 构建命令

在 PowerShell 中执行构建时，需要先激活 ESP-IDF 环境：

```powershell
. 'C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1'
idf.py build
```

### ESP-IDF 配置

- **IDF 路径**：`C:\esp\v6.1\esp-idf`
- **目标芯片**：ESP32-S3
- **烧录端口**：COM3（原生 USB）
- **烧录方式**：UART

### 快捷方式

可以使用桌面上的 ESP-IDF 终端快捷方式（`IDF_v6.1_Powershell.lnk`），它会自动加载所需的环境变量。
