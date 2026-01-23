#include "ssh_chatter/utils/jwt.h"
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// Base64url encoding/decoding helpers
static char *base64url_encode(const unsigned char *input, size_t length) {
    if (!input) return NULL;
    
    // Standard Base64 length calculation
    size_t encoded_len = 4 * ((length + 2) / 3);
    char *output = malloc(encoded_len + 1);
    if (!output) return NULL;

    const char base64_chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_ ";
    
    size_t i = 0, j = 0;
    while (i < length) {
        uint32_t octet_a = i < length ? input[i++] : 0;
        uint32_t octet_b = i < length ? input[i++] : 0;
        uint32_t octet_c = i < length ? input[i++] : 0;

        uint32_t triple = (octet_a << 0x10) + (octet_b << 0x08) + octet_c;

        output[j++] = base64_chars[(triple >> 3 * 6) & 0x3F];
        output[j++] = base64_chars[(triple >> 2 * 6) & 0x3F];
        output[j++] = base64_chars[(triple >> 1 * 6) & 0x3F];
        output[j++] = base64_chars[(triple >> 0 * 6) & 0x3F];
    }
    
    // Padding logic for base64url (no padding chars '=')
    // But we need to adjust the length based on actual input
    size_t pad_count = (3 - length % 3) % 3;
    output[j - pad_count] = '\0';
    
    return output;
}

static unsigned char *base64url_decode(const char *input, size_t *out_len) {
    if (!input || !out_len) return NULL;
    
    size_t len = strlen(input);
    size_t padding = 0;
    
    // In base64url, padding is omitted, we might need to restore it logic-wise or just handle it
    // standard decode logic:
    if (len % 4 == 2) padding = 2;
    else if (len % 4 == 3) padding = 1;
    
    size_t decoded_len = (len * 3) / 4; // approximate
    unsigned char *output = malloc(decoded_len + padding + 1); // + safety
    if (!output) return NULL;

    // Decoding table could be faster, but loop is simple for now
    int values[256];
    for (int i = 0; i < 256; i++) values[i] = -1;
    for (int i = 'A'; i <= 'Z'; i++) values[i] = i - 'A';
    for (int i = 'a'; i <= 'z'; i++) values[i] = i - 'a' + 26;
    for (int i = '0'; i <= '9'; i++) values[i] = i - '0' + 52;
    values['-'] = 62;
    values['_'] = 63;

    size_t i = 0, j = 0;
    uint32_t sextet_a, sextet_b, sextet_c, sextet_d;
    
    // Temporarily handle input as if it were padded
    // We iterate over the input string
    while (i < len) {
        sextet_a = i < len ? (uint32_t)values[(unsigned char)input[i++]] : 0;
        sextet_b = i < len ? (uint32_t)values[(unsigned char)input[i++]] : 0;
        sextet_c = i < len ? (uint32_t)values[(unsigned char)input[i++]] : 0;
        sextet_d = i < len ? (uint32_t)values[(unsigned char)input[i++]] : 0;

        uint32_t triple = (sextet_a << 18) + (sextet_b << 12) + (sextet_c << 6) + sextet_d;

        if (j < decoded_len + padding) output[j++] = (triple >> 16) & 0xFF;
        if (j < decoded_len + padding) output[j++] = (triple >> 8) & 0xFF;
        if (j < decoded_len + padding) output[j++] = triple & 0xFF;
    }
    
    // Adjust final length based on virtual padding
    *out_len = j - padding;
    // Note: the loop above is slightly inexact for unpadded input end, fixing up:
    // Simple fix: Recalculate based on input length logic precisely if needed, 
    // or trust the padding calc.
    // Let's refine:
    // If input length % 4 == 2, we read 2 chars, output 1 byte.
    // If input length % 4 == 3, we read 3 chars, output 2 bytes.
    // The loop reads 4 at a time.
    // If we overshoot, we just cap `j`.
    // Correct approach for `out_len`:
    *out_len = (len * 3) / 4;
    // If padding was needed (len % 4 != 0), the integer division truncates correctly for base64url usually?
    // No, 2 chars (12 bits) -> 1 byte (8 bits) + 4 spare.
    // 3 chars (18 bits) -> 2 bytes (16 bits) + 2 spare.
    // So (len * 3) / 4 is correct for pure data content if no padding chars present.
    
    return output;
}

static void hmac_sha256(const char *secret, const char *data, unsigned char *output, unsigned int *len) {
    HMAC(EVP_sha256(), secret, (int)strlen(secret), (unsigned char *)data, strlen(data), output, len);
}

char *jwt_generate(const char *secret, const char *username, int64_t expiry_seconds) {
    if (!secret || !username) return NULL;

    // Header
    const char *header_json = "{\"alg\":\"HS256\",\"typ\":\"JWT\"}";
    char *header_b64 = base64url_encode((unsigned char *)header_json, strlen(header_json));
    
    // Payload
    time_t now = time(NULL);
    char payload_json[256];
    snprintf(payload_json, sizeof(payload_json), "{\"sub\":\"%s\",\"iat\":%ld,\"exp\":%ld}", 
             username, (long)now, (long)(now + expiry_seconds));
    char *payload_b64 = base64url_encode((unsigned char *)payload_json, strlen(payload_json));

    // Signature Input
    size_t input_len = strlen(header_b64) + 1 + strlen(payload_b64) + 1;
    char *signature_input = malloc(input_len);
    snprintf(signature_input, input_len, "%s.%s", header_b64, payload_b64);

    // Sign
    unsigned char signature[EVP_MAX_MD_SIZE];
    unsigned int signature_len = 0;
    hmac_sha256(secret, signature_input, signature, &signature_len);
    
    char *signature_b64 = base64url_encode(signature, signature_len);

    // Combine
    size_t jwt_len = strlen(signature_input) + 1 + strlen(signature_b64) + 1;
    char *jwt = malloc(jwt_len);
    snprintf(jwt, jwt_len, "%s.%s", signature_input, signature_b64);

    free(header_b64);
    free(payload_b64);
    free(signature_input);
    free(signature_b64);
    return jwt;
}

bool jwt_verify(const char *secret, const char *token, char **username_out) {
    if (!secret || !token) return false;

    // Split token
    const char *dot1 = strchr(token, '.');
    if (!dot1) return false;
    const char *dot2 = strchr(dot1 + 1, '.');
    if (!dot2) return false;

    // Extract parts
    size_t header_len = (size_t)(dot1 - token);
    size_t payload_len = (size_t)(dot2 - (dot1 + 1));

    char *header_b64 = malloc(header_len + 1);
    char *payload_b64 = malloc(payload_len + 1);
    memcpy(header_b64, token, header_len); header_b64[header_len] = '\0';
    memcpy(payload_b64, dot1 + 1, payload_len); payload_b64[payload_len] = '\0';

    // Re-calculate signature
    size_t input_len = header_len + 1 + payload_len + 1;
    char *signature_input = malloc(input_len);
    snprintf(signature_input, input_len, "%s.%s", header_b64, payload_b64);

    unsigned char expected_sig[EVP_MAX_MD_SIZE];
    unsigned int expected_sig_len = 0;
    hmac_sha256(secret, signature_input, expected_sig, &expected_sig_len);
    
    char *expected_sig_b64 = base64url_encode(expected_sig, expected_sig_len);
    
    bool valid = (strcmp(expected_sig_b64, dot2 + 1) == 0);
    
    free(signature_input);
    free(expected_sig_b64);
    free(header_b64); // Cleanup

    if (!valid) {
        free(payload_b64);
        return false;
    }

    // Decode payload and check expiration
    size_t json_len = 0;
    unsigned char *payload_json = base64url_decode(payload_b64, &json_len);
    free(payload_b64);
    
    if (!payload_json) return false;
    
    // Very simple JSON parse for "sub" and "exp"
    // Note: This assumes standard JSON formatting from our generator or similar
    char *json_str = (char *)payload_json; // Safe to cast as it's a string data
    
    // Check expiration
    char *exp_key = strstr(json_str, "\"exp\":");
    if (exp_key) {
        long exp = atol(exp_key + 6);
        if (time(NULL) > exp) {
            free(payload_json);
            return false;
        }
    }

    // Get username (sub)
    if (username_out) {
        char *sub_key = strstr(json_str, "\"sub\":\"");
        if (sub_key) {
            char *start = sub_key + 7;
            char *end = strchr(start, '"');
            if (end) {
                size_t name_len = (size_t)(end - start);
                *username_out = malloc(name_len + 1);
                memcpy(*username_out, start, name_len);
                (*username_out)[name_len] = '\0';
            }
        }
    }

    free(payload_json);
    return true;
}
