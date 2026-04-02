#include "orb_intents.h"

#include <ctype.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

typedef struct {
    const char *name;
    orb_intent_id_t id;
} intent_map_entry_t;

static const intent_map_entry_t s_intent_map[] = {
    { "unknown", ORB_INTENT_UNKNOWN },
    { "love", ORB_INTENT_LOVE },
    { "future", ORB_INTENT_FUTURE },
    { "choice", ORB_INTENT_CHOICE },
    { "money", ORB_INTENT_MONEY },
    { "path", ORB_INTENT_PATH },
    { "danger", ORB_INTENT_DANGER },
    { "inner_state", ORB_INTENT_INNER_STATE },
    { "wish", ORB_INTENT_WISH },
    { "yes_no", ORB_INTENT_YES_NO },
    { "past", ORB_INTENT_PAST },
    { "time", ORB_INTENT_TIME },
    { "place", ORB_INTENT_PLACE },
    { "uncertain", ORB_INTENT_UNCERTAIN },
    { "joke", ORB_INTENT_JOKE },
    { "forbidden", ORB_INTENT_FORBIDDEN },
};

static bool ascii_ieq(const char *a, const char *b)
{
    if (a == NULL || b == NULL) {
        return false;
    }
    while (*a != '\0' && *b != '\0') {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) {
            return false;
        }
        a++;
        b++;
    }
    return (*a == '\0' && *b == '\0');
}

const char *orb_intent_name(orb_intent_id_t id)
{
    for (size_t i = 0; i < (sizeof(s_intent_map) / sizeof(s_intent_map[0])); ++i) {
        if (s_intent_map[i].id == id) {
            return s_intent_map[i].name;
        }
    }
    return "unknown";
}

orb_intent_id_t orb_intent_from_string(const char *intent_str)
{
    if (intent_str == NULL || intent_str[0] == '\0') {
        return ORB_INTENT_UNKNOWN;
    }

    char norm[48];
    size_t i = 0U;
    while (intent_str[i] != '\0' && i < (sizeof(norm) - 1U)) {
        char c = (char)tolower((unsigned char)intent_str[i]);
        if (c == '-' || c == ' ') {
            c = '_';
        }
        norm[i] = c;
        i++;
    }
    norm[i] = '\0';

    /* Accept normalized variants commonly seen in external payloads. */
    if (ascii_ieq(norm, "yesno")) {
        return ORB_INTENT_YES_NO;
    }
    if (ascii_ieq(norm, "innerstate")) {
        return ORB_INTENT_INNER_STATE;
    }
    if (ascii_ieq(norm, "where")) {
        return ORB_INTENT_PLACE;
    }

    for (size_t idx = 0; idx < (sizeof(s_intent_map) / sizeof(s_intent_map[0])); ++idx) {
        if (ascii_ieq(norm, s_intent_map[idx].name)) {
            return s_intent_map[idx].id;
        }
    }

    return ORB_INTENT_UNKNOWN;
}
