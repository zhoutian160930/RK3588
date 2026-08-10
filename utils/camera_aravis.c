/**
 * camera_aravis.c - 原生 aarch64 相机采集守护进程，替代 grab_stream+qemu
 *
 * 使用 Aravis (GigE Vision) 直接驱动 OPT 相机，输出保持和 grab_stream 相同格式:
 *   /tmp/camera_info.txt   - "width height"
 *   /tmp/camera_frame.raw  - 8B width, 8B height, 8B frame_id, w*h mono8
 *
 * 编译: gcc -std=c11 -O2 -o camera_aravis camera_aravis.c $(pkg-config --cflags --libs aravis-0.8)
 */

#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <signal.h>
#include <arpa/inet.h>
#include <arv.h>

static volatile int g_running = 1;

static void sig_handler(int sig) { g_running = 0; }

static int write_atomic(const char *path, const void *data, size_t len) {
    FILE *fp = fopen("/tmp/camera_frame.tmp", "wb");
    if (!fp) return -1;
    if (fwrite(data, 1, len, fp) != len) { fclose(fp); return -1; }
    fclose(fp);
    return rename("/tmp/camera_frame.tmp", path);
}

static void write_info(uint64_t w, uint64_t h) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%llu %llu\n", (unsigned long long)w, (unsigned long long)h);
    FILE *fp = fopen("/tmp/camera_info.txt", "w");
    if (fp) { fputs(buf, fp); fclose(fp); }
}

int main(int argc, char *argv[]) {
    signal(SIGTERM, sig_handler);
    signal(SIGINT, sig_handler);

    /* 可选参数: 相机 IP (默认自动发现) */
    const char *target_ip = NULL;
    if (argc > 1) target_ip = argv[1];

    /* 更新 Aravis 设备列表 */
    arv_update_device_list();
    int n = arv_get_n_devices();
    printf("camera_aravis: found %d device(s)\n", n);
    if (n == 0) {
        /* 尝试强制发现 GigE 设备 */
        printf("camera_aravis: forcing GigE discovery...\n");
        arv_enable_interface("GigEVision");
        arv_update_device_list();
        n = arv_get_n_devices();
        printf("camera_aravis: found %d device(s) after force\n", n);
    }
    if (n == 0) {
        fprintf(stderr, "camera_aravis: no cameras found\n");
        return 1;
    }

    /* 选择相机: 优先匹配 IP，否则第一个 */
    int sel = 0;
    for (int i = 0; i < n; i++) {
        const char *dev_id = arv_get_device_id(i);
        const char *addr = arv_get_device_address(i);
        printf("  [%d] %s  addr=%s\n", i, dev_id ? dev_id : "(nil)", addr ? addr : "(nil)");
        if (target_ip && addr && strcmp(addr, target_ip) == 0) sel = i;
    }
    printf("camera_aravis: using device [%d]\n", sel);

    /* 打开相机 */
    GError *error = NULL;
    ArvCamera *camera = arv_camera_new(arv_get_device_id(sel), &error);
    if (!camera) {
        fprintf(stderr, "camera_aravis: open failed: %s\n", error ? error->message : "?");
        if (error) g_clear_error(&error);
        return 1;
    }

    /* 获取分辨率 */
    int max_w, max_h;
    arv_camera_get_region(camera, NULL, NULL, &max_w, &max_h, &error);
    if (error) { g_clear_error(&error); max_w = max_h = 0; }
    if (max_w <= 0 || max_h <= 0) {
        arv_camera_get_sensor_size(camera, &max_w, &max_h, &error);
        if (error) { g_clear_error(&error); max_w = max_h = 0; }
    }
    printf("camera_aravis: resolution %d x %d\n", max_w, max_h);

    /* 设置 Mono8 像素格式 */
    arv_camera_set_pixel_format(camera, ARV_PIXEL_FORMAT_MONO_8, &error);
    if (error) {
        fprintf(stderr, "camera_aravis: pixel format: %s (continuing)\n", error->message);
        g_clear_error(&error);
    }

    /* 自动曝光：Continuous(持续自动) / Once(单次) / Off(手动) */
    arv_camera_set_exposure_time_auto(camera, ARV_AUTO_CONTINUOUS, &error);
    if (error) {
        fprintf(stderr, "camera_aravis: exposure auto: %s\n", error->message);
        g_clear_error(&error);
    }
    arv_camera_set_gain_auto(camera, ARV_AUTO_CONTINUOUS, &error);
    if (error) {
        fprintf(stderr, "camera_aravis: gain auto: %s\n", error->message);
        g_clear_error(&error);
    }
    /* 开启 Gamma 增强暗部细节 */
    arv_device_set_string_feature_value(arv_camera_get_device(camera),
                                        "GammaEnable", "true", &error);
    if (error) g_clear_error(&error);

    uint64_t cam_w = (uint64_t)max_w;
    uint64_t cam_h = (uint64_t)max_h;
    write_info(cam_w, cam_h);

    /* 开始采集 */
    arv_camera_start_acquisition(camera, &error);
    if (error) {
        fprintf(stderr, "camera_aravis: start failed: %s\n", error->message);
        g_clear_error(&error);
        g_object_unref(camera);
        return 1;
    }
    printf("camera_aravis: acquisition started\n");

    /* 主循环 */
    uint64_t frame_count = 0;
    size_t pixel_bytes = (size_t)max_w * max_h;
    size_t header_size = sizeof(uint64_t) * 3;
    size_t total_size = header_size + pixel_bytes;

    unsigned char *frame_data = malloc(total_size);
    memcpy(frame_data, &cam_w, 8);
    memcpy(frame_data + 8, &cam_h, 8);

    while (g_running) {
        ArvBuffer *buffer = arv_camera_acquisition(camera, 2000000, &error);
        if (!buffer) {
            if (error) {
                fprintf(stderr, "camera_aravis: grab error: %s\n", error->message);
                g_clear_error(&error);
            }
            usleep(10000);
            continue;
        }

        if (arv_buffer_get_status(buffer) == ARV_BUFFER_STATUS_SUCCESS) {
            size_t buf_size;
            const void *pixels = arv_buffer_get_data(buffer, &buf_size);
            if (pixels && buf_size >= pixel_bytes) {
                memcpy(frame_data + 8, &cam_h, 8);
                memcpy(frame_data + 16, &frame_count, 8);
                memcpy(frame_data + header_size, pixels, pixel_bytes);
                write_atomic("/tmp/camera_frame.raw", frame_data, total_size);
                frame_count++;
            }
        }

        g_clear_object(&buffer);

        if (frame_count % 100 == 0)
            printf("camera_aravis: %llu frames\n", (unsigned long long)frame_count);
    }

    printf("camera_aravis: stopping (%llu frames total)\n", (unsigned long long)frame_count);
    arv_camera_stop_acquisition(camera, NULL);
    free(frame_data);
    g_object_unref(camera);
    return 0;
}
