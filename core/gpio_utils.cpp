#include "gpio_utils.h"

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

static int get_base() {
  std::ifstream f("/sys/kernel/debug/gpio");
  if (!f.is_open()) return 485;
  std::string line;
  while (std::getline(f, line)) {
    if (line.find("i2c") == std::string::npos) continue;
    /* 行格式: gpiochip485 485-508 : i2c 2-0023 */
    std::stringstream ss(line);
    std::string tok;
    ss >> tok; /* gpiochipX */
    ss >> tok; /* 485-508 */
    size_t dash = tok.find('-');
    if (dash == std::string::npos) continue;
    return std::stoi(tok.substr(0, dash));
  }
  return 485;
}

int gpio_calc_pin(int tca_offset) {
  static int base = -1;
  if (base < 0) base = get_base();
  if (tca_offset >= 0 && tca_offset <= 7)
    return base + tca_offset;
  else if (tca_offset >= 10 && tca_offset <= 17)
    return base + tca_offset - 2;
  else if (tca_offset >= 20 && tca_offset <= 27)
    return base + tca_offset - 4;
  return base + tca_offset;
}
