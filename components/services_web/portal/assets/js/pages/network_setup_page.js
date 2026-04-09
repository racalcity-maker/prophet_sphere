(function () {
    "use strict";

    async function refreshNetworkSetupState(ctx, pageName) {
        const status = await ctx.api("/api/status");
        const net = await ctx.api("/api/network/status");
        const cfg = await ctx.api("/api/config");

        ctx.ensureModePageAllowed(pageName, status.mode);
        ctx.renderModeStatus(status);

        ctx.setValue("inst-wifi-ssid", net.sta_ssid || "", "");
        ctx.setText("inst-net-up", net.up ? "true" : "false", "false");
        ctx.setText("inst-net-active", net.active || "-", "-");
        ctx.setText("inst-net-sta-ip", net.sta_ip || "-", "-");
        ctx.setText("inst-net-ap-ip", net.ap_ip || "-", "-");
        ctx.setText("inst-net-link", net.link_state || "-", "-");

        ctx.setValue("mode-led-brightness-range", cfg.brightness, 160);
        ctx.setValue("mode-led-brightness-value", cfg.brightness, 160);
        ctx.setText("mode-led-brightness-current", cfg.brightness, "-");

        ctx.setValue("mode-audio-volume-range", cfg.volume, 60);
        ctx.setValue("mode-audio-volume-value", cfg.volume, 60);
        ctx.setText("mode-audio-volume-current", cfg.volume, "-");

        ctx.setValue("mode-hybrid-mic-capture-ms", cfg.hybrid_mic_capture_ms, 8000);
        ctx.setValue("mode-hybrid-reject-threshold", cfg.hybrid_reject_threshold_permille, 380);
        ctx.setValue("mode-hybrid-unknown-retries", cfg.hybrid_unknown_retry_max, 1);
        ctx.setValue("mode-hybrid-bg-fade-out-ms", cfg.prophecy_bg_fade_out_ms, 4000);
        ctx.setText(
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

        const HybridEffects = window.OrbHybridEffects || null;
        if (HybridEffects && typeof HybridEffects.applyConfig === "function") {
            HybridEffects.applyConfig(cfg);
        }

        return { status: status, network: net, config: cfg };
    }

    function bindRangeNumber(els, min, max) {
        const rangeEl = els.range;
        const valueEl = els.value;
        if (!rangeEl || !valueEl) {
            return;
        }
        rangeEl.addEventListener("input", () => {
            valueEl.value = rangeEl.value;
        });
        valueEl.addEventListener("input", () => {
            let v = Number(valueEl.value);
            if (!Number.isFinite(v)) {
                v = min;
            }
            if (v < min) {
                v = min;
            } else if (v > max) {
                v = max;
            }
            valueEl.value = String(Math.round(v));
            rangeEl.value = valueEl.value;
        });
    }

    function bindRefreshSection(ctx, pageName) {
        const refreshBtn = ctx.$("mode-refresh-btn");
        if (!refreshBtn) {
            return;
        }
        refreshBtn.addEventListener("click", async () => {
            try {
                await refreshNetworkSetupState(ctx, pageName);
                ctx.setStatus("Network state refreshed", false);
            } catch (e) {
                ctx.setStatus("Refresh failed: " + e.message, true);
            }
        });
    }

    function bindWifiConfigSection(ctx, pageName) {
        const saveBtn = ctx.$("inst-wifi-save-btn");
        if (!saveBtn) {
            return;
        }
        saveBtn.addEventListener("click", async () => {
            const ssid = (ctx.$("inst-wifi-ssid") && ctx.$("inst-wifi-ssid").value || "").trim();
            const pass = (ctx.$("inst-wifi-password") && ctx.$("inst-wifi-password").value || "");
            if (!ssid) {
                ctx.setStatus("SSID is required", true);
                return;
            }
            if (!pass) {
                ctx.setStatus("Password is required", true);
                return;
            }
            try {
                const net = await ctx.postQuery("/api/network/config", {
                    sta_ssid: ssid,
                    sta_password: pass,
                    save: "1"
                });
                await refreshNetworkSetupState(ctx, pageName);
                const ip = net.sta_ip || "";
                if (ip) {
                    alert("Connected to STA. New IP: " + ip);
                } else {
                    alert("Credentials applied. Waiting for STA IP...");
                }
                ctx.setStatus("STA credentials applied", false);
            } catch (e) {
                ctx.setStatus("Wi-Fi apply failed: " + e.message, true);
            }
        });
    }

    function bindBrightnessSection(ctx, pageName) {
        const led = {
            range: ctx.$("mode-led-brightness-range"),
            value: ctx.$("mode-led-brightness-value"),
            apply: ctx.$("mode-led-brightness-apply-btn")
        };
        bindRangeNumber(led, 0, 255);
        if (!led.apply) {
            return;
        }
        led.apply.addEventListener("click", async () => {
            const raw = (led.value && led.value.value !== "") ? led.value.value : (led.range ? led.range.value : "0");
            let brightness = Number(raw);
            if (!Number.isFinite(brightness)) {
                ctx.setStatus("Brightness must be a number", true);
                return;
            }
            brightness = Math.round(brightness);
            if (brightness < 0 || brightness > 255) {
                ctx.setStatus("Brightness must be in range 0..255", true);
                return;
            }
            try {
                await ctx.postQuery("/api/config", { brightness: brightness, save: "1" });
                await refreshNetworkSetupState(ctx, pageName);
                ctx.setStatus("Brightness updated", false);
            } catch (e) {
                ctx.setStatus("Brightness update failed: " + e.message, true);
            }
        });
    }

    function bindVolumeSection(ctx, pageName) {
        const volume = {
            range: ctx.$("mode-audio-volume-range"),
            value: ctx.$("mode-audio-volume-value"),
            apply: ctx.$("mode-audio-volume-apply-btn")
        };
        bindRangeNumber(volume, 0, 100);
        if (!volume.apply) {
            return;
        }
        volume.apply.addEventListener("click", async () => {
            const raw = (volume.value && volume.value.value !== "") ? volume.value.value : (volume.range ? volume.range.value : "0");
            let v = Number(raw);
            if (!Number.isFinite(v)) {
                ctx.setStatus("Volume must be a number", true);
                return;
            }
            v = Math.round(v);
            if (v < 0 || v > 100) {
                ctx.setStatus("Volume must be in range 0..100", true);
                return;
            }
            try {
                await ctx.postQuery("/api/config", { volume: v, save: "1" });
                await refreshNetworkSetupState(ctx, pageName);
                ctx.setStatus("Volume updated", false);
            } catch (e) {
                ctx.setStatus("Volume update failed: " + e.message, true);
            }
        });
    }

    function bindHybridParamsSection(ctx, pageName) {
        const applyBtn = ctx.$("mode-hybrid-params-apply-btn");
        if (!applyBtn) {
            return;
        }
        applyBtn.addEventListener("click", async () => {
            let micCapture = Number(ctx.$("mode-hybrid-mic-capture-ms") ? ctx.$("mode-hybrid-mic-capture-ms").value : "");
            let rejectThreshold = Number(ctx.$("mode-hybrid-reject-threshold") ? ctx.$("mode-hybrid-reject-threshold").value : "");
            let unknownRetries = Number(ctx.$("mode-hybrid-unknown-retries") ? ctx.$("mode-hybrid-unknown-retries").value : "");
            let bgFadeOutMs = Number(ctx.$("mode-hybrid-bg-fade-out-ms") ? ctx.$("mode-hybrid-bg-fade-out-ms").value : "");
            if (!Number.isFinite(micCapture) ||
                !Number.isFinite(rejectThreshold) ||
                !Number.isFinite(unknownRetries) ||
                !Number.isFinite(bgFadeOutMs)) {
                ctx.setStatus("Hybrid params must be numbers", true);
                return;
            }
            micCapture = Math.round(micCapture);
            rejectThreshold = Math.round(rejectThreshold);
            unknownRetries = Math.round(unknownRetries);
            bgFadeOutMs = Math.round(bgFadeOutMs);
            if (micCapture < 1000 || micCapture > 60000) {
                ctx.setStatus("Mic capture must be in range 1000..60000 ms", true);
                return;
            }
            if (rejectThreshold < 0 || rejectThreshold > 1000) {
                ctx.setStatus("Reject threshold must be in range 0..1000", true);
                return;
            }
            if (unknownRetries < 0 || unknownRetries > 2) {
                ctx.setStatus("Unknown retries must be in range 0..2", true);
                return;
            }
            if (bgFadeOutMs < 0 || bgFadeOutMs > 60000) {
                ctx.setStatus("Background fade-out must be in range 0..60000 ms", true);
                return;
            }
            try {
                await ctx.postQuery("/api/config", {
                    hybrid_mic_capture_ms: micCapture,
                    hybrid_reject_threshold_permille: rejectThreshold,
                    hybrid_unknown_retry_max: unknownRetries,
                    prophecy_bg_fade_out_ms: bgFadeOutMs,
                    save: "1"
                });
                await refreshNetworkSetupState(ctx, pageName);
                ctx.setStatus("Hybrid params updated", false);
            } catch (e) {
                ctx.setStatus("Hybrid params update failed: " + e.message, true);
            }
        });
    }

    function bindHybridFeatureSections(ctx, pageName, isHybridPage) {
        const HybridEffects = window.OrbHybridEffects || null;
        if (HybridEffects && typeof HybridEffects.bind === "function") {
            HybridEffects.bind({
                isHybridPage: isHybridPage,
                setStatus: ctx.setStatus,
                postQuery: ctx.postQuery,
                refreshNetworkSetupState: function (name) {
                    return refreshNetworkSetupState(ctx, name);
                },
                pageName: pageName
            });
        }

        const ServerTtsFeature = window.OrbServerTts || null;
        if (isHybridPage && ServerTtsFeature && typeof ServerTtsFeature.bind === "function") {
            ServerTtsFeature.bind({
                setStatus: ctx.setStatus,
                postQuery: ctx.postQuery
            });
        }
    }

    async function bind(ctx, pageName) {
        ctx.bindModeTabs();
        ctx.bindTalkPanelIfAvailable();

        const isHybridPage = (pageName === "hybrid");
        ctx.setBlockVisible("mode-effects-tab-btn", isHybridPage);
        ctx.setBlockVisible("mode-effects-panel", isHybridPage);

        bindRefreshSection(ctx, pageName);
        bindWifiConfigSection(ctx, pageName);
        bindBrightnessSection(ctx, pageName);
        bindVolumeSection(ctx, pageName);
        bindHybridParamsSection(ctx, pageName);
        bindHybridFeatureSections(ctx, pageName, isHybridPage);

        await refreshNetworkSetupState(ctx, pageName);
    }

    window.OrbPageNetworkSetup = {
        bind: bind,
        refresh: refreshNetworkSetupState
    };
})();
