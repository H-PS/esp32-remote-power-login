// power_bridge.scad —— WiFi 开机器一体化外壳（微型电磁铁版）
// ================================================================
// 用法：
//   1. 装 OpenSCAD（免费开源：openscad.org）
//   2. 实测你的笔记本后修改下方参数（每个参数都有注释教你怎么量）
//   3. F5 预览 → F6 渲染 → 导出 STL → 切片打印
// 材料建议：PLA，0.2mm 层高，3 层壁，20% 填充，无需支撑
// 公差：孔/槽尺寸已含 0.35mm 配合公差，首版打印后按松紧微调 tol
//
// 设计要点：桥形结构 = 天然机械限位
//   电磁铁竖装在桥体中央，推杆只有上下一个运动自由度，
//   物理上不存在横扫/侧滑，是执行器安全的终极形态。

$fn = 64;

/* ====== 1. 桥体尺寸（拿直尺实测后修改） ====== */
span   = 46;   // 两脚柱内侧跨距：电源键宽度 + 两侧各留 ≥8mm 落胶面
depth  = 26;   // 前后深度：覆盖按键区即可
foot_h = 16;   // 脚柱高：跨过键盘面板与键面的高度差 + 3mm 余量
foot_w = 9;    // 脚柱宽度
top_t  = 3.2;  // 顶板厚度（3 层壁）

/* ====== 2. 电磁铁参数（到手后用卡尺实测核对） ====== */
// 型号 ZYE1-P08/15：筒身 Φ8×15.5mm，一端带法兰
sol_d    = 8.0;   // 筒身直径
sol_l    = 15.5;  // 总长（含法兰）
flange_d = 9.6;   // 法兰直径（法兰朝下，挂在筒底沉槽上）
push_d   = 3.9;   // 推杆直径

/* ====== 3. ESP32-S3 托盘（按你的板实测） ====== */
esp_l  = 55;   // 板长（含 USB 口那端）
esp_w  = 26;   // 板宽
esp_h  = 7;    // 板厚 + 正面元件最高点
usb_w  = 12;   // USB 口开槽宽
usb_h  = 7;    // USB 口开槽高

/* ====== 4. 公差与壁厚 ====== */
tol    = 0.35;  // FDM 配合公差：装不进就调大，太松就调小
wall_t = 2.4;   // 筒壁/托盘墙厚（3 层壁）

/* ====== 派生尺寸，勿改 ====== */
total_l  = span + 2 * foot_w;   // 桥体总长
cx       = total_l / 2;         // 电磁铁中心 x（跨中 = 键中心上方）
cy       = depth / 2;
tube_bot = 2.5;                 // 筒底离键面的净空（推杆行程 + 键程的余量）
gap_h    = foot_h - tube_bot;   // 筒身露出顶板以下的高度

difference() {
  union() {
    /* ---- 两只脚柱 ---- */
    cube([foot_w, depth, foot_h]);
    translate([total_l - foot_w, 0, 0])
      cube([foot_w, depth, foot_h]);

    /* ---- 顶板（横跨） ---- */
    cube([total_l, depth, foot_h + top_t]);

    /* ---- 电磁铁外筒（挂在顶板下方，跨中） ---- */
    translate([cx, cy, tube_bot])
      cylinder(d = sol_d + 2 * wall_t + 1, h = gap_h);

    /* ---- ESP32-S3 托盘（叠在顶板上方，占整桥长） ---- */
    translate([0, depth - esp_w - wall_t, foot_h + top_t])
      cube([total_l, esp_w + wall_t, esp_h + wall_t + 2 * tol]);
  }

  /* ---- 电磁铁筒身内腔（从筒底向上贯穿顶板，电磁铁从上往下插入） ---- */
  translate([cx, cy, tube_bot])
    cylinder(d = sol_d + 2 * tol, h = foot_h + top_t - tube_bot + 2);

  /* ---- 法兰沉槽（电磁铁法兰坐在这里，整支重量由收口承担） ---- */
  translate([cx, cy, tube_bot])
    cylinder(d = flange_d + 2 * tol, h = 2.0);

  /* ---- 推杆出口（筒底收口，兼导向） ---- */
  translate([cx, cy, -0.5])
    cylinder(d = push_d + 1.4, h = tube_bot + 3);

  /* ---- 电磁铁顶部引线槽（向上引出到托盘区） ---- */
  translate([cx - 2, cy - 2, foot_h + top_t - 3])
    cube([4, esp_w + wall_t - depth/2 + 4, 5]);

  /* ---- ESP 托盘内腔（挖空） ---- */
  translate([wall_t, depth - esp_w, foot_h + top_t + tol])
    cube([total_l - 2 * wall_t, esp_w + 1, esp_h + 2 * tol + 1]);

  /* ---- USB 口过线槽（托盘 x+ 端墙，朝外） ---- */
  translate([total_l - wall_t - 1, cy - usb_w / 2, foot_h + top_t + tol])
    cube([wall_t + 4, usb_w, usb_h]);

  /* ---- 脚柱减重孔（省料，不影响强度） ---- */
  translate([-1, cy - 6, 3])
    cube([foot_w + 2, 12, foot_h - 3]);
  translate([total_l - foot_w - 1, cy - 6, 3])
    cube([foot_w + 2, 12, foot_h - 3]);
}

/* ---- 使用说明（渲染时注释区不影响模型） ----
 * 1. 安装顺序：纳米胶贴两脚柱底 → 桥体跨过电源键 → 电磁铁从顶板
 *    上方插入（法兰朝下，坐在筒底沉槽）→ 推杆顶端正对键帽中心
 *    → ESP32-S3 放入托盘（USB 口朝外）→ 接线见 README
 * 2. 推杆顶端务必贴一小块硅胶垫/海绵，保护键帽、消敲击声
 * 3. 桥下净空 tube_bot = 2.5mm：推杆行程 2mm + 键程 1.5mm 若压不到底，
 *    把 foot_h 加大 1mm 重打；压太深则相反
 * 4. 平时人手按键：桥体横跨在键上方，手指从侧面/后方伸入按键；
 *    若挡手，把 depth 或 esp_w 调小、桥往键位上方挪
 * 5. 想换舵机凸轮版：把电磁铁筒换成"舵机横仓"（内腔 23×13×13，
 *    输出轴竖直朝下穿顶板，盘缘对键），原理相同
 */
