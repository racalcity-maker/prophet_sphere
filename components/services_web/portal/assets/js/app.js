(function () {
    "use strict";
    var sStarted = false;

    function $(id) {
        return document.getElementById(id);
    }

    async function api(path, method) {
        const resp = await fetch(path, { method: method || "GET" });
        const text = await resp.text();
        let data = {};
        try {
            data = text ? JSON.parse(text) : {};
        } catch (e) {
            data = { ok: false, error: "invalid_json", raw: text };
        }
        if (!resp.ok) {
            throw new Error(data.error || ("http_" + resp.status));
        }
        return data;
    }

    async function postQuery(path, params) {
        const q = new URLSearchParams();
        Object.keys(params || {}).forEach((k) => {
            const v = params[k];
            if (v === undefined || v === null || v === "") {
                return;
            }
            q.set(k, String(v));
        });
        const suffix = q.toString();
        return api(path + (suffix ? ("?" + suffix) : ""), "POST");
    }

    async function postForm(path, params) {
        const q = new URLSearchParams();
        Object.keys(params || {}).forEach((k) => {
            const v = params[k];
            if (v === undefined || v === null || v === "") {
                return;
            }
            q.set(k, String(v));
        });
        const resp = await fetch(path, {
            method: "POST",
            headers: {
                "Content-Type": "application/x-www-form-urlencoded"
            },
            body: q.toString()
        });
        const text = await resp.text();
        let data = {};
        try {
            data = text ? JSON.parse(text) : {};
        } catch (e) {
            data = { ok: false, error: "invalid_json", raw: text };
        }
        if (!resp.ok) {
            throw new Error(data.error || ("http_" + resp.status));
        }
        return data;
    }

    function getServerTtsBaseUrl() {
        const el = $("server-tts-url");
        const raw = (el && el.value ? el.value : "").trim();
        if (!raw) {
            return "";
        }
        return raw.replace(/\/+$/, "");
    }

    function getServerTtsToken() {
        const el = $("server-tts-token");
        return (el && el.value ? el.value : "").trim();
    }

    function setServerTtsStatus(msg, isErr) {
        const el = $("server-tts-status");
        if (!el) {
            return;
        }
        el.textContent = msg;
        el.style.color = isErr ? "#fca5a5" : "#86efac";
    }

    async function serverTtsApi(path, method, body) {
        const base = getServerTtsBaseUrl();
        if (!base) {
            throw new Error("missing_server_url");
        }
        const token = getServerTtsToken();
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

    function modeToPage(mode) {
        if (mode === "hybrid_ai") {
            return "hybrid";
        }
        if (mode === "installation_slave") {
            return "installation";
        }
        return "offline";
    }

    function setStatus(msg, isErr) {
        const el = $("status");
        if (!el) {
            return;
        }
        el.textContent = msg;
        el.style.color = isErr ? "#fca5a5" : "#86efac";
    }

    function setText(id, value, fallback) {
        const el = $(id);
        if (!el) {
            return;
        }
        if (value === undefined || value === null || value === "") {
            el.textContent = String(fallback);
            return;
        }
        el.textContent = String(value);
    }

    function setValue(id, value, fallback) {
        const el = $(id);
        if (!el) {
            return;
        }
        if (value === undefined || value === null || value === "") {
            el.value = String(fallback);
            return;
        }
        el.value = String(value);
    }

    function setChecked(id, checked) {
        const el = $(id);
        if (!el) {
            return;
        }
        el.checked = !!checked;
    }

    function ensureModePageAllowed(page, mode) {
        if (page !== "offline" && page !== "hybrid" && page !== "installation") {
            return;
        }
        const expected = modeToPage(mode);
        if (expected !== page) {
            window.location.replace("/" + expected);
        }
    }

    function bindTabButtons(buttonSelector, panelSelector, buttonAttr, panelAttr, onChanged) {
        const buttons = Array.from(document.querySelectorAll(buttonSelector));
        const panels = Array.from(document.querySelectorAll(panelSelector));
        if (buttons.length === 0 || panels.length === 0) {
            return function () {};
        }
        function activate(name) {
            if (!name) {
                return;
            }
            buttons.forEach((btn) => {
                btn.classList.toggle("active", btn.getAttribute(buttonAttr) === name);
            });
            panels.forEach((panel) => {
                panel.classList.toggle("active", panel.getAttribute(panelAttr) === name);
            });
            if (typeof onChanged === "function") {
                onChanged(name);
            }
        }
        buttons.forEach((btn) => {
            btn.addEventListener("click", () => activate(btn.getAttribute(buttonAttr)));
        });
        activate(buttons[0].getAttribute(buttonAttr));
        return activate;
    }

    function renderModeStatus(status) {
        setText("mode-name", status.mode, "-");
        setText("mode-fsm", status.fsm, "-");
        setText("mode-session", status.session && status.session.state ? status.session.state : "IDLE", "IDLE");
    }

    function renderOfflineState(state) {
        setText("mode-submode", state.submode, "aura");
        setText("mode-aura-color", state.aura ? state.aura.selected_color : "-", "-");
        setText("home-session-state", state.session && state.session.state ? state.session.state : "IDLE", "IDLE");
        setText("home-submode-state", state.submode || "aura", "aura");
        setText("home-aura-color", state.aura ? state.aura.selected_color : "-", "-");
        setText("home-lottery-seq", state.lottery ? state.lottery.start_seq : 0, 0);

        setValue("offline-fg-volume", state.audio ? state.audio.fg_volume : 60, 60);
        setValue("offline-submode-select", state.submode || "aura", "aura");
        if (state.aura) {
            setValue("offline-aura-gap-ms", state.aura.gap_ms, 1200);
            setValue("offline-aura-intro-dir", state.aura.intro_dir, "/sdcard/audio/aura/intro");
            setValue("offline-aura-response-dir", state.aura.response_dir, "/sdcard/audio/aura/response");
            setValue("offline-aura-selected-color", state.aura.selected_color, "");
        }

        if (state.prophecy) {
            setValue("offline-prophecy-gap12", state.prophecy.gap12_ms, 800);
            setValue("offline-prophecy-gap23", state.prophecy.gap23_ms, 800);
            setValue("offline-prophecy-gap34", state.prophecy.gap34_ms, 2000);
            setValue("offline-prophecy-leadin", state.prophecy.leadin_wait_ms, 1000);
        }
        if (state.audio) {
            setValue("offline-prophecy-bg-gain", state.audio.bg_gain_permille, 260);
            setValue("offline-prophecy-bg-fade-in", state.audio.bg_fade_in_ms, 2000);
            setValue("offline-prophecy-bg-fade-out", state.audio.bg_fade_out_ms, 4000);
            setValue("offline-prophecy-bg-path", state.audio.bg_path, "");
        }

    }

    async function refreshOfflineAll() {
        const pair = await Promise.all([api("/api/status"), api("/api/offline/state")]);
        const status = pair[0];
        const state = pair[1];
        ensureModePageAllowed("offline", status.mode);
        renderModeStatus(status);
        renderOfflineState(state);
        return { status: status, state: state };
    }

    async function refreshModeGeneric(page) {
        const pair = await Promise.all([api("/api/status"), api("/api/config")]);
        const status = pair[0];
        const cfg = pair[1];
        ensureModePageAllowed(page, status.mode);
        renderModeStatus(status);
        return { status: status, config: cfg };
    }

    async function refreshNetworkSetupState(pageName) {
        const pair = await Promise.all([api("/api/status"), api("/api/network/status"), api("/api/config")]);
        const status = pair[0];
        const net = pair[1];
        const cfg = pair[2];
        ensureModePageAllowed(pageName, status.mode);
        renderModeStatus(status);

        setValue("inst-wifi-ssid", net.sta_ssid || "", "");
        setText("inst-net-up", net.up ? "true" : "false", "false");
        setText("inst-net-active", net.active || "-", "-");
        setText("inst-net-sta-ip", net.sta_ip || "-", "-");
        setText("inst-net-ap-ip", net.ap_ip || "-", "-");
        setText("inst-net-link", net.link_state || "-", "-");
        setValue("mode-led-brightness-range", cfg.brightness, 160);
        setValue("mode-led-brightness-value", cfg.brightness, 160);
        setText("mode-led-brightness-current", cfg.brightness, "-");
        setValue("mode-audio-volume-range", cfg.volume, 60);
        setValue("mode-audio-volume-value", cfg.volume, 60);
        setText("mode-audio-volume-current", cfg.volume, "-");
        setValue("mode-hybrid-mic-capture-ms", cfg.hybrid_mic_capture_ms, 8000);
        setValue("mode-hybrid-reject-threshold", cfg.hybrid_reject_threshold_permille, 380);
        setText(
            "mode-hybrid-params-current",
            (cfg.hybrid_mic_capture_ms !== undefined && cfg.hybrid_reject_threshold_permille !== undefined)
                ? (String(cfg.hybrid_mic_capture_ms) + " ms / " + String(cfg.hybrid_reject_threshold_permille))
                : "-",
            "-"
        );
        return { status: status, network: net, config: cfg };
    }

    function fillServerTtsForm(cfg) {
        if (!cfg) {
            return;
        }
        setValue("server-tts-backend", cfg.tts_backend, "silero");
        setValue("server-silero-speaker", cfg.silero_speaker, "xenia");
        setValue("server-tts-tempo-scale", cfg.tts_tempo_scale, 0.85);
        setValue("server-piper-pitch-scale", cfg.piper_pitch_scale, 1.0);
        setValue("server-tts-phrase-pause-ms", cfg.tts_phrase_pause_ms, 1500);
        setValue("server-tts-fx-preset", cfg.tts_fx_preset, "mystic");
        setValue("server-tts-echo-mix", cfg.tts_echo_mix, 0.24);
        setValue("server-tts-echo-delay-ms", cfg.tts_echo_delay_ms, 220);
        setValue("server-tts-echo-feedback", cfg.tts_echo_feedback, 0.46);
        setValue("server-tts-reverb-mix", cfg.tts_reverb_mix, 0.36);
        setValue("server-tts-reverb-room-scale", cfg.tts_reverb_room_scale, 1.35);
        setValue("server-tts-reverb-damp", cfg.tts_reverb_damp, 0.26);
    }

    function bindModeTabs() {
        bindTabButtons("[data-mode-tab]", "[data-mode-panel]", "data-mode-tab", "data-mode-panel");
    }

    function bindSettingsTabs() {
        bindTabButtons("[data-settings-tab]", "[data-settings-panel]", "data-settings-tab", "data-settings-panel");
    }

    function createTalkLiveState() {
        return {
            active: false,
            ws: null,
            mediaStream: null,
            audioCtx: null,
            sourceNode: null,
            processorNode: null,
            sinkGainNode: null,
            onStatus: null
        };
    }

    const talkLive = createTalkLiveState();

    function talkLiveSetUi(active) {
        const startBtn = $("talk-live-start-btn");
        const stopBtn = $("talk-live-stop-btn");
        const sayBtn = $("talk-say-btn");
        const statusEl = $("talk-live-state");
        if (startBtn) {
            startBtn.disabled = !!active;
        }
        if (stopBtn) {
            stopBtn.disabled = !active;
        }
        if (sayBtn) {
            sayBtn.disabled = !!active;
        }
        if (statusEl) {
            statusEl.textContent = active ? "Live: streaming" : "Live: stopped";
        }
    }

    function talkLiveResampleLinear(input, inRate, outRate) {
        if (!input || input.length === 0) {
            return new Float32Array(0);
        }
        if (inRate === outRate) {
            return new Float32Array(input);
        }
        const ratio = inRate / outRate;
        const outLen = Math.max(1, Math.floor(input.length / ratio));
        const out = new Float32Array(outLen);
        for (let i = 0; i < outLen; i++) {
            const idx = i * ratio;
            const idx0 = Math.floor(idx);
            const idx1 = Math.min(idx0 + 1, input.length - 1);
            const frac = idx - idx0;
            out[i] = input[idx0] * (1 - frac) + input[idx1] * frac;
        }
        return out;
    }

    const TALK_LIVE_INPUT_GAIN_FLOOR = 0.55;
    const TALK_LIVE_INPUT_GAIN_MAX = 1.25;
    const TALK_LIVE_TARGET_PEAK = 0.30;
    const TALK_LIVE_SILENCE_PEAK = 0.015;

    function talkLiveComputeGain(samples) {
        if (!samples || samples.length === 0) {
            return TALK_LIVE_INPUT_GAIN_FLOOR;
        }
        let peak = 0.0;
        for (let i = 0; i < samples.length; i++) {
            const a = Math.abs(samples[i]);
            if (a > peak) {
                peak = a;
            }
        }
        if (peak <= TALK_LIVE_SILENCE_PEAK) {
            return TALK_LIVE_INPUT_GAIN_FLOOR;
        }
        const auto = TALK_LIVE_TARGET_PEAK / peak;
        const g = Math.max(TALK_LIVE_INPUT_GAIN_FLOOR, Math.min(TALK_LIVE_INPUT_GAIN_MAX, auto));
        return g;
    }

    function talkLiveFloatToPcm16(samples, gain) {
        const g = (typeof gain === "number" && isFinite(gain) && gain > 0) ? gain : TALK_LIVE_INPUT_GAIN_FLOOR;
        const out = new Int16Array(samples.length);
        for (let i = 0; i < samples.length; i++) {
            let s = samples[i] * g;
            if (s > 1) {
                s = 1;
            } else if (s < -1) {
                s = -1;
            }
            out[i] = s < 0 ? (s * 32768) : (s * 32767);
        }
        return out;
    }

    function talkLiveGetUserMediaLegacy(constraints) {
        const getLegacy =
            navigator.getUserMedia ||
            navigator.webkitGetUserMedia ||
            navigator.mozGetUserMedia ||
            navigator.msGetUserMedia;
        if (!getLegacy) {
            return null;
        }
        return new Promise((resolve, reject) => {
            getLegacy.call(navigator, constraints, resolve, reject);
        });
    }

    async function talkLiveGetUserMedia(constraints) {
        if (navigator.mediaDevices && navigator.mediaDevices.getUserMedia) {
            return navigator.mediaDevices.getUserMedia(constraints);
        }
        const legacyPromise = talkLiveGetUserMediaLegacy(constraints);
        if (legacyPromise) {
            return legacyPromise;
        }
        if (window.isSecureContext === false) {
            throw new Error("mic_requires_https");
        }
        throw new Error("mic_not_supported");
    }

    async function talkLiveStopInternal(reason, postStopCommand) {
        const st = talkLive;
        if (!st.active && !st.ws && !st.mediaStream && !st.audioCtx) {
            talkLiveSetUi(false);
            return;
        }

        const ws = st.ws;
        st.ws = null;
        if (ws) {
            ws.onopen = null;
            ws.onmessage = null;
            ws.onerror = null;
            ws.onclose = null;
            if (postStopCommand && ws.readyState === WebSocket.OPEN) {
                try {
                    ws.send("stop");
                } catch (e) {
                    void e;
                }
            }
            if (ws.readyState === WebSocket.OPEN || ws.readyState === WebSocket.CONNECTING) {
                try {
                    ws.close();
                } catch (e) {
                    void e;
                }
            }
        }

        if (st.processorNode) {
            st.processorNode.onaudioprocess = null;
            try {
                st.processorNode.disconnect();
            } catch (e) {
                void e;
            }
            st.processorNode = null;
        }
        if (st.sourceNode) {
            try {
                st.sourceNode.disconnect();
            } catch (e) {
                void e;
            }
            st.sourceNode = null;
        }
        if (st.sinkGainNode) {
            try {
                st.sinkGainNode.disconnect();
            } catch (e) {
                void e;
            }
            st.sinkGainNode = null;
        }
        if (st.mediaStream) {
            st.mediaStream.getTracks().forEach((t) => t.stop());
            st.mediaStream = null;
        }
        if (st.audioCtx) {
            try {
                await st.audioCtx.close();
            } catch (e) {
                void e;
            }
            st.audioCtx = null;
        }

        st.active = false;
        talkLiveSetUi(false);
        if (typeof st.onStatus === "function") {
            st.onStatus(reason || "Live stream stopped", false);
        }
    }

    async function talkLiveStart(onStatus) {
        const st = talkLive;
        if (st.active) {
            return;
        }
        st.onStatus = onStatus;

        let ws = null;
        let mediaStream = null;
        let audioCtx = null;
        let source = null;
        let processor = null;
        let sinkGain = null;
        try {
            mediaStream = await talkLiveGetUserMedia({
                audio: {
                    channelCount: 1,
                    echoCancellation: true,
                    noiseSuppression: true,
                    autoGainControl: true
                },
                video: false
            });

            const AudioContextCtor = window.AudioContext || window.webkitAudioContext;
            if (!AudioContextCtor) {
                throw new Error("audio_context_not_supported");
            }

            audioCtx = new AudioContextCtor();
            if (audioCtx.state === "suspended") {
                await audioCtx.resume();
            }

            source = audioCtx.createMediaStreamSource(mediaStream);
            processor = audioCtx.createScriptProcessor(1024, 1, 1);
            sinkGain = audioCtx.createGain();
            sinkGain.gain.value = 0.0;
            source.connect(processor);
            processor.connect(sinkGain);
            sinkGain.connect(audioCtx.destination);

            const proto = (window.location.protocol === "https:") ? "wss://" : "ws://";
            const wsUrl = proto + window.location.host + "/ws/talk";
            let wsOpened = false;
            ws = new WebSocket(wsUrl);
            ws.binaryType = "arraybuffer";
            await new Promise((resolve, reject) => {
                const t = setTimeout(() => reject(new Error("live_ws_timeout")), 5000);
                ws.onopen = () => {
                    clearTimeout(t);
                    wsOpened = true;
                    resolve();
                };
                ws.onerror = () => {
                    clearTimeout(t);
                    reject(new Error("live_ws_open_failed"));
                };
                ws.onclose = () => {
                    if (!wsOpened) {
                        clearTimeout(t);
                        reject(new Error("live_ws_closed"));
                    }
                };
            });

            processor.onaudioprocess = (event) => {
                if (!st.active || !st.ws || st.ws.readyState !== WebSocket.OPEN) {
                    return;
                }
                if (st.ws.bufferedAmount > (256 * 1024)) {
                    return;
                }
                const ch0 = event.inputBuffer.getChannelData(0);
                const resampled = talkLiveResampleLinear(ch0, event.inputBuffer.sampleRate, 44100);
                const gain = talkLiveComputeGain(resampled);
                const pcm16 = talkLiveFloatToPcm16(resampled, gain);
                try {
                    st.ws.send(pcm16.buffer);
                } catch (e) {
                    void e;
                }
            };

            ws.onclose = () => {
                if (st.active) {
                    talkLiveStopInternal("Live stream closed", false).catch(() => { });
                }
            };
            ws.onerror = () => {
                if (st.active && typeof st.onStatus === "function") {
                    st.onStatus("Live stream socket error", true);
                }
            };

            st.ws = ws;
            st.mediaStream = mediaStream;
            st.audioCtx = audioCtx;
            st.sourceNode = source;
            st.processorNode = processor;
            st.sinkGainNode = sinkGain;
            st.active = true;
            talkLiveSetUi(true);
            if (typeof st.onStatus === "function") {
                st.onStatus("Live mic stream started", false);
            }
        } catch (e) {
            if (e && e.name === "NotAllowedError") {
                e = new Error("mic_permission_denied");
            } else if (e && e.name === "NotFoundError") {
                e = new Error("mic_device_not_found");
            } else if (e && e.name === "NotReadableError") {
                e = new Error("mic_device_busy");
            }
            if (mediaStream) {
                mediaStream.getTracks().forEach((t) => t.stop());
            }
            if (audioCtx) {
                try {
                    await audioCtx.close();
                } catch (closeErr) {
                    void closeErr;
                }
            }
            try {
                if (ws && ws.readyState === WebSocket.OPEN) {
                    ws.send("stop");
                }
            } catch (sendErr) {
                void sendErr;
            }
            try {
                if (ws) {
                    ws.close();
                }
            } catch (closeWsErr) {
                void closeWsErr;
            }
            throw e;
        }
    }

    function bindTalkPanel() {
        const sayBtn = $("talk-say-btn");
        const liveStartBtn = $("talk-live-start-btn");
        const liveStopBtn = $("talk-live-stop-btn");
        if (!sayBtn && !liveStartBtn && !liveStopBtn) {
            return;
        }
        talkLiveSetUi(false);

        if (sayBtn) {
            sayBtn.addEventListener("click", async () => {
                const text = (($("talk-text") && $("talk-text").value) || "").trim();
                const timeout = ($("talk-timeout-ms") && $("talk-timeout-ms").value) || "90000";
                if (!text) {
                    setStatus("Talk text is required", true);
                    return;
                }
                sayBtn.disabled = true;
                try {
                    await postForm("/api/talk/say", {
                        text: text,
                        timeout_ms: timeout,
                    });
                    setStatus("Talk request sent", false);
                } catch (e) {
                    if (e.message === "talk_busy") {
                        setStatus("Sphere is busy now, wait for current response", true);
                    } else {
                        setStatus("Talk failed: " + e.message, true);
                    }
                } finally {
                    if (!talkLive.active) {
                        sayBtn.disabled = false;
                    }
                }
            });
        }

        if (liveStartBtn) {
            liveStartBtn.addEventListener("click", async () => {
                liveStartBtn.disabled = true;
                try {
                    await talkLiveStart(setStatus);
                } catch (e) {
                    const msg = (e && e.message) ? e.message : "unknown_error";
                    if (msg === "mic_requires_https") {
                        setStatus("Live start failed: microphone needs HTTPS (or localhost)", true);
                    } else if (msg === "mic_permission_denied") {
                        setStatus("Live start failed: microphone permission denied", true);
                    } else if (msg === "mic_device_not_found") {
                        setStatus("Live start failed: no microphone found", true);
                    } else if (msg === "mic_device_busy") {
                        setStatus("Live start failed: microphone is busy", true);
                    } else {
                        setStatus("Live start failed: " + msg, true);
                    }
                    talkLiveSetUi(false);
                } finally {
                    if (!talkLive.active) {
                        liveStartBtn.disabled = false;
                    }
                }
            });
        }

        if (liveStopBtn) {
            liveStopBtn.addEventListener("click", async () => {
                liveStopBtn.disabled = true;
                try {
                    await talkLiveStopInternal("Live mic stream stopped", true);
                } catch (e) {
                    setStatus("Live stop failed: " + e.message, true);
                } finally {
                    talkLiveSetUi(false);
                }
            });
        }

        window.addEventListener("beforeunload", () => {
            if (talkLive.active) {
                talkLiveStopInternal("", true).catch(() => { });
            }
        });
    }

    async function bindOfflinePage() {
        bindModeTabs();
        bindSettingsTabs();
        bindTalkPanel();

        const refreshBtn = $("mode-refresh-btn");
        if (refreshBtn) {
            refreshBtn.addEventListener("click", async () => {
                try {
                    await refreshOfflineAll();
                    setStatus("Offline state refreshed", false);
                } catch (e) {
                    setStatus("Refresh failed: " + e.message, true);
                }
            });
        }

        const saveCommon = $("offline-save-common-btn");
        if (saveCommon) {
            saveCommon.addEventListener("click", async () => {
                try {
                    await postQuery("/api/offline/config", {
                        submode: $("offline-submode-select").value,
                        fg_volume: $("offline-fg-volume").value,
                        save: "1",
                    });
                    await refreshOfflineAll();
                    setStatus("Common settings saved", false);
                } catch (e) {
                    setStatus("Save failed: " + e.message, true);
                }
            });
        }

        const saveAura = $("offline-save-aura-btn");
        if (saveAura) {
            saveAura.addEventListener("click", async () => {
                try {
                    await postQuery("/api/offline/config", {
                        aura_gap_ms: $("offline-aura-gap-ms").value,
                        aura_intro_dir: $("offline-aura-intro-dir").value,
                        aura_response_dir: $("offline-aura-response-dir").value,
                        save: "1",
                    });
                    await refreshOfflineAll();
                    setStatus("Aura settings saved", false);
                } catch (e) {
                    setStatus("Aura save failed: " + e.message, true);
                }
            });
        }

        const saveProphecy = $("offline-save-prophecy-btn");
        if (saveProphecy) {
            saveProphecy.addEventListener("click", async () => {
                try {
                    await postQuery("/api/offline/config", {
                        prophecy_gap12_ms: $("offline-prophecy-gap12").value,
                        prophecy_gap23_ms: $("offline-prophecy-gap23").value,
                        prophecy_gap34_ms: $("offline-prophecy-gap34").value,
                        prophecy_leadin_wait_ms: $("offline-prophecy-leadin").value,
                        prophecy_bg_gain_permille: $("offline-prophecy-bg-gain").value,
                        prophecy_bg_fade_in_ms: $("offline-prophecy-bg-fade-in").value,
                        prophecy_bg_fade_out_ms: $("offline-prophecy-bg-fade-out").value,
                        save: "1",
                    });
                    await refreshOfflineAll();
                    setStatus("Prophecy settings saved", false);
                } catch (e) {
                    setStatus("Prophecy save failed: " + e.message, true);
                }
            });
        }

        const lotteryStart = $("offline-lottery-start-btn");
        if (lotteryStart) {
            lotteryStart.addEventListener("click", async () => {
                try {
                    await postQuery("/api/offline/action", { name: "lottery_start" });
                    await refreshOfflineAll();
                    setStatus("Lottery round start requested", false);
                } catch (e) {
                    setStatus("Lottery start failed: " + e.message, true);
                }
            });
        }

        await refreshOfflineAll();
    }

    async function bindModeGenericPage(pageName) {
        bindModeTabs();
        bindTalkPanel();

        const refreshBtn = $("mode-refresh-btn");
        if (refreshBtn) {
            refreshBtn.addEventListener("click", async () => {
                try {
                    await refreshModeGeneric(pageName);
                    setStatus("Mode state refreshed", false);
                } catch (e) {
                    setStatus("Refresh failed: " + e.message, true);
                }
            });
        }

        const saveBtn = $("mode-save-settings-btn");
        if (saveBtn) {
            saveBtn.addEventListener("click", async () => {
                try {
                    await postQuery("/api/config", {
                        save: "1",
                    });
                    await refreshModeGeneric(pageName);
                    setStatus("Mode settings saved", false);
                } catch (e) {
                    setStatus("Settings save failed: " + e.message, true);
                }
            });
        }

        await refreshModeGeneric(pageName);
    }

    async function bindNetworkSetupPage(pageName) {
        bindModeTabs();
        bindTalkPanel();

        const refreshBtn = $("mode-refresh-btn");
        if (refreshBtn) {
            refreshBtn.addEventListener("click", async () => {
                try {
                    await refreshNetworkSetupState(pageName);
                    setStatus("Network state refreshed", false);
                } catch (e) {
                    setStatus("Refresh failed: " + e.message, true);
                }
            });
        }

        const saveBtn = $("inst-wifi-save-btn");
        if (saveBtn) {
            saveBtn.addEventListener("click", async () => {
                const ssid = ($("inst-wifi-ssid") && $("inst-wifi-ssid").value || "").trim();
                const pass = ($("inst-wifi-password") && $("inst-wifi-password").value || "");
                if (!ssid) {
                    setStatus("SSID is required", true);
                    return;
                }
                if (!pass) {
                    setStatus("Password is required", true);
                    return;
                }
                try {
                    const net = await postQuery("/api/network/config", {
                        sta_ssid: ssid,
                        sta_password: pass,
                        save: "1",
                    });
                    await refreshNetworkSetupState(pageName);
                    const ip = net.sta_ip || "";
                    if (ip) {
                        alert("Connected to STA. New IP: " + ip);
                    } else {
                        alert("Credentials applied. Waiting for STA IP...");
                    }
                    setStatus("STA credentials applied", false);
                } catch (e) {
                    setStatus("Wi-Fi apply failed: " + e.message, true);
                }
            });
        }

        const ledRange = $("mode-led-brightness-range");
        const ledValue = $("mode-led-brightness-value");
        const ledApply = $("mode-led-brightness-apply-btn");
        if (ledRange && ledValue) {
            const syncRangeToValue = () => {
                ledValue.value = ledRange.value;
            };
            const syncValueToRange = () => {
                let v = Number(ledValue.value);
                if (!Number.isFinite(v)) {
                    v = 0;
                }
                if (v < 0) {
                    v = 0;
                } else if (v > 255) {
                    v = 255;
                }
                ledValue.value = String(Math.round(v));
                ledRange.value = ledValue.value;
            };
            ledRange.addEventListener("input", syncRangeToValue);
            ledValue.addEventListener("input", syncValueToRange);
        }
        if (ledApply) {
            ledApply.addEventListener("click", async () => {
                const raw = (ledValue && ledValue.value !== "") ? ledValue.value : (ledRange ? ledRange.value : "0");
                let brightness = Number(raw);
                if (!Number.isFinite(brightness)) {
                    setStatus("Brightness must be a number", true);
                    return;
                }
                brightness = Math.round(brightness);
                if (brightness < 0 || brightness > 255) {
                    setStatus("Brightness must be in range 0..255", true);
                    return;
                }
                try {
                    await postQuery("/api/config", {
                        brightness: brightness,
                        save: "1",
                    });
                    await refreshNetworkSetupState(pageName);
                    setStatus("Brightness updated", false);
                } catch (e) {
                    setStatus("Brightness update failed: " + e.message, true);
                }
            });
        }

        const volumeRange = $("mode-audio-volume-range");
        const volumeValue = $("mode-audio-volume-value");
        const volumeApply = $("mode-audio-volume-apply-btn");
        if (volumeRange && volumeValue) {
            const syncRangeToValue = () => {
                volumeValue.value = volumeRange.value;
            };
            const syncValueToRange = () => {
                let v = Number(volumeValue.value);
                if (!Number.isFinite(v)) {
                    v = 0;
                }
                if (v < 0) {
                    v = 0;
                } else if (v > 100) {
                    v = 100;
                }
                volumeValue.value = String(Math.round(v));
                volumeRange.value = volumeValue.value;
            };
            volumeRange.addEventListener("input", syncRangeToValue);
            volumeValue.addEventListener("input", syncValueToRange);
        }
        if (volumeApply) {
            volumeApply.addEventListener("click", async () => {
                const raw = (volumeValue && volumeValue.value !== "") ? volumeValue.value : (volumeRange ? volumeRange.value : "0");
                let volume = Number(raw);
                if (!Number.isFinite(volume)) {
                    setStatus("Volume must be a number", true);
                    return;
                }
                volume = Math.round(volume);
                if (volume < 0 || volume > 100) {
                    setStatus("Volume must be in range 0..100", true);
                    return;
                }
                try {
                    await postQuery("/api/config", {
                        volume: volume,
                        save: "1",
                    });
                    await refreshNetworkSetupState(pageName);
                    setStatus("Volume updated", false);
                } catch (e) {
                    setStatus("Volume update failed: " + e.message, true);
                }
            });
        }

        const hybridMicCapture = $("mode-hybrid-mic-capture-ms");
        const hybridReject = $("mode-hybrid-reject-threshold");
        const hybridParamsApply = $("mode-hybrid-params-apply-btn");
        if (hybridParamsApply) {
            hybridParamsApply.addEventListener("click", async () => {
                let micCapture = Number(hybridMicCapture ? hybridMicCapture.value : "");
                let rejectThreshold = Number(hybridReject ? hybridReject.value : "");
                if (!Number.isFinite(micCapture) || !Number.isFinite(rejectThreshold)) {
                    setStatus("Hybrid params must be numbers", true);
                    return;
                }
                micCapture = Math.round(micCapture);
                rejectThreshold = Math.round(rejectThreshold);
                if (micCapture < 1000 || micCapture > 60000) {
                    setStatus("Mic capture must be in range 1000..60000 ms", true);
                    return;
                }
                if (rejectThreshold < 0 || rejectThreshold > 1000) {
                    setStatus("Reject threshold must be in range 0..1000", true);
                    return;
                }
                try {
                    await postQuery("/api/config", {
                        hybrid_mic_capture_ms: micCapture,
                        hybrid_reject_threshold_permille: rejectThreshold,
                        save: "1",
                    });
                    await refreshNetworkSetupState(pageName);
                    setStatus("Hybrid params updated", false);
                } catch (e) {
                    setStatus("Hybrid params update failed: " + e.message, true);
                }
            });
        }

        const serverTtsLoadBtn = $("server-tts-load-btn");
        const serverTtsApplyBtn = $("server-tts-apply-btn");
        const serverTtsSaveBtn = $("server-tts-save-btn");

        if (serverTtsLoadBtn) {
            serverTtsLoadBtn.addEventListener("click", async () => {
                try {
                    const data = await serverTtsApi("/api/tts/config", "GET");
                    fillServerTtsForm(data.config || {});
                    setServerTtsStatus("Loaded", false);
                    setStatus("Server TTS config loaded", false);
                } catch (e) {
                    setServerTtsStatus("Load failed", true);
                    setStatus("Server TTS load failed: " + e.message, true);
                }
            });
        }

        if (serverTtsApplyBtn) {
            serverTtsApplyBtn.addEventListener("click", async () => {
                const num = (id, fallback) => {
                    const el = $(id);
                    const raw = el && el.value !== "" ? el.value : String(fallback);
                    const v = Number(raw);
                    if (!Number.isFinite(v)) {
                        throw new Error("invalid_number_" + id);
                    }
                    return v;
                };
                try {
                    const patch = {
                        tts_backend: ($("server-tts-backend") && $("server-tts-backend").value) || "silero",
                        silero_speaker: ($("server-silero-speaker") && $("server-silero-speaker").value || "").trim(),
                        tts_tempo_scale: num("server-tts-tempo-scale", 0.85),
                        piper_pitch_scale: num("server-piper-pitch-scale", 1.0),
                        tts_phrase_pause_ms: Math.round(num("server-tts-phrase-pause-ms", 1500)),
                        tts_fx_preset: ($("server-tts-fx-preset") && $("server-tts-fx-preset").value) || "mystic",
                        tts_echo_mix: num("server-tts-echo-mix", 0.24),
                        tts_echo_delay_ms: Math.round(num("server-tts-echo-delay-ms", 220)),
                        tts_echo_feedback: num("server-tts-echo-feedback", 0.46),
                        tts_reverb_mix: num("server-tts-reverb-mix", 0.36),
                        tts_reverb_room_scale: num("server-tts-reverb-room-scale", 1.35),
                        tts_reverb_damp: num("server-tts-reverb-damp", 0.26),
                    };
                    const data = await serverTtsApi("/api/tts/config", "POST", patch);
                    fillServerTtsForm(data.config || {});
                    setServerTtsStatus("Applied", false);
                    setStatus("Server TTS config applied", false);
                } catch (e) {
                    setServerTtsStatus("Apply failed", true);
                    setStatus("Server TTS apply failed: " + e.message, true);
                }
            });
        }

        if (serverTtsSaveBtn) {
            serverTtsSaveBtn.addEventListener("click", async () => {
                try {
                    await serverTtsApi("/api/tts/config/save", "POST", {});
                    setServerTtsStatus("Saved", false);
                    setStatus("Server TTS config saved", false);
                } catch (e) {
                    setServerTtsStatus("Save failed", true);
                    setStatus("Server TTS save failed: " + e.message, true);
                }
            });
        }

        await refreshNetworkSetupState(pageName);
    }

    async function bindLegacyPage(page) {
        void page;
    }

    async function boot() {
        const page = document.body.dataset.page;
        if (page === "offline") {
            await bindOfflinePage();
            return;
        }
        if (page === "hybrid") {
            await bindNetworkSetupPage("hybrid");
            return;
        }
        if (page === "installation") {
            await bindNetworkSetupPage("installation");
            return;
        }
        await bindLegacyPage(page);
    }

    function startBoot() {
        if (sStarted) {
            return;
        }
        sStarted = true;
        window.__orb_app_boot_started = true;
        fetch("/health", { method: "GET", cache: "no-store" }).catch(function () { });
        boot().catch((e) => setStatus("Page init failed: " + e.message, true));
    }

    if (document.readyState === "loading") {
        window.addEventListener("DOMContentLoaded", startBoot);
    } else {
        startBoot();
    }
})();
