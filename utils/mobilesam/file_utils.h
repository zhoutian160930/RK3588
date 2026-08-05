#ifndef _RKNN_MODEL_ZOO_FILE_UTILS_H_
#define _RKNN_MODEL_ZOO_FILE_UTILS_H_

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Write data to file
 * 
 * @param path [in] File path
 * @param data [in] Write data
 * @param size [in] Write data size
 * @return int 0: success; -1: error
 */
int write_data_to_file(const char *path, const char *data, unsigned int size);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif //_RKNN_MODEL_ZOO_FILE_UTILS_H_