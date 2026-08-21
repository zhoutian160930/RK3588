#!/usr/bin/env python3
"""枚举 GigE 相机(海康 MVS SDK),部署时验证相机连通。
字段偏移与 mvs_configure_ip.py 一致(已实测验证)。

用法: python3 mvs_list_devices.py
"""
import ctypes
import socket

SDK_LIB = "/opt/MVS/lib/aarch64/libMvCameraControl.so"
MV_GIGE_DEVICE = 0x00000001


class MV_CC_DEVICE_INFO_LIST(ctypes.Structure):
    _fields_ = [("nDeviceNum", ctypes.c_uint32),
                ("pDeviceInfo", (ctypes.c_void_p * 256))]


def u32_ip(v):
    return socket.inet_ntoa(v.to_bytes(4, "big"))


def get_str(raw, off, n):
    return raw[off:off + n].split(b"\x00")[0].decode(errors="replace")


def main():
    lib = ctypes.CDLL(SDK_LIB)
    lib.MV_CC_EnumDevices.argtypes = [ctypes.c_uint32,
                                      ctypes.POINTER(MV_CC_DEVICE_INFO_LIST)]
    lib.MV_CC_EnumDevices.restype = ctypes.c_int

    lst = MV_CC_DEVICE_INFO_LIST()
    ret = lib.MV_CC_EnumDevices(MV_GIGE_DEVICE, ctypes.byref(lst))
    if ret != 0 or lst.nDeviceNum == 0:
        print("未发现 GigE 相机 (ret=0x%08X, num=%d)" % (ret & 0xFFFFFFFF,
                                                        lst.nDeviceNum))
        return 1

    print("发现 %d 台相机:" % lst.nDeviceNum)
    for i in range(lst.nDeviceNum):
        raw = bytes(ctypes.cast(lst.pDeviceInfo[i],
                                ctypes.POINTER(ctypes.c_uint8 * 220)).contents)
        print("  [%d] %s %s  SN:%s" % (
            i, get_str(raw, 52, 32), get_str(raw, 84, 32),
            get_str(raw, 196, 16)))
        print("      IP:%s Mask:%s GW:%s" % (
            u32_ip(int.from_bytes(raw[40:44], "little")),
            u32_ip(int.from_bytes(raw[44:48], "little")),
            u32_ip(int.from_bytes(raw[48:52], "little"))))
    return 0


if __name__ == "__main__":
    exit(main())
