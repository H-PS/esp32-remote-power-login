/*
 * WiFi PIN 登录器 —— ESP32-S3 版（v5.4：巴法云 MQTT + SG90 舵机按压电源键；WOL 可用开关控制）
 *      - WOL 由 `ENABLE_WOL` 控制（本发布版设为 1）。仅在 ESP 与目标机处于同一广播域时才有意义
 * ==================================================================
 * 功能：
 *   1. 执行器物理按压笔记本电源键（远程开机，笔记本 WOL 不可靠时的正解）
 *      - SG90 舵机（凸轮装法），信号线接 GPIO14（左排针底部，与 5Vin/GND 相邻）
 *   2. 等电脑开机后，以"USB 键盘"身份在 Windows 登录界面
 *      自动键入 PIN 并回车，完成登录
 *   3. 手机联动两条通道（可同时启用）：
 *      - 路线 A 局域网网页：手机连同一 WiFi，浏览器打开 ESP 的 IP 点按钮
 *      - 路线 B 巴法云 MQTT：在控制台「MQTT设备云」分页建 MQTT 类型主题
 *        power003，微信小程序/控制台发 on / off / login → ESP 执行
 *      ⚠️ 巴法云的 TCP设备云 与 MQTT设备云 是隔离空间：
 *         MQTT 协议(9501) 必须配 MQTT 类型的主题；
 *         TCP 类型的主题只认 TCP 协议(8344) —— 协议与主题类型必须配对！
 *
 * 语言/软件：Arduino C/C++，Arduino IDE（或 VSCode + PlatformIO）
 * 库：ESP32Servo（舵机方案）、PubSubClient（MQTT）
 *
 * ⚠️ Arduino IDE 关键设置（缺一不可）：
 *   开发板:  ESP32S3 Dev Module
 *   USB Mode: USB-OTG (TinyUSB)   ← 不选这个就不会被识别为键盘！
 *   USB CDC On Boot: Enabled      ← 日志从 OTG 口虚拟串口出
 *   Flash Size: 16MB / PSRAM: OPI PSRAM
 *   烧录时数据线插 "CH343P/USB转串口" 口；烧完把线换插 "OTG/直通" 口
 */

/* ---- WOL 网络唤醒开关（必须在 #include 之前定义）----
 * 1 = 启用：广播魔术包唤醒睡眠中的电脑。
 *           ⚠️ 前提是「ESP 与目标机处于同一广播域」。跨网段时必然无效 ——
 *           受限广播 255.255.255.255 不会被三层设备转发。典型反例：
 *           校园网的无线段与宿舍有线段分属不同 VLAN，魔法包到不了。
 *           想稳定使用，让 ESP 与目标机挂在同一台路由器下面。
 * 0 = 关闭：不需要远程唤醒（例如用「锁屏代替睡眠」），此时 sendWol() 不参与
 *           编译、login 不再空等 3 秒、WiFiUdp 不被引用，运行开销为零。
 */
#define ENABLE_WOL 1

#include <WiFi.h>
#include <WebServer.h>
#if ENABLE_WOL
#include <WiFiUdp.h>
#endif
#include <HTTPClient.h>
#include "USBHIDKeyboard.h"

/* ============ 0. 执行器：SG90 舵机 ============ */
#define ACTUATOR_SERVO 1
#define ACTUATOR ACTUATOR_SERVO  // 执行器：SG90 舵机

#define USE_CLOUD 1  // ← 1 = 启用巴法云 MQTT（9501）

#if ACTUATOR == ACTUATOR_SERVO
#include <ESP32Servo.h>
#endif
#if USE_CLOUD
#include <PubSubClient.h>
#endif

/* ================= 1. 必改配置 =================
 * 方式一（推荐，开源友好）：在同目录建 secrets.h（从 secrets.h.example 复制），
 *   把真实配置写在里面 —— secrets.h 已被 .gitignore 排除，不会被提交。
 * 方式二（简单）：直接在下面这几行改。
 */
#if __has_include("secrets.h")
  #include "secrets.h"
  const char* WIFI_SSID  = SECRET_WIFI_SSID;
  const char* WIFI_PASS  = SECRET_WIFI_PASS;
  const char* PIN_CODE   = SECRET_PIN_CODE;
  const char* PAGE_TOKEN = SECRET_PAGE_TOKEN;
#else
  const char* WIFI_SSID  = "YourWiFiName";     // ← 改成你的 WiFi 名
  const char* WIFI_PASS  = "YourPassword";     // ← 开放网络留空 ""
  const char* PIN_CODE   = "123456";           // ← 改成你的 Windows PIN
  const char* PAGE_TOKEN = "change_this";      // ← 改成你自己的网页口令
#endif

/* ---- 巴法云（USE_CLOUD=1 时必改）----
 * 主题必须在控制台「MQTT设备云」分页创建：
 *   - 类型选「消息型」（开关型只推 on/off，其他指令会被静默忽略！）
 *   - ⚠️ 主题"名称"和"昵称"要在【新建那一刻】就填成最终值，不要建完再改名！
 *     （昵称必须 == 主题名；事后改名容易只改一个 → 昵称≠主题名 → 出现
 *      "ESP 说订阅成功、后台显示订阅者离线、发消息没反应"的伪在线，极难查）
 *     建错了就删掉重建，比改名可靠。
 * 消息映射：
 *      on     = 开机并自动登录
 *      off    = 只按压一次电源键（只开机）
 *      login  = 唤醒 + 键入 PIN（完整流程，含 ESC/回车聚焦）
 *      login1 = 仅键入 PIN + 回车（锁屏 PIN 框已就绪时直接登录）
 *      test   = 打字自检：5 秒后把 PIN 打进记事本（排查"多打了字符"）
 * 发送端：微信小程序"巴法云"（开关按钮）/ 网页控制台发送栏
 */
#if __has_include("secrets.h")
const char* BEMFA_UID   = SECRET_BEMFA_UID;
const char* BEMFA_TOPIC = SECRET_BEMFA_TOPIC;
#else
const char* BEMFA_UID   = "your_private_key";  // ← 巴法云私钥
const char* BEMFA_TOPIC = "your_topic";        // ← MQTT设备云 里建的消息型主题
#endif

#if ENABLE_WOL
/* ---- WOL 目标配置（ENABLE_WOL=1 时才需要填）----
 * PC_MAC：目标机网卡的 MAC（去冒号/横杠、小写、正好 12 位）
 *   ⚠️ 有线网卡和无线网卡是【两个不同的 MAC】。填错 = 魔法包发给了另一张卡，
 *      必然失败，而且不会有任何报错 —— 这是 WOL 最常见的坑。
 *      ipconfig /all 里分别看：「以太网适配器」=有线 /「无线局域网适配器」=无线
 * PC_MAC2：第二张网卡的 MAC（留空 "" 则跳过）
 * PC_IP  ：可选。填了会额外发「定向单播」包 —— 部分交换机/AP 抑制广播但放行
 *      单播，这时只有定向包能到。留空则只发广播。
 */
#if __has_include("secrets.h")
const char* PC_MAC  = SECRET_PC_MAC;
const char* PC_MAC2 = SECRET_PC_MAC2;
const char* PC_IP   = SECRET_PC_IP;
#else
const char* PC_MAC  = "aabbccddeeff";  // ← 主目标网卡 MAC（有线优先）
const char* PC_MAC2 = "";              // ← 备用网卡 MAC，留空则不发
const char* PC_IP   = "";              // ← 可选：目标机 IP（额外发定向单播）
#endif
#endif

/* ================= 2. 舵机微调 ================= */
const int ACT_PIN = 14;  // 舵机信号线（橙线）——左排针底部，与 5Vin/GND 相邻
/* ⚠️ 左排针底部三针自上而下：14 → 5Vin → GND，正好对上舵机 橙→红→棕。
 *    插错一位 = 信号脚吃到 5V，或电源/地反接 → 烧舵机。插前逐根核对丝印！
 *    想换回板子顶部的 GPIO4，把上面那行改成 4 即可（其余代码无需改动）。
 */
const uint32_t PRESS_HOLD_MS = 600;  // 按住时长，电源键短按即可

const int SERVO_IDLE_DEG = 10;   // 待机角度（离开电源键）
const int SERVO_PRESS_DEG = 95;  // 按压角度——装好后实测微调

/* ================= 3. 流程微调 ================= */
const uint32_t BOOT_WAIT_MS = 40000;  // 按下电源键后等多久开始输 PIN
const uint32_t WAKE_WAIT_MS = 2500;   // 按唤醒键后等多久屏幕亮起
const uint32_t FOCUS_WAIT_MS = 800;   // 回车聚焦 PIN 框后的等待
/* ---- 按键时序（修复"PIN 多出 00"用）----
 * KEY_HOLD_MS   : 按下后保持多久才抬起——太短主机可能采不到按下态
 * KEY_SETTLE_MS : 抬起到下一个字符之间留多久——太短字符会被合并/触发重复
 * 已实测通过（login / login1 均正常）。节奏放慢还能降低搜索框联想补全
 * 与 auto-repeat 的概率，所以取 60ms 这个更保守的值。
 * 若觉得输入太慢，可逐级下调：60 → 50 → 45。
 * （旧版用单一 TYPE_DELAY_MS，已被这两个参数取代）
 */
const uint8_t KEY_HOLD_MS = 30;       // 单键按住时长
const uint8_t KEY_SETTLE_MS = 60;     // 字符间隔（保守值，降低补全/重复概率）
/* ================================================ */

USBHIDKeyboard Keyboard;
WebServer server(80);

/* ---- login 是否发送前置键（ESC + 回车）----
 * 1 = 发（默认）：锁屏/黑屏场景，ESC 点亮屏幕、回车聚焦 PIN 框
 * 0 = 不发：只打 PIN + 回车。适合"电脑已解锁、怕 ESC/回车误操作"的场景；
 *     这种场景更推荐直接用 login1（等价于此开关设 0）
 */
#define LOGIN_SEND_PREAMBLE 1

Servo powerServo;
#if USE_CLOUD
WiFiClient mqttTcp;
PubSubClient mqtt(mqttTcp);
uint32_t mqttRetryAt = 0;  // MQTT 断线重连计时
uint32_t lastWifiCheck = 0;  // WiFi 掉线检查计时（校园网会定时踢人）
#endif
#if ENABLE_WOL
WiFiUDP wolUdp;  // WOL 魔术包广播用（纯网络，与 USB 栈无关）
#endif

/* ---------- WOL 网络唤醒：魔术包唤醒睡眠中的笔记本 ----------
 * 纯 UDP，不依赖任何 USB 协议；目标网卡设「只允许幻数据包唤醒」即可。
 * 一次调用 = 多目的地 × 多端口 × 多轮，尽量提高命中率：
 *   ① 全局广播 255.255.255.255   ② 本子网广播（由 ESP 的 IP+掩码算出）
 *   ③ 定向单播（只有填了 PC_IP 才发）
 * 并打印 ESP 自己的 IP / 掩码 / 广播地址——若与笔记本不在同一网段，一眼可见。
 */
#if ENABLE_WOL
void sendWol() {
  /* ---- 目的地清单先算好：全局广播 + 本子网广播 +（可选）定向单播 ---- */
  IPAddress dst[3];
  int nDst = 0;
  dst[nDst++] = IPAddress(255, 255, 255, 255);
  IPAddress myIp = WiFi.localIP(), myMask = WiFi.subnetMask();
  IPAddress subnetBc;
  for (int i = 0; i < 4; i++)
    subnetBc[i] = (myIp[i] & myMask[i]) | (uint8_t)(~myMask[i]);
  dst[nDst++] = subnetBc;
  IPAddress pcIp;
  if (strlen(PC_IP) > 0 && pcIp.fromString(PC_IP)) dst[nDst++] = pcIp;
  const uint16_t ports[] = { 9, 7 };

  /* ---- 对「有线 + 无线」两张网卡各发一轮 ---- */
  const char* macs[2] = { PC_MAC, PC_MAC2 };
  const char* tags[2] = { "有线", "无线" };
  for (int m = 0; m < 2; m++) {
    if (strlen(macs[m]) != 12) continue;   // 留空或长度不对 → 跳过这张卡
    uint8_t mac[6], pkt[102];
    for (int i = 0; i < 6; i++) {
      char b[3] = { macs[m][2 * i], macs[m][2 * i + 1], 0 };
      mac[i] = (uint8_t)strtol(b, nullptr, 16);


    
  }
  memset(pkt, 0xFF, 6);
  for (int i = 0; i < 16; i++) memcpy(pkt + 6 + i * 6, mac, 6);
  /* ---- 端口 9 / 7 是 WOL 标准端口；连发 3 轮，睡眠网卡容易漏包 ---- */
  for (int round = 0; round < 3; round++) {
    for (int d = 0; d < nDst; d++) {
      for (uint16_t p : ports) {
        wolUdp.beginPacket(dst[d], p);
        wolUdp.write(pkt, sizeof(pkt));
        wolUdp.endPacket();
      }
    }
    delay(120);
  }
  Serial.printf("[WOL] %s网卡 %s 已发（%d 个目的地 x 3 轮）\n",
                tags[m], macs[m], nDst);
  }   /* 结束「遍历两张网卡」的循环 */

  Serial.printf("[WOL] ESP 侧: IP=%s 掩码=%s 子网广播=%s\n",
                myIp.toString().c_str(), myMask.toString().c_str(),
                subnetBc.toString().c_str());
  if (nDst > 2) Serial.printf("[WOL] 另发定向单播 → %s\n", PC_IP);
  Serial.println("[WOL] 没醒？先比对上面 ESP 网段与笔记本是否一致");
}
#endif  // ENABLE_WOL

uint32_t loginAt = 0;    // 到点执行登录的时刻；0 = 无任务
bool bootLogin = false;  // 是否处于"开机后自动登录"流程中

/* ---------- 网页（路线 A） ---------- */
const char PAGE[] PROGMEM = R"HTML(
<!doctype html><html lang="zh"><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>WiFi 开机+PIN 登录器</title>
<style>
 body{font-family:sans-serif;max-width:420px;margin:32px auto;padding:0 16px;background:#f7f7f8;color:#222}
 h1{font-size:20px} input{width:100%;padding:10px;margin:8px 0;box-sizing:border-box}
 button{width:100%;padding:12px;margin:6px 0;border:0;border-radius:8px;font-size:16px;color:#fff}
 #b1{background:#185fa5} #b2{background:#0f6e56} #b3{background:#854f0b} #b4{background:#5f5e5a}
 #b5{background:#6b3fa0}
 #msg{margin-top:12px;color:#555;min-height:20px}
</style>
<h1>WiFi 开机 + PIN 登录器</h1>
<input id="t" placeholder="口令">
<button id="b1" onclick="go('servologin')">开机并自动登录（压电源键）</button>
<button id="b2" onclick="go('servopower')">只开机（压电源键）</button>
<button id="b3" onclick="go('login')">键入 PIN（完整流程）</button>
<button id="b4" onclick="go('login1')">仅键入 PIN（锁屏就绪）</button>
<button id="b5" onclick="go('test')">打字自检（先在电脑开记事本）</button>
<div id="msg"></div>
<script>
function go(a){
 const m=document.getElementById('msg');
 m.textContent='执行中...';
 fetch('/'+a+'?token='+encodeURIComponent(document.getElementById('t').value))
  .then(r=>r.text()).then(s=>m.textContent=s)
  .catch(()=>m.textContent='请求失败');
}
</script>
)HTML";

/* ---------- 执行器：按压电源键（统一入口） ---------- */
void pressPower() {
  powerServo.write(SERVO_PRESS_DEG);  // 压下去
  delay(PRESS_HOLD_MS);               // 按住
  powerServo.write(SERVO_IDLE_DEG);   // 松开回位
  delay(300);
}

#if USE_CLOUD
/* ---------- 校园网 MQTT 连通性诊断 ----------
 * 实测结论（2026-09-27）：校园网的深澜 Portal **只劫持 HTTP 80 端口**，
 * 巴法云 MQTT 用的 9501 端口不受影响 —— 所以 ESP 连上开放 WiFi
 * 后无需任何认证即可直接用 MQTT。
 *
 * 本函数只做诊断打印，不阻塞、不依赖 HTTP 结果。
 * 若哪天 MQTT 连不上，可看这里的 [Net] 日志判断是不是网络层问题。
 */
void diagnoseNetwork() {
  Serial.println("[Net] 诊断 MQTT 端口 bemfa.com:9501 ...");
  WiFiClient tcp;
  if (tcp.connect("bemfa.com", 9501, 8000)) {
    Serial.println("[Net] ✅ bemfa.com:9501 可达，MQTT 应能正常工作");
    tcp.stop();
  } else {
    Serial.println("[Net] ❌ bemfa.com:9501 不可达，检查 WiFi / 校园网策略");
  }
}
#endif

/* ---------- 键入一个字符串（健壮版，替代裸 Keyboard.write） ----------
 * 为什么不用裸的 Keyboard.write()：
 *   USBHIDKeyboard::write() 内部是「press(c); release(c);」两个 HID 报文
 *   紧挨着发出，中间没有间隔。若主机（Windows）USB 轮询没来得及取走中间态，
 *   可能出现「按下/抬起被合并成一次长按」→ 被识别为**自动重复**，
 *   表现出来就是某个字符被多打了几下（例如 PIN 末尾多出 "00"）。
 * 这里改成：先 releaseAll() 清干净残留键 → 每个字符单独 press → 短暂
 *   停留 → 单独 release → 再等 KEY_SETTLE_MS，确保每次按键都是
 *   「干干净净的一次 down+up」，主机一定看得清。
 */
void typeString(const char* s) {
  Keyboard.releaseAll();          // 清掉可能残留的按下状态（防"黏键"）
  delay(60);
  for (const char* p = s; *p; p++) {
    Keyboard.press(*p);           // 按下
    delay(KEY_HOLD_MS);           // 保持一小会儿（主机轮询能采到）
    Keyboard.release(*p);         // 抬起
    delay(KEY_SETTLE_MS);         // 字符间隔，防止被合并/触发重复
  }
  Keyboard.releaseAll();          // 收尾再清一次
  Serial.printf("[Key] 已键入: \"%s\"\n", s);
}

/* ---------- 以键盘身份键入 PIN（完整流程） ----------
 * 时序：WOL 唤醒 → ESC 点亮屏幕 → 回车让焦点进入 PIN 框 → 逐字键入 → 回车
 *
 * ⚠️ 前置的 ESC / 回车只在「锁屏或黑屏」时是安全的：
 *    若电脑已解锁、焦点在浏览器里，ESC 会关掉页面元素、回车会触发误操作
 *    （实测踩过：未锁屏时回车落进必应搜索框 → 触发搜索 + 联想补全）。
 *    所以是否发送前置键由 LOGIN_SEND_PREAMBLE 控制。
 *    想更保守就直接发 login1（纯 PIN，不带任何前置键）。
 * '\n' = 回车，0x1B = ESC
 */
void doLogin() {
#if ENABLE_WOL
  sendWol();                    // 若笔记本在睡眠，先通过网卡魔术包唤醒它
  delay(3000);                  // 等笔记本被唤醒（1~3 秒）
#endif

#if LOGIN_SEND_PREAMBLE
  Keyboard.releaseAll();
  Keyboard.write(0x1B);         // ESC：点亮屏幕 / 退出可能的全屏
  delay(WAKE_WAIT_MS);
  Keyboard.write('\n');         // 回车：把焦点送进 PIN 框
  delay(FOCUS_WAIT_MS);
  Serial.println("[Key] 已发送前置键（ESC + 回车）");
#else
  delay(WAKE_WAIT_MS);          // 跳过前置键，只留一点等待时间
  Serial.println("[Key] 已跳过前置键（LOGIN_SEND_PREAMBLE=0）");
#endif

  typeString(PIN_CODE);
  delay(300);
  Keyboard.releaseAll();
  Keyboard.write('\n');         // 回车提交
  Serial.println("[Key] 完整流程完成");
}

/* ---------- 纯输入 PIN（锁屏界面 PIN 框已就绪时用） ----------
 * 不做唤醒/ESC/前置回车，直接逐字键入 PIN 再回车——锁屏状态直接登录。
 */
void doLogin1() {
  typeString(PIN_CODE);
  delay(300);
  Keyboard.releaseAll();
  Keyboard.write('\n');
  Serial.println("[Key] 纯 PIN 输入完成");
}

/* ---------- 打字自检（dry-run）：把 PIN 当成普通文本打出去 ----------
 * 用途：在**记事本 / 任意输入框**里验证 ESP 到底发出了什么字符。
 *   - 若记事本里得到干净的 "123456" → ESP 侧没问题，多出的字符是
 *     主机侧的（浏览器搜索框联想补全 / 输入法 / 焦点）；
 *   - 若记事本里也有多余字符 → 问题在发送端，再回头查固件/主机轮询。
 * 只读配置，不改任何状态，安全。
 */
void typeSelfTest() {
  Serial.println("[Test] === 打字自检 ===");
  Serial.println("[Test] 请在 5 秒内切换到电脑上的「记事本」并点进输入区");
  delay(5000);                     // 给用户 5 秒切到记事本
  Keyboard.releaseAll();
  delay(100);
  Serial.printf("[Test] 即将输出 %u 个字符: \"%s\"\n",
                (unsigned)strlen(PIN_CODE), PIN_CODE);
  typeString(PIN_CODE);
  delay(500);
  Keyboard.releaseAll();
  Keyboard.write('\n');
  delay(200);
  Serial.printf("[Test] === 自检结束，请核对记事本内容是否为 %u 个字符 ===\n",
                (unsigned)strlen(PIN_CODE));
}

/* ---------- 三大动作（网页路由 / MQTT 回调共用） ---------- */
void actionBootAndLogin() {  // 开机并自动登录
  if (bootLogin) return;     // 防抖：已在等待流程中，忽略重复 on
  pressPower();
  loginAt = millis() + BOOT_WAIT_MS;
  bootLogin = true;
}
void actionPowerOnly() {  // 只开机
  pressPower();
}

/* ---------- 鉴权 ---------- */
bool checkToken() {
  if (server.hasArg("token") && server.arg("token") == PAGE_TOKEN) return true;
  server.send(401, "text/plain", "口令错误");
  return false;
}

#if USE_CLOUD
/* ---------- 巴法云 MQTT 回调：收到消息执行对应动作 ---------- */
/* 重复投递防护：手机小程序/控制台在弱网下可能把同一条消息发两次
 * （或巴法云做重试），如果 login1 被投两次，PIN 就会被完整打两遍。
 * 这里用「消息 + 时间窗」去重：同一内容 3 秒内只执行一次。
 */
String lastMsg = "";
uint32_t lastMsgAt = 0;
const uint32_t MSG_DEDUP_MS = 3000;

void mqttCallback(char* topic, byte* payload, unsigned int len) {
  String msg;
  for (unsigned int i = 0; i < len; i++) msg += (char)payload[i];
  msg.trim();
  Serial.printf("[MQTT] 收到: \"%s\"\n", msg.c_str());

  // 去重：同一条消息 3 秒内重复到达则忽略
  if (msg == lastMsg && (millis() - lastMsgAt) < MSG_DEDUP_MS) {
    Serial.printf("[MQTT] ⚠️ 重复消息（%lu ms 内），已忽略\n",
                  (unsigned long)(millis() - lastMsgAt));
    return;
  }
  lastMsg = msg;
  lastMsgAt = millis();

  if (msg == "on") actionBootAndLogin();         // 开机并自动登录（防抖：等待期忽略重复 on）
  else if (msg == "off") actionPowerOnly();      // 只开机
  else if (msg == "login") doLogin();            // 唤醒+键入 PIN（睡眠/完整流程）
  else if (msg == "login1") doLogin1();          // 纯键入 PIN（锁屏 PIN 框已就绪）
  else if (msg == "test") typeSelfTest();        // 打字自检（打进记事本，验证发的是什么）
}

void mqttConnect() {
  if (mqtt.connected()) return;
  if (millis() < mqttRetryAt) return;  // 非阻塞重连：5 秒试一次
  mqttRetryAt = millis() + 5000;

  // 连接前先确认 WiFi 还在
  if (WiFi.status() != WL_CONNECTED) {
    Serial.printf("[MQTT] WiFi 断开 (status=%d)，等待重连...\n", WiFi.status());
    return;
  }

  Serial.printf("[MQTT] 连接 bemfa.com:9501 ... (IP=%s)\n",
                WiFi.localIP().toString().c_str());
  // 巴法云：ClientID = 私钥 uid，无需用户名密码
  if (mqtt.connect(BEMFA_UID)) {
    mqtt.subscribe(BEMFA_TOPIC);
    Serial.printf("[MQTT] ✅ 成功，已订阅主题 %s\n", BEMFA_TOPIC);
  } else {
    // state 含义：-4=超时 -3=连接丢失 -2=连接失败 -1=断开 1..5=协议错误
    Serial.printf("[MQTT] ❌ 失败 (state=%d)，5 秒后重试\n", mqtt.state());
    if (mqtt.state() == -4) {
      Serial.println("[MQTT]    state=-4 = TCP 超时（网络不通或被拦）");
    } else if (mqtt.state() == -2) {
      Serial.println("[MQTT]    state=-2 = TCP 连接失败（服务器拒连/DNS 失败）");
    } else if (mqtt.state() == 5) {
      Serial.println("[MQTT]    state=5 = 未授权（检查 BEMFA_UID 是否正确）");
    }
  }
}
#endif

void setup() {
  Serial.begin(115200);
  delay(800);        // 等 USB 虚拟串口枚举完，防止开头几行日志丢失
  Keyboard.begin();  // 枚举为 USB HID 键盘

  /* ---- 开机自报家门：一眼确认烧进去的是哪版、舵机接的是哪个引脚 ---- */
  Serial.println();
  Serial.println("===== WiFi PIN 登录器 v5.4 =====");
  Serial.printf("[Cfg] 舵机信号脚 ACT_PIN = %d  →  GPIO%d\n", ACT_PIN, ACT_PIN);
#if ENABLE_WOL
  Serial.printf("[Cfg] WOL 有线 MAC = %s\n",
                strlen(PC_MAC) == 12 ? PC_MAC : "⚠️ 长度不对，必须 12 位！");
  Serial.printf("[Cfg] WOL 无线 MAC = %s\n",
                strlen(PC_MAC2) == 12 ? PC_MAC2 : "(未启用)");
#else
  Serial.println("[Cfg] WOL 已关闭（ENABLE_WOL=0）—— 本项目用「锁屏代替睡眠」");
#endif
  Serial.printf("[Cfg] 待机 %d° / 按压 %d° / 按住 %u ms\n",
                SERVO_IDLE_DEG, SERVO_PRESS_DEG, (unsigned)PRESS_HOLD_MS);
  Serial.printf("[Cfg] WiFi = %s\n", WIFI_SSID);

  powerServo.setPeriodHertz(50);  // SG90 标准 50Hz
  powerServo.attach(ACT_PIN, 500, 2500);
  powerServo.write(SERVO_IDLE_DEG);  // 上电回待机位，别一直压着电源键
  Serial.printf("[Servo] 已初始化，归位到 %d°（信号脚 GPIO%d）\n",
                SERVO_IDLE_DEG, ACT_PIN);

  WiFi.mode(WIFI_STA);
  if (strlen(WIFI_PASS) > 0) {
    WiFi.begin(WIFI_SSID, WIFI_PASS);
  } else {
    WiFi.begin(WIFI_SSID);  // 开放网络（无密码，如校园 Portal WiFi）
  }
  while (WiFi.status() != WL_CONNECTED) {
    delay(400);
    Serial.print('.');
  }
  Serial.printf("\n[OK] 已连接 WiFi，网页地址: http://%s/\n",
                WiFi.localIP().toString().c_str());
#if USE_CLOUD
  diagnoseNetwork();  // 诊断 MQTT 端口可达性（不阻塞）
#endif
#if ENABLE_WOL
  wolUdp.begin(9);  // WiFi 就绪后再初始化 UDP（WOL 广播用）
#endif

  server.on("/", HTTP_GET, []() {
    server.send_P(200, "text/html", PAGE);
  });

  server.on("/servologin", HTTP_GET, []() {
    if (!checkToken()) return;
    server.send(200, "text/plain",
                "OK：电源键已按压，" + String(BOOT_WAIT_MS / 1000) + " 秒后自动键入 PIN");
    actionBootAndLogin();  // 防抖：bootLogin 等待期忽略重复触发
  });

  server.on("/servopower", HTTP_GET, []() {
    if (!checkToken()) return;
    server.send(200, "text/plain", "OK：电源键已按压");
    actionPowerOnly();
  });

  server.on("/login", HTTP_GET, []() {
    if (!checkToken()) return;
    server.send(200, "text/plain", "OK：正在键入 PIN（约 5 秒）");
    doLogin();
  });

  server.on("/login1", HTTP_GET, []() {
    if (!checkToken()) return;
    server.send(200, "text/plain", "OK：正在键入 PIN（纯输入）");
    doLogin1();
  });

  server.on("/test", HTTP_GET, []() {
    if (!checkToken()) return;
    server.send(200, "text/plain", "OK：3 秒后开始打字自检，请先在电脑上打开记事本");
    typeSelfTest();
  });

  server.begin();
  Serial.println("[OK] 网页服务已启动");

#if USE_CLOUD
  mqtt.setServer("bemfa.com", 9501);  // 巴法云 MQTT 端口 9501（MQTT设备云）
  mqtt.setCallback(mqttCallback);
  mqtt.setKeepAlive(15);  // 15s 心跳防半开掉线（60s 会被服务器踢）
  mqttConnect();
#endif
}

void loop() {
  server.handleClient();
#if USE_CLOUD
  // WiFi 掉线自动重连（校园网 Portal 会定时踢人，必须处理）
  if (WiFi.status() != WL_CONNECTED) {
    if (millis() - lastWifiCheck > 10000) {   // 10 秒检查一次，别太频繁
      lastWifiCheck = millis();
      Serial.printf("[WiFi] 掉线了 (status=%d)，重连中...\n", WiFi.status());
      WiFi.disconnect();
      delay(100);
      if (strlen(WIFI_PASS) > 0) WiFi.begin(WIFI_SSID, WIFI_PASS);
      else                       WiFi.begin(WIFI_SSID);
    }
  }
  mqttConnect();  // 内部自带 5 秒重试节流
  mqtt.loop();    // 处理收到的消息与心跳
#endif
  if (bootLogin && loginAt && millis() >= loginAt) {
    bootLogin = false;
    loginAt = 0;
    doLogin();
  }
}
