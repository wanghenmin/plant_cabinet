# 植物养护系统后端

后端使用 FastAPI、SQLite 和原生 HTML/JavaScript 控制台。Python 依赖与运行均使用 `code/backend/.venv` 虚拟环境，配置通过 `.env` 环境变量提供。

## Windows / PowerShell 启动

首次运行：

```powershell
cd F:\辅导\物联网植物养护\code\backend
py -3 -m venv .venv
.\.venv\Scripts\Activate.ps1
python -m pip install -r requirements.txt
Copy-Item .env.example .env
```

编辑 `.env`，至少将 `DEVICE_TOKEN` 替换为随机长字符串。启动服务：

```powershell
cd F:\辅导\物联网植物养护\code\backend
.\.venv\Scripts\Activate.ps1
python -m uvicorn app.main:app --host 0.0.0.0 --port 8000
```

电脑本机打开 `http://127.0.0.1:8000`，API 文档在 `/docs`。SQLite 数据库默认保存在 `code/backend/data/`，不会纳入版本控制。`DATABASE_PATH`、`DEVICE_ID`、`DEVICE_TOKEN` 和 `DEVICE_ONLINE_TIMEOUT_SECONDS` 均可在 `.env` 中设置。

## ESP32 联网配置

1. 电脑与 ESP32 连接同一局域网；在 Windows 运行 `ipconfig`，找到当前 Wi-Fi/以太网适配器的 IPv4 地址。
2. 在 Sketch 的 `plant_cabinet.ino` 填入 Wi-Fi 名称和密码、`BACKEND_URL`（例如 `http://192.168.1.23:8000`）、`DEVICE_ID` 与 `DEVICE_TOKEN`。
3. `DEVICE_ID` 必须与 `.env` 一致，`DEVICE_TOKEN` 必须与 `.env` 完全一致。`127.0.0.1` 是 ESP32 自身而不是电脑，不能填作设备端后端地址。
4. 先启动后端，再由用户自行编译、烧录 Sketch。网页显示设备在线后再发送控制命令。

若 `.env` 设置了 `DEVICE_TOKEN`，设备 API 必须在 `X-Device-Token` 请求头中提供相同令牌。网页控制台当前面向可信实验室局域网使用，不要将服务直接暴露到公网。

## 功能与 API

- 网页展示设备在线状态、传感器实时值、历史曲线、采样分析、操作事件，并支持 CSV 导出。
- 灯光支持手动开/关与恢复定时；水泵支持短时测试。每项网页手动控制均先弹出确认框，设备离线时不接受新命令。
- 网页可调整灯光时段与 PWM、水分阈值、浇水脉冲/间隔、每日水泵运行上限；自动浇水默认关闭。
- `POST /api/sensors`：设备传感器上报。
- `GET /api/status`、`GET /api/readings`、`GET /api/readings.csv`、`GET/PUT /api/config`：状态、历史、导出与控制配置。
- `POST /api/commands`：创建控制命令；`GET /api/device/poll`：设备心跳、获取配置和命令；`POST /api/device/commands/{id}/result`：设备回执。
- `GET /api/events`：控制与配置事件。

## 硬件测试注意事项

网页水泵测试最长 3 秒，固件还设置了单次 3 秒与 24 小时累计 60 秒的硬限制。测试前确认水泵已浸水、出水管固定且供电安全；灯带和水泵使用外部 5V 电源，并按已验证的接线方式与 ESP32 共地。首次测试时保持自动浇水关闭。软件限制不能替代现场看护或硬件断电保护。
