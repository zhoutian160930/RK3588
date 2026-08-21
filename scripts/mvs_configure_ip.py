#!/usr/bin/env python3
"""海康 GigE 相机 IP 配置(官方 ForceIPEx 例程流程),部署新板/换相机时用一次:
  1. ForceIpEx 强制下发 IP(相机 IP 未知/0.0.0.0/DHCP 失败时也能救活)
  2. 重新枚举确认
  3. SetIpConfig(STATIC) 写入相机持久存储(断电保留)

用法:
  python3 mvs_configure_ip.py                          # 默认 192.168.1.64/24 网关 192.168.1.100
  python3 mvs_configure_ip.py --ip 192.168.1.65 ...    # 多相机时逐台指定唯一 IP

前提: 板端连接相机的网口已配同网段 IP(见 docs/deployment.md 第 3 节)。
"""
import argparse
import ctypes
import socket
import sys
import time

SDK_LIB = "/opt/MVS/lib/aarch64/libMvCameraControl.so"
MV_GIGE_DEVICE = 0x00000001
MV_IP_CFG_STATIC = 0x05000000
MV_IP_CFG_DHCP = 0x06000000
MV_IP_CFG_LLA = 0x04000000


class MV_CC_DEVICE_INFO_LIST(ctypes.Structure):
    _fields_ = [("nDeviceNum", ctypes.c_uint32),
                ("pDeviceInfo", (ctypes.c_void_p * 256))]


def ip_u32(ip):
    return int.from_bytes(socket.inet_aton(ip), "big")


def u32_ip(v):
    return socket.inet_ntoa(v.to_bytes(4, "big"))


def get_str(raw, off, n):
    return raw[off:off + n].split(b"\x00")[0].decode(errors="replace")


def enum(lib):
    lst = MV_CC_DEVICE_INFO_LIST()
    ret = lib.MV_CC_EnumDevices(MV_GIGE_DEVICE, ctypes.byref(lst))
    if ret != 0:
        sys.exit("EnumDevices failed: 0x%08X" % (ret & 0xFFFFFFFF))
    return lst


def show(lst, idx):
    raw = bytes(ctypes.cast(lst.pDeviceInfo[idx],
                            ctypes.POINTER(ctypes.c_uint8 * 220)).contents)
    print("  [%d] %s %s  SN:%s" % (
        idx, get_str(raw, 52, 32), get_str(raw, 84, 32), get_str(raw, 196, 16)))
    cur = int.from_bytes(raw[40:44], "little")
    msk = int.from_bytes(raw[44:48], "little")
    cfg = int.from_bytes(raw[36:40], "little")
    print("     IP:%s Mask:%s  cfg=%s" % (
        u32_ip(cur), u32_ip(msk),
        {MV_IP_CFG_STATIC: "static", MV_IP_CFG_DHCP: "dhcp",
         MV_IP_CFG_LLA: "lla"}.get(cfg, hex(cfg))))
    return cur


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ip", default="192.168.1.64")
    ap.add_argument("--mask", default="255.255.255.0")
    ap.add_argument("--gw", default="192.168.1.100")
    args = ap.parse_args()

    try:
        lib = ctypes.CDLL(SDK_LIB)
    except OSError:
        sys.exit("无法加载 %s(MVS SDK 未安装?)" % SDK_LIB)
    lib.MV_CC_EnumDevices.argtypes = [ctypes.c_uint32,
                                      ctypes.POINTER(MV_CC_DEVICE_INFO_LIST)]
    lib.MV_CC_EnumDevices.restype = ctypes.c_int
    lib.MV_CC_CreateHandleWithoutLog.argtypes = [
        ctypes.POINTER(ctypes.c_void_p), ctypes.c_void_p]
    lib.MV_CC_CreateHandleWithoutLog.restype = ctypes.c_int
    lib.MV_CC_DestroyHandle.argtypes = [ctypes.c_void_p]
    lib.MV_CC_DestroyHandle.restype = ctypes.c_int
    lib.MV_GIGE_ForceIpEx.argtypes = [ctypes.c_void_p, ctypes.c_uint32,
                                      ctypes.c_uint32, ctypes.c_uint32]
    lib.MV_GIGE_ForceIpEx.restype = ctypes.c_int
    lib.MV_GIGE_SetIpConfig.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
    lib.MV_GIGE_SetIpConfig.restype = ctypes.c_int

    print("== 配置前 ==")
    lst = enum(lib)
    if lst.nDeviceNum == 0:
        sys.exit("未发现 GigE 相机(检查网线/网口/供电)")
    show(lst, 0)

    print("== ForceIpEx 下发 %s ==" % args.ip)
    h = ctypes.c_void_p()
    r = lib.MV_CC_CreateHandleWithoutLog(ctypes.byref(h), lst.pDeviceInfo[0])
    if r:
        sys.exit("CreateHandle fail 0x%08X" % (r & 0xFFFFFFFF))
    r = lib.MV_GIGE_ForceIpEx(h, ip_u32(args.ip), ip_u32(args.mask),
                              ip_u32(args.gw))
    print("  ForceIpEx ret=0x%08X" % (r & 0xFFFFFFFF))
    lib.MV_CC_DestroyHandle(h)

    time.sleep(3)
    print("== 重新枚举 ==")
    lst = enum(lib)
    cur = show(lst, 0)
    if cur != ip_u32(args.ip):
        print("  警告: 相机 IP 仍不是 %s(多相机 IP 冲突? 逐台配置)" % args.ip)

    print("== SetIpConfig(STATIC) 持久化 ==")
    r = lib.MV_CC_CreateHandleWithoutLog(ctypes.byref(h), lst.pDeviceInfo[0])
    if r:
        sys.exit("CreateHandle fail 0x%08X" % (r & 0xFFFFFFFF))
    r = lib.MV_GIGE_SetIpConfig(h, MV_IP_CFG_STATIC)
    print("  SetIpConfig ret=0x%08X" % (r & 0xFFFFFFFF))
    lib.MV_CC_DestroyHandle(h)

    print("== 完成,最终状态 ==")
    show(enum(lib), 0)
    print("验证: ping %s" % args.ip)


if __name__ == "__main__":
    main()
