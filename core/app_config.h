#pragma once
#include <string>

struct AppConfig {
  std::string input_path;
  std::string yolo_path = "model/yolov8s.rknn";
  std::string sam_enc_path = "model/mobile_sam_encoder.rknn";
  std::string sam_dec_path = "model/mobile_sam_decoder.rknn";
  std::string label_path = "model/coco_80_labels_list.txt";
  std::string out_dir = "outputs";
  int yolo_threads = 3;
  int sam_threads = 3;
  bool use_sam = true;
};
