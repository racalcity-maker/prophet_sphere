#include "prophecy_common.h"

#include "esp_random.h"

static const char *s_archetype_names[PROPHECY_ARCHETYPE_COUNT] = {
    "choice", "danger", "future", "inner_state", "love", "path", "money", "wish", "yes_no", "timing",
};

static const char *s_phase_names[PROPHECY_PHASE_COUNT] = {
    "greeting", "understanding", "prediction", "farewell",
};

static const uint32_t s_assets[PROPHECY_ARCHETYPE_COUNT][PROPHECY_PHASE_COUNT] = {
    { 1201U, 1202U, 1203U, 1204U }, /* choice */
    { 1205U, 1206U, 1207U, 1208U }, /* danger */
    { 1209U, 1210U, 1211U, 1212U }, /* future */
    { 1213U, 1214U, 1215U, 1216U }, /* inner_state */
    { 1217U, 1218U, 1219U, 1220U }, /* love */
    { 1221U, 1222U, 1223U, 1224U }, /* path */
    { 1225U, 1226U, 1227U, 1228U }, /* money */
    { 1229U, 1230U, 1231U, 1232U }, /* wish */
    { 1233U, 1234U, 1235U, 1236U }, /* yes_no */
    { 1237U, 1238U, 1239U, 1240U }, /* timing */
};

const char *prophecy_archetype_name(prophecy_archetype_t archetype)
{
    if ((unsigned)archetype >= (unsigned)PROPHECY_ARCHETYPE_COUNT) {
        return "unknown";
    }
    return s_archetype_names[(unsigned)archetype];
}

const char *prophecy_phase_name(prophecy_phase_t phase)
{
    if ((unsigned)phase >= (unsigned)PROPHECY_PHASE_COUNT) {
        return "unknown";
    }
    return s_phase_names[(unsigned)phase];
}

prophecy_archetype_t prophecy_random_archetype(void)
{
    uint32_t v = esp_random() % (uint32_t)PROPHECY_ARCHETYPE_COUNT;
    return (prophecy_archetype_t)v;
}

uint32_t prophecy_asset_for(prophecy_archetype_t archetype, prophecy_phase_t phase)
{
    if ((unsigned)archetype >= (unsigned)PROPHECY_ARCHETYPE_COUNT) {
        return 0U;
    }
    if ((unsigned)phase >= (unsigned)PROPHECY_PHASE_COUNT) {
        return 0U;
    }
    return s_assets[(unsigned)archetype][(unsigned)phase];
}

bool prophecy_phase_next(prophecy_phase_t current_phase, prophecy_phase_t *out_next_phase)
{
    if ((unsigned)current_phase >= (unsigned)PROPHECY_PHASE_COUNT) {
        return false;
    }
    if (current_phase == PROPHECY_PHASE_FAREWELL) {
        return false;
    }

    if (out_next_phase != NULL) {
        *out_next_phase = (prophecy_phase_t)((unsigned)current_phase + 1U);
    }
    return true;
}

bool prophecy_phase_advance(prophecy_archetype_t archetype,
                            prophecy_phase_t current_phase,
                            prophecy_phase_t *out_next_phase,
                            uint32_t *out_next_asset_id)
{
    prophecy_phase_t next_phase = PROPHECY_PHASE_GREET;
    if (!prophecy_phase_next(current_phase, &next_phase)) {
        return false;
    }

    uint32_t asset_id = prophecy_asset_for(archetype, next_phase);
    if (asset_id == 0U) {
        return false;
    }

    if (out_next_phase != NULL) {
        *out_next_phase = next_phase;
    }
    if (out_next_asset_id != NULL) {
        *out_next_asset_id = asset_id;
    }
    return true;
}

bool prophecy_asset_to_indices(uint32_t asset_id, uint8_t *out_archetype, uint8_t *out_phase)
{
    const uint32_t first = 1201U;
    const uint32_t last = 1240U;
    if (asset_id < first || asset_id > last) {
        return false;
    }

    uint32_t idx = asset_id - first;
    uint32_t archetype = idx / (uint32_t)PROPHECY_PHASE_COUNT;
    uint32_t phase = idx % (uint32_t)PROPHECY_PHASE_COUNT;
    if (archetype >= (uint32_t)PROPHECY_ARCHETYPE_COUNT || phase >= (uint32_t)PROPHECY_PHASE_COUNT) {
        return false;
    }

    if (out_archetype != NULL) {
        *out_archetype = (uint8_t)archetype;
    }
    if (out_phase != NULL) {
        *out_phase = (uint8_t)phase;
    }
    return true;
}
