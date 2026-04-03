#!/bin/sh
# 1. 开机画面
/usr/local/bin/lcd_test /mnt/udisk/product_test/my_image.bmp

# 2. 配置 loopback 接口 (系统未默认配置, adbd 需要 127.0.0.1 正常工作)
ifconfig lo 127.0.0.1 netmask 255.0.0.0 up

# 3. 安装 WiFi 驱动
/usr/local/bin/wifi_drv_ins

# 4. 启动 wpa_supplicant
/etc/init.d/wpa_supplicant start

# 5. 清除已保存的 WiFi 网络，防止开机自动连接
#    等待 wpa_supplicant 就绪后立即清除所有 network 条目
sleep 1
for i in 0 1 2 3 4; do
    /usr/local/sbin/wpa_cli -i wlan0 remove_network $i 2>/dev/null
done
# 把空的网络列表持久化，下次开机也不会自动连接
/usr/local/sbin/wpa_cli -i wlan0 save_config 2>/dev/null || true

# 6. 运行 LVGL 应用
/mnt/udisk/product_test/lvgl_app_bin
