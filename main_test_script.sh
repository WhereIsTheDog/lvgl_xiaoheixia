#!/bin/sh
# Boot splash
/usr/local/bin/lcd_test /mnt/udisk/product_test/my_image.bmp
# Configure loopback (system ships without lo IP, adbd needs 127.0.0.1)
ifconfig lo 127.0.0.1 netmask 255.0.0.0 up
# Restore last saved system time (if available)
TIME_FILE="/mnt/udisk/last_time.txt"
if [ -f "$TIME_FILE" ]; then
    SAVED_TIME=$(cat "$TIME_FILE")
    if [ "$SAVED_TIME" -gt 1577836800 ] 2>/dev/null; then
        date -s @$SAVED_TIME > /dev/null 2>&1
    fi
fi
# Install WiFi driver
/usr/local/bin/wifi_drv_ins
# Start wpa_supplicant
/etc/init.d/wpa_supplicant start
# Clear saved WiFi networks to prevent auto-connect on boot
sleep 1
for i in 0 1 2 3 4; do
    /usr/local/sbin/wpa_cli -i wlan0 remove_network $i 2>/dev/null
done
/usr/local/sbin/wpa_cli -i wlan0 save_config 2>/dev/null || true
# Run LVGL app
