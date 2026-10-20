# 嘉立创 ESP32-S3 开发板天气站

基于嘉立创 ESP32-S3「实战派」开发板与 ESP-IDF 6.1 的桌面天气站固件。屏幕采用 320×240 ST7789 横屏，天气数据由和风天气提供，包含实时天气、空气质量、恶劣天气预警、逐小时变化曲线和 7 天预报。

> **凭据说明：**公开仓库不包含和风天气 API Key、专属 API Host 或 WiFi 密码。请自行申请和风天气凭据并创建本地 `main/app_secrets.h`。项目使用的 API Key 已从公开源码配置中移除；仓库中的配置模板不包含可用密钥。API 服务账号、调用额度和服务条款由使用者自行负责。

## 功能

- 今日天气：实时温度、体感、湿度、风向风速、气压、AQI、空气质量描述、逐小时趋势曲线。
- 预报页面：3 天、5 天和 7 天预报；左右滑动切换天气页面。
- 城市管理：最多保存 10 个城市，按省份和城市选择；首屏上下滑动循环切换城市。
- 天气预警：有恶劣天气预警时显示预警内容，无预警时隐藏提醒区域。
- 设置：城市、屏幕亮度、刷新间隔、温度单位、自动息屏和 WiFi 设置；设置通过 NVS 保存。
- 触摸与按键：FT5x06 触摸操作，BOOT 按键可翻页或唤醒屏幕。
- 显示与节能：ST7789 图形界面、矢量天气图标、亮度调节和自动息屏。
- 网络时间：联网后通过 SNTP 校时。

## 硬件

| 模块 | 接线/规格 |
|---|---|
| 主控 | 嘉立创 ESP32-S3「实战派」开发板 |
| 内存 | 8 MB Octal PSRAM，80 MHz |
| LCD | ST7789，320×240，横屏 |
| LCD SPI | SPI3，MOSI GPIO40，CLK GPIO41，DC GPIO39 |
| LCD 背光 | GPIO42，LEDC；低电平点亮 |
| LCD CS | PCA9557 IO0 控制 |
| 触摸 | FT5x06，I²C 地址 `0x38` |
| I²C | PCA9557 地址 `0x19`；SDA GPIO1，SCL GPIO2；触摸与 LCD 控制共用总线 |
| BOOT | GPIO0，低电平按下 |

硬件引脚依据该开发板和当前实测连接配置。其他 ESP32-S3 板卡使用前，请先核对实际原理图并修改驱动引脚。

## 开始使用

### 1. 安装环境并克隆

安装 ESP-IDF **v6.1** 及其 ESP32-S3 工具链。然后在终端执行：

```bash
git clone https://github.com/zhangxucn/jlc-esp32-s3-weather-station.git
cd jlc-esp32-s3-weather-station
```

在已配置 ESP-IDF 的终端中继续以下步骤。若你的 ESP-IDF 未自动进入环境，请按 Espressif 对应系统的安装说明先加载 `export.sh`。

### 2. 创建本地凭据文件

```bash
cp main/app_secrets.example.h main/app_secrets.h
```

编辑 `main/app_secrets.h`，填写自己的 WiFi SSID/密码和和风天气配置：

- `APP_QWEATHER_KEY`：在[和风天气控制台](https://console.qweather.com/)申请的 API Key。
- `APP_QWEATHER_HOST`：在控制台获取的项目专属 API Host。
- `APP_WIFI_SSID`、`APP_WIFI_PASSWORD`：首次启动用于联网的 WiFi。
- `APP_WIFI_SSID2`、`APP_WIFI_PASSWORD2`：可选备用网络；不使用时留空。

首次连接 WiFi 后，也可在设备设置页里修改 WiFi。`app_secrets.h` 已被 Git 忽略，不要移除忽略规则，也不要将凭据粘贴到公开 issue、截图或日志中。丢失凭据文件时可重新从模板复制。

### 3. 构建

```bash
idf.py set-target esp32s3
idf.py build
```

成功后固件位于 `build/esp32-weather.bin`。项目配置在 `sdkconfig.defaults`，自定义分区表在 `partitions.csv`。

### 4. 首次烧录

将开发板连接电脑，确认串口设备名，然后将下面的 `/dev/ttyUSB0` 替换成实际端口。该命令写入 bootloader、分区表和应用；应用地址必须为 `0x10000`：

```bash
python -m esptool --chip esp32s3 --port /dev/ttyUSB0 -b 460800 \
  --before default-reset --after hard-reset write-flash \
  --flash-mode dio --flash-size 8MB --flash-freq 80m \
  0x0 build/bootloader/bootloader.bin \
  0x8000 build/partition_table/partition-table.bin \
  0x10000 build/esp32-weather.bin
```

仅更新应用时，可只写应用分区：

```bash
python -m esptool --chip esp32s3 --port /dev/ttyUSB0 -b 460800 \
  --before default-reset --after hard-reset write-flash \
  --flash-mode dio --flash-size 8MB --flash-freq 80m \
  0x10000 build/esp32-weather.bin
```

确认烧录器报告的写入字节数等于 `build/esp32-weather.bin` 文件大小，并看到数据校验成功。不要把 `build/` 下的固件、日志或临时文件提交到仓库。

## 操作说明

- 首屏上下滑动：在已保存城市间循环切换；目标城市天气由后台获取后更新首屏。
- 首屏左右滑动：切换今日、3 天、5 天、7 天和设置页面。
- 长按首屏右上角地区栏：打开城市管理，可添加、选择或删除城市。
- 设置页：触摸选项进行调整；WiFi 设置项可扫描、输入和保存网络。
- 屏幕闲置达到设置时间后自动息屏；触摸或 BOOT 键唤醒。

## 项目结构

```text
main/                   主程序、触摸/LCD 驱动、页面、设置与地区/字库数据
components/weather/     和风天气 API 组件
managed_components/     ESP-IDF Component Manager 下载的依赖（不提交）
tools/                  字库与地区数据生成脚本
partitions.csv          自定义 Flash 分区表
sdkconfig.defaults      ESP-IDF 默认配置
```

依赖版本由 `dependencies.lock` 锁定；首次构建时 ESP-IDF Component Manager 会按清单下载依赖。`components/weather` 目录含其上游组件许可证文件。

## 免责声明与隐私

本项目是个人硬件与固件示例，按原样提供。和风天气数据的可用范围、准确性、配额及商用权限以服务商当前条款为准。设备会连接配置的 WiFi，并向和风天气服务请求天气数据；城市定位方式取决于所选城市和服务接口。请勿将个人 API Key、WiFi 密码或其他访问凭据提交到公开仓库。若凭据曾经被公开，应在对应服务控制台撤销并重新生成。
