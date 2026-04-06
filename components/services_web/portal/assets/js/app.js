(function () {
    "use strict";

    let sStarted = false;

    const Core = window.OrbAppCore || {};
    const HybridEffects = window.OrbHybridEffects || null;
    const TalkFeature = window.OrbTalkFeature || null;
    const ServerTtsFeature = window.OrbServerTts || null;

    const $ = Core.$ || function (id) { return document.getElementById(id); };
    const api = Core.api;
    const postQuery = Core.postQuery;
    const setStatus = Core.setStatus || function () { };
    const setText = Core.setText || function () { };
    const setValue = Core.setValue || function () { };
    const setBlockVisible = Core.setBlockVisible || function () { };

    function modeToPage(mode) {
        if (mode === "hybrid_ai") {
            return "hybrid";
        }
        if (mode === "installation_slave") {
            return "installation";
        }
        return "offline";
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
            return function () { };
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
        const status = await api("/api/status");
        const state = await api("/api/offline/state");
        ensureModePageAllowed("offline", status.mode);
        renderModeStatus(status);
        renderOfflineState(state);
        return { status: status, state: state };
    }

    async function refreshModeGeneric(page) {
        const status = await api("/api/status");
        const cfg = await api("/api/config");
        ensureModePageAllowed(page, status.mode);
        renderModeStatus(status);
        return { status: status, config: cfg };
    }

    async function refreshNetworkSetupState(pageName) {
        const status = await api("/api/status");
        const net = await api("/api/network/status");
        const cfg = await api("/api/config");
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
        setValue("mode-hybrid-unknown-retries", cfg.hybrid_unknown_retry_max, 1);
        setValue("mode-hybrid-bg-fade-out-ms", cfg.prophecy_bg_fade_out_ms, 4000);
        setText(
            "mode-hybrid-params-current",
            (cfg.hybrid_mic_capture_ms !== undefined &&
                cfg.hybrid_reject_threshold_permille !== undefined &&
                cfg.hybrid_unknown_retry_max !== undefined &&
                cfg.prophecy_bg_fade_out_ms !== undefined)
                ? (
                    String(cfg.hybrid_mic_capture_ms) + " ms / " +
                    String(cfg.hybrid_reject_threshold_permille) + " / " +
                    String(cfg.hybrid_unknown_retry_max) + " / " +
                    String(cfg.prophecy_bg_fade_out_ms) + " ms"
                )
                : "-",
            "-"
        );
        if (HybridEffects && typeof HybridEffects.applyConfig === "function") {
            HybridEffects.applyConfig(cfg);
        }
        return { status: status, network: net, config: cfg };
    }

    function bindModeTabs() {
        bindTabButtons("[data-mode-tab]", "[data-mode-panel]", "data-mode-tab", "data-mode-panel");
    }

    function bindSettingsTabs() {
        bindTabButtons("[data-settings-tab]", "[data-settings-panel]", "data-settings-tab", "data-settings-panel");
    }

    function bindTalkPanelIfAvailable() {
        if (TalkFeature && typeof TalkFeature.bindPanel === "function") {
            TalkFeature.bindPanel({
                setStatus: setStatus,
                postForm: Core.postForm
            });
        }
    }

    async function bindOfflinePage() {
        bindModeTabs();
        bindSettingsTabs();
        bindTalkPanelIfAvailable();

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
        bindTalkPanelIfAvailable();

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
        bindTalkPanelIfAvailable();

        const isHybridPage = (pageName === "hybrid");
        setBlockVisible("mode-effects-tab-btn", isHybridPage);
        setBlockVisible("mode-effects-panel", isHybridPage);

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
        const hybridRetries = $("mode-hybrid-unknown-retries");
        const hybridBgFadeOut = $("mode-hybrid-bg-fade-out-ms");
        const hybridParamsApply = $("mode-hybrid-params-apply-btn");
        if (hybridParamsApply) {
            hybridParamsApply.addEventListener("click", async () => {
                let micCapture = Number(hybridMicCapture ? hybridMicCapture.value : "");
                let rejectThreshold = Number(hybridReject ? hybridReject.value : "");
                let unknownRetries = Number(hybridRetries ? hybridRetries.value : "");
                let bgFadeOutMs = Number(hybridBgFadeOut ? hybridBgFadeOut.value : "");
                if (!Number.isFinite(micCapture) ||
                    !Number.isFinite(rejectThreshold) ||
                    !Number.isFinite(unknownRetries) ||
                    !Number.isFinite(bgFadeOutMs)) {
                    setStatus("Hybrid params must be numbers", true);
                    return;
                }
                micCapture = Math.round(micCapture);
                rejectThreshold = Math.round(rejectThreshold);
                unknownRetries = Math.round(unknownRetries);
                bgFadeOutMs = Math.round(bgFadeOutMs);
                if (micCapture < 1000 || micCapture > 60000) {
                    setStatus("Mic capture must be in range 1000..60000 ms", true);
                    return;
                }
                if (rejectThreshold < 0 || rejectThreshold > 1000) {
                    setStatus("Reject threshold must be in range 0..1000", true);
                    return;
                }
                if (unknownRetries < 0 || unknownRetries > 2) {
                    setStatus("Unknown retries must be in range 0..2", true);
                    return;
                }
                if (bgFadeOutMs < 0 || bgFadeOutMs > 60000) {
                    setStatus("Background fade-out must be in range 0..60000 ms", true);
                    return;
                }
                try {
                    await postQuery("/api/config", {
                        hybrid_mic_capture_ms: micCapture,
                        hybrid_reject_threshold_permille: rejectThreshold,
                        hybrid_unknown_retry_max: unknownRetries,
                        prophecy_bg_fade_out_ms: bgFadeOutMs,
                        save: "1",
                    });
                    await refreshNetworkSetupState(pageName);
                    setStatus("Hybrid params updated", false);
                } catch (e) {
                    setStatus("Hybrid params update failed: " + e.message, true);
                }
            });
        }

        if (HybridEffects && typeof HybridEffects.bind === "function") {
            HybridEffects.bind({
                isHybridPage: isHybridPage,
                setStatus: setStatus,
                postQuery: postQuery,
                refreshNetworkSetupState: refreshNetworkSetupState,
                pageName: pageName
            });
        }

        if (ServerTtsFeature && typeof ServerTtsFeature.bind === "function") {
            ServerTtsFeature.bind({
                setStatus: setStatus,
                postQuery: postQuery
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
        if (page === "mode") {
            await bindModeGenericPage("mode");
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
