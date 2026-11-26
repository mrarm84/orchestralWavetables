// Lookup tables from Mutable Instruments Plaits. MIT License.
#pragma once
#include <cstdint>

namespace plaits {
extern float lut_sine[641];             // filled by InitTables()
extern const float lut_fm_frequency_quantizer[130];
extern const int16_t lut_fold[516];     // x 32000
extern const int16_t lut_fold_2[516];   // x 32000
extern const int16_t lut_ws_inverse_tan[257];
extern const int16_t lut_ws_inverse_sin[257];
extern const int16_t lut_ws_linear[257];
extern const int16_t lut_ws_bump[257];
extern const int16_t lut_ws_double_bump[257];
extern const int16_t* const lookup_table_i16_table[5];

void InitTables();
}  // namespace plaits
