#include <stdio.h>
#include <stdlib.h>
#include <string.h>

unsigned char* load_model(const char* filename, int* model_size)
{
    FILE* fp = fopen(filename, "rb");
    if (fp == NULL) {
        printf("fopen %s fail!\n", filename);
        return NULL;
    }
    fseek(fp, 0, SEEK_END);
    int model_len = ftell(fp);
    unsigned char* model = (unsigned char*)malloc(model_len);
    fseek(fp, 0, SEEK_SET);
    if (model_len != fread(model, 1, model_len, fp)) {
        printf("fread %s fail!\n", filename);
        free(model);
        fclose(fp);
        return NULL;
    }
    *model_size = model_len;
    fclose(fp);
    return model;
}

int write_data_to_file(const char *path, const char *data, unsigned int size)
{
    FILE *fp;

    fp = fopen(path, "w");
    if(fp == NULL) {
        printf("open error: %s\n", path);
        return -1;
    }

    fwrite(data, 1, size, fp);
    fflush(fp);

    fclose(fp);
    return 0;
}
