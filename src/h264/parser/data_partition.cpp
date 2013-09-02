#include <stdio.h>

#include "memalloc.h"
#include "bitstream_cabac.h"
#include "data_partition.h"


// Table 9-44 Specification of rangeTabLPS depending on pStateIdx and qCodIRangeIdx
static const uint8_t rangeTabLPS[64][4] = {
    { 128, 176, 208, 240 }, { 128, 167, 197, 227 }, { 128, 158, 187, 216 }, { 123, 150, 178, 205 },
    { 116, 142, 169, 195 }, { 111, 135, 160, 185 }, { 105, 128, 152, 175 }, { 100, 122, 144, 166 },
    {  95, 116, 137, 158 }, {  90, 110, 130, 150 }, {  85, 104, 123, 142 }, {  81,  99, 117, 135 },
    {  77,  94, 111, 128 }, {  73,  89, 105, 122 }, {  69,  85, 100, 116 }, {  66,  80,  95, 110 },
    {  62,  76,  90, 104 }, {  59,  72,  86,  99 }, {  56,  69,  81,  94 }, {  53,  65,  77,  89 },
    {  51,  62,  73,  85 }, {  48,  59,  69,  80 }, {  46,  56,  66,  76 }, {  43,  53,  63,  72 },
    {  41,  50,  59,  69 }, {  39,  48,  56,  65 }, {  37,  45,  54,  62 }, {  35,  43,  51,  59 },
    {  33,  41,  48,  56 }, {  32,  39,  46,  53 }, {  30,  37,  43,  50 }, {  29,  35,  41,  48 },
    {  27,  33,  39,  45 }, {  26,  31,  37,  43 }, {  24,  30,  35,  41 }, {  23,  28,  33,  39 },
    {  22,  27,  32,  37 }, {  21,  26,  30,  35 }, {  20,  24,  29,  33 }, {  19,  23,  27,  31 },
    {  18,  22,  26,  30 }, {  17,  21,  25,  28 }, {  16,  20,  23,  27 }, {  15,  19,  22,  25 },
    {  14,  18,  21,  24 }, {  14,  17,  20,  23 }, {  13,  16,  19,  22 }, {  12,  15,  18,  21 },
    {  12,  14,  17,  20 }, {  11,  14,  16,  19 }, {  11,  13,  15,  18 }, {  10,  12,  15,  17 },
    {  10,  12,  14,  16 }, {   9,  11,  13,  15 }, {   9,  11,  12,  14 }, {   8,  10,  12,  14 },
    {   8,   9,  11,  13 }, {   7,   9,  11,  12 }, {   7,   9,  10,  12 }, {   7,   8,  10,  11 },
    {   6,   8,   9,  11 }, {   6,   7,   9,  10 }, {   6,   7,   8,   9 }, {   2,   2,   2,   2 }
};

// Table 9-45 State transition table
static const uint8_t transIdxLPS[64] = {
     0,  0,  1,  2,  2,  4,  4,  5,
     6,  7,  8,  9,  9, 11, 11, 12,
    13, 13, 15, 15, 16, 16, 18, 18,
    19, 19, 21, 21, 22, 22, 23, 24,
    24, 25, 26, 26, 27, 27, 28, 29,
    29, 30, 30, 30, 31, 32, 32, 33,
    33, 33, 34, 34, 35, 35, 35, 36,
    36, 36, 37, 37, 37, 38, 38, 63
};

static const uint8_t transIdxMPS[64] = {
     1,  2,  3,  4,  5,  6,  7,  8,
     9, 10, 11, 12, 13, 14, 15, 16,
    17, 18, 19, 20, 21, 22, 23, 24,
    25, 26, 27, 28, 29, 30, 31, 32,
    33, 34, 35, 36, 37, 38, 39, 40,
    41, 42, 43, 44, 45, 46, 47, 48,
    49, 50, 51, 52, 53, 54, 55, 56,
    57, 58, 59, 60, 61, 62, 62, 63
};

static const uint8_t renorm_table_32[32] = {
    6, 5, 4, 4, 3, 3, 3, 3,
    2, 2, 2, 2, 2, 2, 2, 2,
    1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1
};


void cabac_engine_t::init(data_partition_t* dp)
{
    if (dp->frame_bitoffset & 7)
        dp->f(8 - (dp->frame_bitoffset & 7));

    this->dp = dp;
    this->codIRange  = 510;
    this->codIOffset = this->dp->read_bits(9);
}

bool cabac_engine_t::decode_decision(cabac_context_t* ctx)
{
    uint8_t qCodIRangeIdx = (this->codIRange >> 6) & 3;
    uint8_t codIRangeLPS  = rangeTabLPS[ctx->pStateIdx][qCodIRangeIdx];
    uint8_t binVal;

    this->codIRange -= codIRangeLPS;

    if (this->codIOffset < this->codIRange) {
        binVal = ctx->valMPS;
        ctx->pStateIdx = transIdxMPS[ctx->pStateIdx];
    } else {
        binVal = !ctx->valMPS;
        this->codIOffset -= this->codIRange;
        this->codIRange = codIRangeLPS;
        if (ctx->pStateIdx == 0)
            ctx->valMPS = 1 - ctx->valMPS; 
        ctx->pStateIdx = transIdxLPS[ctx->pStateIdx];
    }

    this->renormD();

    return binVal;
}

bool cabac_engine_t::decode_bypass()
{
    uint8_t binVal;

    this->codIOffset = (this->codIOffset << 1) | this->dp->read_bits(1);

    if (this->codIOffset < this->codIRange)
        binVal = 0;
    else {
        binVal = 1;
        this->codIOffset -= this->codIRange;
    }

    return binVal;
}

bool cabac_engine_t::decode_terminate()
{
    uint8_t binVal;

    this->codIRange -= 2;

    if (this->codIOffset < this->codIRange) {
        binVal = 0;
        this->renormD();
    } else
        binVal = 1;

    return binVal;
}

void cabac_engine_t::renormD()
{
    if (this->codIRange < 256) {
        int renorm = renorm_table_32[(this->codIRange >> 3) & 0x1F];
        this->codIRange <<= renorm;
        this->codIOffset = (this->codIOffset << renorm) | this->dp->read_bits(renorm);
    }
}

uint32_t cabac_engine_t::u(cabac_context_t* ctx, int ctx_offset)
{
    uint32_t bins = 0;
    //int bins = -1;

    if (!this->decode_decision(ctx))
        return bins;

    ctx += ctx_offset;
    for (int b = 1; b; bins++)
        b = this->decode_decision(ctx);
    return bins;
}

uint32_t cabac_engine_t::tu(cabac_context_t* ctx, int ctx_offset, unsigned int max_symbol)
{
    unsigned int bins = this->decode_decision(ctx);

    if (bins == 0 || max_symbol == 0)
        return bins;

    unsigned int l;
    ctx += ctx_offset;
    bins = 0;
    do {
        l = this->decode_decision(ctx);
        ++bins;
    } while (l != 0 && bins < max_symbol);

    if (l != 0 && bins == max_symbol)
        ++bins;
    return bins;
}


data_partition_t::data_partition_t()
{
    this->streamBuffer = new uint8_t[MAX_CODED_FRAME_SIZE];
    if (!this->streamBuffer) {
        snprintf(errortext, ET_SIZE, "AllocPartition: Memory allocation for streamBuffer failed");
        error(errortext, 100);
    }
}

data_partition_t::~data_partition_t()
{
    assert(this->streamBuffer != NULL);
    delete this->streamBuffer;
}

static int RBSPtoSODB(uint8_t* streamBuffer, int last_byte_pos)
{
    int bitoffset = 0;
    int ctr_bit   = streamBuffer[last_byte_pos - 1] & (1 << bitoffset);

    while (ctr_bit == 0) {
        ++bitoffset;
        if (bitoffset == 8) {
            if (last_byte_pos == 0)
                printf(" Panic: All zero data sequence in RBSP \n");
            assert(last_byte_pos != 0);
            --last_byte_pos;
            bitoffset = 0;
        }
        ctr_bit = streamBuffer[last_byte_pos - 1] & (1 << bitoffset);
    }

    return last_byte_pos;
}

void data_partition_t::init(nalu_t* nalu)
{
    memcpy(this->streamBuffer, &nalu->buf[1], nalu->len - 1);
    this->bitstream_length = RBSPtoSODB(this->streamBuffer, nalu->len - 1);
    this->frame_bitoffset = 0;
}


bool data_partition_t::more_rbsp_data(void)
{
    uint8_t* buffer       = this->streamBuffer;
    int      totbitoffset = this->frame_bitoffset;
    int      bytecount    = this->bitstream_length;

    int      byteoffset   = totbitoffset >> 3;

    if (byteoffset < bytecount - 1)
        return true;

    int      bitoffset = 7 - (totbitoffset & 7);
    uint8_t* cur_byte  = &buffer[byteoffset];
    int      ctr_bit   = ((*cur_byte) >> (bitoffset--)) & 1;

    if (ctr_bit == 0)
        return true;

    int cnt = 0;
    while (bitoffset >= 0 && !cnt)
        cnt |= ((*cur_byte) >> (bitoffset--)) & 1;
    return cnt ? true : false;
}

uint32_t data_partition_t::next_bits(uint8_t n)
{
    uint8_t* buffer       = this->streamBuffer;
    int      totbitoffset = this->frame_bitoffset;
    int      bitcount     = (this->bitstream_length << 3) + 7;

    if (totbitoffset + n > bitcount) 
        return 0;

    int      bitoffset  = 7 - (totbitoffset & 7);
    int      byteoffset = (totbitoffset >> 3);
    uint8_t* curbyte = &buffer[byteoffset];
    uint32_t inf = 0;

    while (n--) {
        inf <<= 1;
        inf |= ((*curbyte) >> (bitoffset--)) & 1;    
        if (bitoffset == -1) {
            curbyte++;
            bitoffset = 7;
        }
    }

    return inf;
}

uint32_t data_partition_t::read_bits(uint8_t n)
{
    uint32_t value = this->next_bits(n);
    this->frame_bitoffset += n;
    return value;
}

uint32_t data_partition_t::u(uint8_t n, const char* name)
{
    return this->read_bits(n);
}

int32_t data_partition_t::i(uint8_t n, const char* name)
{
    uint32_t value = this->read_bits(n);
    return -(value & (1 << (n - 1))) | value;
}

uint32_t data_partition_t::f(uint8_t n, const char* name)
{
    return this->read_bits(n);
}

uint32_t data_partition_t::b(uint8_t n, const char* name)
{
    return this->read_bits(n);
}

uint32_t data_partition_t::ue(const char* name)
{
    int leadingZeroBits = -1;
    uint32_t b;
    uint32_t codeNum;

    for (b = 0; !b; leadingZeroBits++)
        b = this->read_bits(1);

    codeNum = (1 << leadingZeroBits) - 1 + this->read_bits(leadingZeroBits);
    return codeNum;
}

int32_t data_partition_t::se(const char* name)
{
    uint32_t codeNum = this->ue();
    return (codeNum % 2 ? 1 : -1) * ((codeNum + 1) / 2);
}

uint32_t data_partition_t::ae(const char* name)
{
    return 0;
}

uint32_t data_partition_t::ce(const char* name)
{
    return 0;
}

uint32_t data_partition_t::me(const char* name)
{
    return 0;
}

uint32_t data_partition_t::te(const char* name)
{
    return 0;
}
