#ifndef KWS_MODEL_H
#define KWS_MODEL_H

#include <stdint.h>

#define KWS_MODEL_MAGIC    0x4D53574Bu
#define KWS_MODEL_VERSION  2u
#define KWS_MODEL_LABEL    16u

#define KWS_MODEL_FLAG_PLACEHOLDER  0x0001u

#define KWS_SLOT_INPUT     0xFFu
#define KWS_SLOTS          3u

typedef enum
{
    KWS_OP_CONV   = 1,
    KWS_OP_DWCONV = 2,
    KWS_OP_ADD    = 3
} kws_op_t;

typedef struct
{
    uint32_t magic;
    uint16_t version;
    uint16_t layer_count;
    uint16_t step_frames;
    uint16_t mel_bins;
    uint16_t flags;
    uint16_t state_bytes;
    float    in_scale;
    int32_t  in_zero;
    float    out_scale;
    int32_t  out_zero;
    uint32_t arena_bytes;
    uint32_t payload_off;
    uint32_t payload_len;
    char     label[KWS_MODEL_LABEL];
} kws_model_header_t;

typedef struct
{
    uint8_t  op;
    uint8_t  relu;
    uint8_t  src;
    uint8_t  dst;
    uint8_t  aux;
    uint8_t  stride_t;
    uint8_t  stride_f;
    uint8_t  dilation;
    uint16_t k_t;
    uint16_t k_f;
    uint16_t in_t;
    uint16_t in_f;
    uint16_t in_c;
    uint16_t out_t;
    uint16_t out_f;
    uint16_t out_c;
    uint16_t hist_t;
    uint16_t pad_f;
    int32_t  in_zero;
    int32_t  out_zero;
    int32_t  aux_zero;
    uint32_t state_off;
    uint32_t w_off;
    uint32_t b_off;
    uint32_t q_off;
} kws_layer_t;

int      kws_model_load(void);
uint32_t kws_model_arena_bytes(void);
void     kws_model_set_arena(void *base, uint32_t bytes);
void     kws_model_reset(void);
float    kws_model_step(const int8_t *frames);

const kws_model_header_t *kws_model_info(void);
uint32_t kws_model_flash_bytes(void);
uint32_t kws_model_weights(void);
uint32_t kws_model_last_us(void);
uint32_t kws_model_step_count(void);

#endif /* KWS_MODEL_H */
