#ifndef ACR_PARSER_H
#define ACR_PARSER_H

#include <stddef.h>

typedef struct {
    int found;
    int state;
    char prediction[32];
    double ai_probability;
} acr_result_t;

int acr_parser_extract_matching_file_result(const char *json_response,
                                            const char *expected_name,
                                            acr_result_t *result);
void acr_parser_log_result_summary(const acr_result_t *result);

#endif
