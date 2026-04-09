set(ORB_WEB_CERT_DIR "${CMAKE_CURRENT_LIST_DIR}/certs")
set(ORB_WEB_CERT_FILE "${ORB_WEB_CERT_DIR}/servercert.pem")
set(ORB_WEB_KEY_FILE "${ORB_WEB_CERT_DIR}/prvtkey.pem")
set(ORB_WEB_CERT_GEN_SCRIPT "${CMAKE_SOURCE_DIR}/kws/scripts/gen_web_tls_cert.py")

if(NOT EXISTS "${ORB_WEB_CERT_FILE}" OR NOT EXISTS "${ORB_WEB_KEY_FILE}")
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND AND EXISTS "${ORB_WEB_CERT_GEN_SCRIPT}")
        execute_process(
            COMMAND "${Python3_EXECUTABLE}" "${ORB_WEB_CERT_GEN_SCRIPT}" --cert-dir "${ORB_WEB_CERT_DIR}"
            RESULT_VARIABLE ORB_WEB_CERT_GEN_RESULT
            OUTPUT_VARIABLE ORB_WEB_CERT_GEN_STDOUT
            ERROR_VARIABLE ORB_WEB_CERT_GEN_STDERR
        )
        if(NOT ORB_WEB_CERT_GEN_RESULT EQUAL 0)
            message(WARNING "TLS cert/key auto-generation failed (${ORB_WEB_CERT_GEN_RESULT})")
            message(WARNING "${ORB_WEB_CERT_GEN_STDOUT}")
            message(WARNING "${ORB_WEB_CERT_GEN_STDERR}")
        endif()
    endif()
endif()

if(NOT EXISTS "${ORB_WEB_CERT_FILE}" OR NOT EXISTS "${ORB_WEB_KEY_FILE}")
    message(FATAL_ERROR
        "Missing HTTPS cert/key for services_web: ${ORB_WEB_CERT_FILE}, ${ORB_WEB_KEY_FILE}. "
        "Generate them with: python kws/scripts/gen_web_tls_cert.py --cert-dir components/services_web/certs")
endif()

idf_component_register(
    SRCS
        "web/web_server.c"
        "web/web_portal.c"
        "api/core/rest_api.c"
        "api/core/rest_api_common.c"
        "api/core/rest_api_core.c"
        "api/endpoints/rest_api_mode.c"
        "api/endpoints/rest_api_led.c"
        "api/endpoints/rest_api_audio.c"
        "api/endpoints/rest_api_config.c"
        "api/endpoints/rest_api_network.c"
        "api/endpoints/rest_api_offline.c"
        "api/endpoints/rest_api_server_tts.c"
        "api/talk/rest_api_talk.c"
        "api/talk/rest_api_talk_common.c"
        "api/talk/rest_api_talk_say.c"
        "api/talk/rest_api_talk_live_buffer.c"
        "api/talk/rest_api_talk_live_postfx.c"
        "api/talk/rest_api_talk_live_runtime.c"
        "api/talk/rest_api_talk_live_ring.c"
        "api/talk/rest_api_talk_ws.c"
    INCLUDE_DIRS
        "include"
    PRIV_INCLUDE_DIRS
        "."
        "web"
        "api/core"
        "api/endpoints"
        "api/talk"
    REQUIRES
        app_core
        common
        freertos
        services_config
        services_network
        esp_http_server
        esp_https_server
        esp_http_client
        esp_timer
    EMBED_TXTFILES
        "${ORB_WEB_CERT_FILE}"
        "${ORB_WEB_KEY_FILE}"
        "portal/pages/offline.html"
        "portal/pages/hybrid.html"
        "portal/pages/installation.html"
        "portal/assets/css/app.css"
        "portal/assets/js/core/dom_http.js"
        "portal/assets/js/features/hybrid_effects.js"
        "portal/assets/js/features/talk.js"
        "portal/assets/js/features/server_tts.js"
        "portal/assets/js/pages/page_offline.js"
        "portal/assets/js/pages/network_setup_page.js"
        "portal/assets/js/pages/page_mode_generic.js"
        "portal/assets/js/app.js"
)
