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
        return { status: status, network: net, config: cfg };
    }

    function bindModeTabs() {
        bindTabButtons("[data-mode-tab]", "[data-mode-panel]", "data-mode-tab", "data-mode-panel");
    }

    function bindSettingsTabs() {
        bindTabButtons("[data-settings-tab]", "[data-settings-panel]", "data-settings-tab", "data-settings-panel");
    }

    async function bindOfflinePage() {
        bindModeTabs();
        bindSettingsTabs();

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
