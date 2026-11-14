"""ESP32 纯 TCP 测速（电脑端）

用法：python tcp_speed.py [板子IP] [秒数]
      默认 IP 是 esp32-led.local，默认每个方向测 5 秒

上行（电脑→板子，5001 端口）：速度用板子自己统计的结果，电脑这边的发送速度不准，
    因为数据进了系统缓冲区就算"发出去"了。
下行（板子→电脑，5002 端口）：速度用电脑这边的接收统计。
"""
import socket
import sys
import time

HOST = sys.argv[1] if len(sys.argv) > 1 else "esp32-led.local"
SECONDS = float(sys.argv[2]) if len(sys.argv) > 2 else 5.0
CHUNK = bytes(65536)


def mbps(nbytes, seconds):
    return nbytes * 8 / seconds / 1e6 if seconds > 0 else 0.0


def test_up(ip):
    with socket.create_connection((ip, 5001), timeout=15) as s:
        sent, t0, last = 0, time.perf_counter(), 0.0
        while (elapsed := time.perf_counter() - t0) < SECONDS:
            s.sendall(CHUNK)
            sent += len(CHUNK)
            if elapsed - last >= 1:
                last = elapsed
                print(f"  {elapsed:4.1f}s  已发 {sent / 1048576:6.1f} MB")
        s.shutdown(socket.SHUT_WR)  # 告诉板子发完了，板子会回统计结果
        reply = s.makefile().readline().split()
    nbytes, us = int(reply[0]), int(reply[1])
    print(f"上行：板子收到 {nbytes / 1048576:.1f} MB，用时 {us / 1e6:.2f} s，"
          f"{mbps(nbytes, us / 1e6):.1f} Mbps")


def test_down(ip):
    with socket.create_connection((ip, 5002), timeout=15) as s:
        got, t0, last = 0, None, 0.0
        while True:
            data = s.recv(65536)
            if not data:
                break
            now = time.perf_counter()
            if t0 is None:
                t0 = now  # 从第一块数据开始计时，第一块不计入
                continue
            got += len(data)
            elapsed = now - t0
            if elapsed - last >= 1:
                last = elapsed
                print(f"  {elapsed:4.1f}s  已收 {got / 1048576:6.1f} MB  {mbps(got, elapsed):.1f} Mbps")
            if elapsed >= SECONDS:
                break
    elapsed = time.perf_counter() - t0
    print(f"下行：收到 {got / 1048576:.1f} MB，用时 {elapsed:.2f} s，{mbps(got, elapsed):.1f} Mbps")


if __name__ == "__main__":
    ip = socket.gethostbyname(HOST)  # 先解析一次，避免每次连接都查 mDNS
    print(f"板子 {HOST} = {ip}，每个方向测 {SECONDS:g} 秒")
    print("测上行（电脑→板子）...")
    test_up(ip)
    print("测下行（板子→电脑）...")
    test_down(ip)
