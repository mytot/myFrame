#include "frame_apps_builtin/apps.h"

#include "frame_core/app.h"
#include "frame_core/app_manager.h"

#include "esp_log.h"

#include <string.h>

static const char *TAG = "apps";

esp_err_t frame_apps_register_builtins(void)
{
    static const struct {
        const char *id;
        const char *name;
    } builtins[] = {
        { FRAME_APPS_STORE_ID,    "App Store" },
        { FRAME_APPS_SETTINGS_ID, "Settings"  },
        { FRAME_APPS_LOGS_ID,     "Logs"      },
    };

    for (size_t i = 0; i < sizeof(builtins) / sizeof(builtins[0]); i++) {
        if (frame_app_manager_find(builtins[i].id) != NULL) {
            continue;
        }
        frame_app_state_t st;
        memset(&st, 0, sizeof(st));
        strncpy(st.id, builtins[i].id, sizeof(st.id) - 1u);
        strncpy(st.name, builtins[i].name, sizeof(st.name) - 1u);
        strncpy(st.version, FRAME_SDK_VERSION, sizeof(st.version) - 1u);
        st.kind    = FRAME_APP_KIND_BUILTIN;
        st.slot    = FRAME_APP_NO_SLOT;
        st.builtin = true;
        st.healthy = true;
        esp_err_t err = frame_app_manager_upsert(&st);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "cannot register %s: %s", builtins[i].id, esp_err_to_name(err));
        }
    }
    return ESP_OK;
}

esp_err_t frame_apps_open(const char *id)
{
    if (id == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strcmp(id, FRAME_APPS_STORE_ID) == 0) {
        frame_app_store_show();
        return ESP_OK;
    }
    if (strcmp(id, FRAME_APPS_SETTINGS_ID) == 0) {
        frame_app_settings_show();
        return ESP_OK;
    }
    if (strcmp(id, FRAME_APPS_LOGS_ID) == 0) {
        frame_app_logs_show();
        return ESP_OK;
    }
    return ESP_ERR_NOT_FOUND;
}
