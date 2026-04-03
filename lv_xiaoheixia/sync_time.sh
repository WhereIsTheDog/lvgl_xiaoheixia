#!/bin/sh
export TZ='CST-8'

HTTP_DATE=$(wget -q -S -O /dev/null "http://www.baidu.com" 2>&1 | grep -i "Date:" | head -1 | sed 's/.*Date: *//')

if [ -z "$HTTP_DATE" ]; then
    echo "Failed: no Date header"
    exit 1
fi

echo "HTTP Date: $HTTP_DATE"

Y=$(echo "$HTTP_DATE" | awk '{print $4}')
M=$(echo "$HTTP_DATE" | awk '{print $3}')
D=$(echo "$HTTP_DATE" | awk '{print $2}')
T=$(echo "$HTTP_DATE" | awk '{print $5}')

case $M in
    Jan) M=01;; Feb) M=02;; Mar) M=03;; Apr) M=04;;
    May) M=05;; Jun) M=06;; Jul) M=07;; Aug) M=08;;
    Sep) M=09;; Oct) M=10;; Nov) M=11;; Dec) M=12;;
esac

date -u -s "${Y}-${M}-${D} ${T}" 2>/dev/null
echo "Time set: $(TZ='CST-8' date '+%Y-%m-%d %H:%M:%S CST')"
