(function () {
    "use strict";

    const Core = window.OrbAppCore || {};
    const $ = Core.$ || function (id) { return document.getElementById(id); };
    const setValue = Core.setValue || function () { };
    const valueFromInput = Core.valueFromInput || function (_id, fallback) { return fallback; };
    const setBlockVisible = Core.setBlockVisible || function () { };
    const postForm = Core.postForm;
    const postQueryCore = Core.postQuery;

    const STORAGE_KEY = "orb_server_tts_ui_v1";
    const DEFAULTS = {
        tts_backend: "silero",
        silero_speaker: "xenia",
        piper_default_model: "",
        yandex_endpoint: "https://tts.api.cloud.yandex.net/speech/v1/tts:synthesize",
        yandex_api_key: "",
        yandex_iam_token: "",
        yandex_folder_id: "",
        yandex_lang: "ru-RU",
        yandex_voice: "ermil",
        yandex_speed: "1.0",
        yandex_emotion: "",
        yandex_timeout_s: "20.0",
        tts_tempo_scale: "0.85",
        piper_pitch_scale: "1.0",
        tts_phrase_pause_ms: "1500",
        tts_fx_preset: "mystic",
        tts_echo_mix: "0.24",
        tts_echo_delay_ms: "220",
        tts_echo_feedback: "0.46",
        tts_reverb_mix: "0.36",
        tts_reverb_room_scale: "1.35",
        tts_reverb_damp: "0.26"
    };

    let sSetStatus = function () { };
    let sPostQuery = postQueryCore;
    let sUiReady = false;

    function getBaseUrl() {
        const el = $("server-tts-url");
        const raw = (el && el.value ? el.value : "").trim();
        if (!raw) {
            return "";
        }
        return raw.replace(/\/+$/, "");
    }

    function ensureBaseUrlOrReport(silent) {
        const base = getBaseUrl();
        if (base) {
            return base;
        }
        if (!silent) {
            setServerStatus("Set Control URL", true);
            sSetStatus("Set Server Voice Control URL first (example: http://PI_IP:8766)", true);
        }
        return "";
    }

    function getToken() {
        const el = $("server-tts-token");
        return (el && el.value ? el.value : "").trim();
    }

    function saveUiStateToStorage() {
        try {
            const payload = {
                url: getBaseUrl(),
                token: getToken()
            };
            localStorage.setItem(STORAGE_KEY, JSON.stringify(payload));
        } catch (e) {
            void e;
        }
    }

    function restoreUiStateFromStorage() {
        try {
            const raw = localStorage.getItem(STORAGE_KEY);
            if (!raw) {
                return;
            }
            const saved = JSON.parse(raw);
            if (!saved || typeof saved !== "object") {
                return;
            }
            setValue("server-tts-url", saved.url, "");
            setValue("server-tts-token", saved.token, "");
        } catch (e) {
            void e;
        }
    }

    function setServerStatus(msg, isErr) {
        const el = $("server-tts-status");
        if (!el) {
            return;
        }
        el.textContent = msg;
        el.style.color = isErr ? "#fca5a5" : "#86efac";
    }

    async function api(path, method, body) {
        const base = getBaseUrl();
        if (!base) {
            throw new Error("missing_server_url");
        }
        const hostMatch = base.match(/^https?:\/\/([^/:]+)/i);
        const host = hostMatch && hostMatch[1] ? hostMatch[1].toLowerCase() : "";
        if (host === "127.0.0.1" || host === "localhost") {
            throw new Error("invalid_target_localhost");
        }
        const token = getToken();
        const needsProxy = (window.location.protocol === "https:" && /^http:\/\//i.test(base));
        if (needsProxy) {
            let op = "";
            if ((method || "GET") === "GET" && path === "/api/tts/config") {
                op = "load";
            } else if ((method || "GET") === "GET" && path === "/api/tts/voices") {
                op = "load_voices";
            } else if ((method || "GET") === "POST" && path === "/api/tts/config") {
                op = "apply";
            } else if ((method || "GET") === "POST" && path === "/api/tts/config/save") {
                op = "save";
            } else if ((method || "GET") === "POST" && path === "/api/tts/config/reload") {
                op = "reload";
            } else {
                throw new Error("unsupported_proxy_op");
            }
            const payload = {
                op: op,
                target: base,
                token: token
            };
            if (body !== undefined) {
                payload.patch = JSON.stringify(body);
            }
            const proxied = await postForm("/api/server_tts/proxy", payload);
            if (!proxied || proxied.ok === false) {
                throw new Error((proxied && proxied.error) ? proxied.error : "proxy_error");
            }
            return proxied;
        }
        const headers = {};
        if (token) {
            headers["X-Orb-Token"] = token;
        }
        let payloadBody = undefined;
        if (body !== undefined) {
            headers["Content-Type"] = "application/json";
            payloadBody = JSON.stringify(body);
        }
        const resp = await fetch(base + path, {
            method: method || "GET",
            headers: headers,
            body: payloadBody
        });
        const text = await resp.text();
        let data = {};
        try {
            data = text ? JSON.parse(text) : {};
        } catch (e) {
            data = { ok: false, error: "invalid_json", raw: text };
        }
        if (!resp.ok || data.ok === false) {
            throw new Error(data.error || ("http_" + resp.status));
        }
        return data;
    }

    async function logPortalEvent(op, result, detail) {
        void op;
        void result;
        void detail;
    }

    function setValueIfPresent(id, cfg, key) {
        if (!cfg || !Object.prototype.hasOwnProperty.call(cfg, key)) {
            return;
        }
        const v = cfg[key];
        if (v === undefined || v === null || v === "") {
            return;
        }
        const el = $(id);
        if (!el) {
            return;
        }
        el.value = String(v);
    }

    function fillForm(cfg) {
        if (!cfg) {
            return;
        }
        setValueIfPresent("server-tts-backend", cfg, "tts_backend");
        setValueIfPresent("server-silero-speaker", cfg, "silero_speaker");
        setValueIfPresent("server-piper-default-model", cfg, "piper_default_model");
        setValueIfPresent("server-yandex-endpoint", cfg, "yandex_endpoint");
        setValueIfPresent("server-yandex-api-key", cfg, "yandex_api_key");
        setValueIfPresent("server-yandex-iam-token", cfg, "yandex_iam_token");
        setValueIfPresent("server-yandex-folder-id", cfg, "yandex_folder_id");
        setValueIfPresent("server-yandex-lang", cfg, "yandex_lang");
        setValueIfPresent("server-yandex-voice", cfg, "yandex_voice");
        setValueIfPresent("server-yandex-speed", cfg, "yandex_speed");
        setValueIfPresent("server-yandex-emotion", cfg, "yandex_emotion");
        setValueIfPresent("server-yandex-timeout-s", cfg, "yandex_timeout_s");
        setValueIfPresent("server-tts-tempo-scale", cfg, "tts_tempo_scale");
        setValueIfPresent("server-piper-pitch-scale", cfg, "piper_pitch_scale");
        setValueIfPresent("server-tts-phrase-pause-ms", cfg, "tts_phrase_pause_ms");
        setValueIfPresent("server-tts-fx-preset", cfg, "tts_fx_preset");
        setValueIfPresent("server-tts-echo-mix", cfg, "tts_echo_mix");
        setValueIfPresent("server-tts-echo-delay-ms", cfg, "tts_echo_delay_ms");
        setValueIfPresent("server-tts-echo-feedback", cfg, "tts_echo_feedback");
        setValueIfPresent("server-tts-reverb-mix", cfg, "tts_reverb_mix");
        setValueIfPresent("server-tts-reverb-room-scale", cfg, "tts_reverb_room_scale");
        setValueIfPresent("server-tts-reverb-damp", cfg, "tts_reverb_damp");
        updateBackendVisibility();
        saveUiStateToStorage();
    }

    function applyDefaultsToForm() {
        setValue("server-tts-backend", DEFAULTS.tts_backend, DEFAULTS.tts_backend);
        setValue("server-silero-speaker", DEFAULTS.silero_speaker, DEFAULTS.silero_speaker);
        setValue("server-piper-default-model", DEFAULTS.piper_default_model, DEFAULTS.piper_default_model);
        setValue("server-yandex-endpoint", DEFAULTS.yandex_endpoint, DEFAULTS.yandex_endpoint);
        setValue("server-yandex-api-key", DEFAULTS.yandex_api_key, DEFAULTS.yandex_api_key);
        setValue("server-yandex-iam-token", DEFAULTS.yandex_iam_token, DEFAULTS.yandex_iam_token);
        setValue("server-yandex-folder-id", DEFAULTS.yandex_folder_id, DEFAULTS.yandex_folder_id);
        setValue("server-yandex-lang", DEFAULTS.yandex_lang, DEFAULTS.yandex_lang);
        setValue("server-yandex-voice", DEFAULTS.yandex_voice, DEFAULTS.yandex_voice);
        setValue("server-yandex-speed", DEFAULTS.yandex_speed, DEFAULTS.yandex_speed);
        setValue("server-yandex-emotion", DEFAULTS.yandex_emotion, DEFAULTS.yandex_emotion);
        setValue("server-yandex-timeout-s", DEFAULTS.yandex_timeout_s, DEFAULTS.yandex_timeout_s);
        setValue("server-tts-tempo-scale", DEFAULTS.tts_tempo_scale, DEFAULTS.tts_tempo_scale);
        setValue("server-piper-pitch-scale", DEFAULTS.piper_pitch_scale, DEFAULTS.piper_pitch_scale);
        setValue("server-tts-phrase-pause-ms", DEFAULTS.tts_phrase_pause_ms, DEFAULTS.tts_phrase_pause_ms);
        setValue("server-tts-fx-preset", DEFAULTS.tts_fx_preset, DEFAULTS.tts_fx_preset);
        setValue("server-tts-echo-mix", DEFAULTS.tts_echo_mix, DEFAULTS.tts_echo_mix);
        setValue("server-tts-echo-delay-ms", DEFAULTS.tts_echo_delay_ms, DEFAULTS.tts_echo_delay_ms);
        setValue("server-tts-echo-feedback", DEFAULTS.tts_echo_feedback, DEFAULTS.tts_echo_feedback);
        setValue("server-tts-reverb-mix", DEFAULTS.tts_reverb_mix, DEFAULTS.tts_reverb_mix);
        setValue("server-tts-reverb-room-scale", DEFAULTS.tts_reverb_room_scale, DEFAULTS.tts_reverb_room_scale);
        setValue("server-tts-reverb-damp", DEFAULTS.tts_reverb_damp, DEFAULTS.tts_reverb_damp);
        updateBackendVisibility();
        saveUiStateToStorage();
    }

    function looksLikeConfig(obj) {
        if (!obj || typeof obj !== "object") {
            return false;
        }
        return !!(
            Object.prototype.hasOwnProperty.call(obj, "tts_backend") ||
            Object.prototype.hasOwnProperty.call(obj, "silero_speaker") ||
            Object.prototype.hasOwnProperty.call(obj, "piper_default_model") ||
            Object.prototype.hasOwnProperty.call(obj, "tts_tempo_scale") ||
            Object.prototype.hasOwnProperty.call(obj, "piper_pitch_scale")
        );
    }

    function normalizeConfig(data) {
        const maxDepth = 6;
        const visit = (node, depth) => {
            if (!node || depth > maxDepth) {
                return {};
            }
            if (looksLikeConfig(node)) {
                return node;
            }
            if (typeof node !== "object") {
                return {};
            }
            if (node.config && typeof node.config === "object") {
                const found = visit(node.config, depth + 1);
                if (looksLikeConfig(found)) {
                    return found;
                }
            }
            if (node.data && typeof node.data === "object") {
                const found = visit(node.data, depth + 1);
                if (looksLikeConfig(found)) {
                    return found;
                }
            }
            const vals = Object.values(node);
            for (let i = 0; i < vals.length; i += 1) {
                const found = visit(vals[i], depth + 1);
                if (looksLikeConfig(found)) {
                    return found;
                }
            }
            return {};
        };
        return visit(data, 0);
    }

    function hasConfig(cfg) {
        return looksLikeConfig(cfg);
    }

    function safeCfgValue(cfg, key) {
        if (!cfg || typeof cfg !== "object") {
            return "-";
        }
        const v = cfg[key];
        if (v === undefined || v === null || v === "") {
            return "-";
        }
        return String(v);
    }

    function updateBackendVisibility() {
        const backend = valueFromInput("server-tts-backend", DEFAULTS.tts_backend).trim().toLowerCase();
        const isSilero = backend === "silero";
        const isPiper = backend === "piper";
        const isYandex = backend === "yandex";
        const isLocal = isSilero || isPiper;

        setBlockVisible("server-tts-row-silero", isSilero);
        setBlockVisible("server-tts-row-piper", isPiper);
        setBlockVisible("server-tts-row-local-tools", isLocal);

        setBlockVisible("server-tts-row-yandex-1", isYandex);
        setBlockVisible("server-tts-row-yandex-2", isYandex);
        setBlockVisible("server-tts-row-yandex-3", isYandex);
        setBlockVisible("server-tts-row-yandex-4", isYandex);
        setBlockVisible("server-tts-row-yandex-5", isYandex);
    }

    function setSelectOptions(id, options, selected, includeAuto) {
        const el = $(id);
        if (!el) {
            return;
        }
        const current = (selected !== undefined && selected !== null) ? String(selected) : "";
        const unique = [];
        const seen = new Set();
        (options || []).forEach((v) => {
            const s = String(v || "").trim();
            if (!s || seen.has(s)) {
                return;
            }
            seen.add(s);
            unique.push(s);
        });
        if (current && !seen.has(current)) {
            unique.unshift(current);
        }
        const keep = current || (el.value ? String(el.value) : "");
        el.innerHTML = "";
        if (includeAuto) {
            const o = document.createElement("option");
            o.value = "";
            o.textContent = "(auto)";
            el.appendChild(o);
        }
        unique.forEach((v) => {
            const o = document.createElement("option");
            o.value = v;
            o.textContent = v;
            el.appendChild(o);
        });
        if (keep !== "") {
            el.value = keep;
        }
    }

    function normalizeVoicesPayload(data) {
        if (!data || typeof data !== "object") {
            return {};
        }
        if (data.voices && typeof data.voices === "object") {
            return data.voices;
        }
        if (data.data && data.data.voices && typeof data.data.voices === "object") {
            return data.data.voices;
        }
        return {};
    }

    async function loadVoices(options) {
        const silent = !!(options && options.silent);
        if (!ensureBaseUrlOrReport(silent)) {
            return {};
        }
        const selectedBackend = valueFromInput("server-tts-backend", DEFAULTS.tts_backend);
        try {
            const data = await api("/api/tts/voices", "GET");
            const voices = normalizeVoicesPayload(data);
            const silero = voices.silero || {};
            const piper = voices.piper || {};
            setSelectOptions(
                "server-silero-speaker",
                silero.speakers || [],
                valueFromInput("server-silero-speaker", silero.selected || DEFAULTS.silero_speaker),
                false
            );
            setSelectOptions(
                "server-piper-default-model",
                piper.models || [],
                valueFromInput("server-piper-default-model", piper.selected || DEFAULTS.piper_default_model),
                true
            );
            const sN = Array.isArray(silero.speakers) ? silero.speakers.length : 0;
            const pN = Array.isArray(piper.models) ? piper.models.length : 0;
            if (!silent) {
                sSetStatus("Voices loaded (silero=" + sN + ", piper=" + pN + ", backend=" + selectedBackend + ")", false);
            }
            return voices;
        } catch (e) {
            if (!silent) {
                sSetStatus("Voices load failed: " + e.message, true);
            }
            throw e;
        }
    }

    async function loadConfigFromServer(options) {
        const silent = !!(options && options.silent);
        const op = silent ? "server_tts_autoload" : "server_tts_load";
        if (!ensureBaseUrlOrReport(silent)) {
            if (!silent) {
                await logPortalEvent(op, "error", "missing_server_url");
            }
            return null;
        }
        await logPortalEvent(op, "start", "-");
        try {
            saveUiStateToStorage();
            const data = await api("/api/tts/config", "GET");
            const cfg = normalizeConfig(data);
            if (!hasConfig(cfg)) {
                throw new Error("invalid_config_payload");
            }
            fillForm(cfg);
            await loadVoices({ silent: true }).catch(() => { });
            setServerStatus("Loaded", false);
            const tempoShow = valueFromInput("server-tts-tempo-scale", safeCfgValue(cfg, "tts_tempo_scale"));
            const pitchShow = valueFromInput("server-piper-pitch-scale", safeCfgValue(cfg, "piper_pitch_scale"));
            sSetStatus(
                "Server TTS loaded (tempo=" + tempoShow +
                ", pitch=" + pitchShow + ")",
                false
            );
            await logPortalEvent(
                op,
                "ok",
                "tempo=" + tempoShow + ",pitch=" + pitchShow
            );
            return data;
        } catch (e) {
            if (!silent) {
                setServerStatus("Load failed", true);
                sSetStatus("Server TTS load failed: " + e.message, true);
            }
            await logPortalEvent(op, "error", e && e.message ? e.message : "unknown_error");
            throw e;
        }
    }

    function bind(options) {
        const opts = options || {};
        sSetStatus = opts.setStatus || function () { };
        sPostQuery = opts.postQuery || postQueryCore;
        sUiReady = !!$("server-tts-url");
        if (!sUiReady) {
            return;
        }

        restoreUiStateFromStorage();
        updateBackendVisibility();

        const loadBtn = $("server-tts-load-btn");
        const applySaveBtn = $("server-tts-apply-save-btn");
        const resetBtn = $("server-tts-reset-btn");
        const voicesRefreshBtn = $("server-tts-voices-refresh-btn");
        const backendSel = $("server-tts-backend");

        if (loadBtn) {
            loadBtn.addEventListener("click", async () => {
                try {
                    await loadConfigFromServer({ silent: false });
                } catch (e) {
                    void e;
                }
            });
        }

        if (resetBtn) {
            resetBtn.addEventListener("click", async () => {
                applyDefaultsToForm();
                await loadVoices({ silent: true }).catch(() => { });
                setServerStatus("Defaults loaded", false);
                sSetStatus("Server TTS defaults applied in UI", false);
                await logPortalEvent("server_tts_reset_ui", "ok", "-");
            });
        }

        if (voicesRefreshBtn) {
            voicesRefreshBtn.addEventListener("click", async () => {
                try {
                    await loadVoices({ silent: false });
                    await logPortalEvent("server_tts_voices_refresh", "ok", "-");
                } catch (e) {
                    await logPortalEvent("server_tts_voices_refresh", "error", e && e.message ? e.message : "unknown_error");
                }
            });
        }

        if (backendSel) {
            backendSel.addEventListener("change", () => {
                updateBackendVisibility();
                void loadVoices({ silent: true });
            });
        }

        if (applySaveBtn) {
            applySaveBtn.addEventListener("click", async () => {
                const num = (id, fallback) => {
                    const el = $(id);
                    const raw = el && el.value !== "" ? el.value : String(fallback);
                    const v = Number(raw);
                    if (!Number.isFinite(v)) {
                        throw new Error("invalid_number_" + id);
                    }
                    return v;
                };
                if (!ensureBaseUrlOrReport(false)) {
                    await logPortalEvent("server_tts_apply_save", "error", "missing_server_url");
                    return;
                }
                await logPortalEvent("server_tts_apply_save", "start", "-");
                try {
                    saveUiStateToStorage();
                    const patch = {
                        tts_backend: ($("server-tts-backend") && $("server-tts-backend").value) || DEFAULTS.tts_backend,
                        silero_speaker: ($("server-silero-speaker") && $("server-silero-speaker").value || "").trim(),
                        piper_default_model: ($("server-piper-default-model") && $("server-piper-default-model").value || "").trim(),
                        yandex_endpoint: ($("server-yandex-endpoint") && $("server-yandex-endpoint").value || "").trim(),
                        yandex_api_key: ($("server-yandex-api-key") && $("server-yandex-api-key").value || "").trim(),
                        yandex_iam_token: ($("server-yandex-iam-token") && $("server-yandex-iam-token").value || "").trim(),
                        yandex_folder_id: ($("server-yandex-folder-id") && $("server-yandex-folder-id").value || "").trim(),
                        yandex_lang: ($("server-yandex-lang") && $("server-yandex-lang").value || "").trim(),
                        yandex_voice: ($("server-yandex-voice") && $("server-yandex-voice").value || "").trim(),
                        yandex_speed: num("server-yandex-speed", Number(DEFAULTS.yandex_speed)),
                        yandex_emotion: ($("server-yandex-emotion") && $("server-yandex-emotion").value || "").trim(),
                        yandex_timeout_s: num("server-yandex-timeout-s", Number(DEFAULTS.yandex_timeout_s)),
                        tts_tempo_scale: num("server-tts-tempo-scale", Number(DEFAULTS.tts_tempo_scale)),
                        piper_pitch_scale: num("server-piper-pitch-scale", Number(DEFAULTS.piper_pitch_scale)),
                        tts_phrase_pause_ms: Math.round(num("server-tts-phrase-pause-ms", Number(DEFAULTS.tts_phrase_pause_ms))),
                        tts_fx_preset: ($("server-tts-fx-preset") && $("server-tts-fx-preset").value) || DEFAULTS.tts_fx_preset,
                        tts_echo_mix: num("server-tts-echo-mix", Number(DEFAULTS.tts_echo_mix)),
                        tts_echo_delay_ms: Math.round(num("server-tts-echo-delay-ms", Number(DEFAULTS.tts_echo_delay_ms))),
                        tts_echo_feedback: num("server-tts-echo-feedback", Number(DEFAULTS.tts_echo_feedback)),
                        tts_reverb_mix: num("server-tts-reverb-mix", Number(DEFAULTS.tts_reverb_mix)),
                        tts_reverb_room_scale: num("server-tts-reverb-room-scale", Number(DEFAULTS.tts_reverb_room_scale)),
                        tts_reverb_damp: num("server-tts-reverb-damp", Number(DEFAULTS.tts_reverb_damp)),
                    };
                    const data = await api("/api/tts/config", "POST", patch);
                    await api("/api/tts/config/save", "POST", {});
                    const cfg = normalizeConfig(data);
                    if (!hasConfig(cfg)) {
                        throw new Error("invalid_config_payload");
                    }
                    fillForm(cfg);
                    await loadVoices({ silent: true }).catch(() => { });
                    setServerStatus("Applied + Saved", false);
                    const tempoShow = valueFromInput("server-tts-tempo-scale", safeCfgValue(cfg, "tts_tempo_scale"));
                    const pitchShow = valueFromInput("server-piper-pitch-scale", safeCfgValue(cfg, "piper_pitch_scale"));
                    sSetStatus(
                        "Server TTS applied+saved (tempo=" + tempoShow +
                        ", pitch=" + pitchShow + ")",
                        false
                    );
                    await logPortalEvent("server_tts_apply_save", "ok", "keys=" + Object.keys(patch).length);
                } catch (e) {
                    setServerStatus("Apply+Save failed", true);
                    sSetStatus("Server TTS apply+save failed: " + e.message, true);
                    await logPortalEvent("server_tts_apply_save", "error", e && e.message ? e.message : "unknown_error");
                }
            });
        }

        setTimeout(() => {
            updateBackendVisibility();
            loadConfigFromServer({ silent: true }).catch(() => { });
            loadVoices({ silent: true }).catch(() => { });
        }, 150);
    }

    window.OrbServerTts = {
        defaults: DEFAULTS,
        bind: bind,
        loadConfigFromServer: loadConfigFromServer,
        loadVoices: loadVoices
    };
})();
