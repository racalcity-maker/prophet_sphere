(function () {
    "use strict";

    let sStarted = false;

    const Core = window.OrbAppCore || {};
    const TalkFeature = window.OrbTalkFeature || null;

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

    function createPageContext() {
        return {
            $: $,
            api: api,
            postQuery: postQuery,
            setStatus: setStatus,
            setText: setText,
            setValue: setValue,
            setBlockVisible: setBlockVisible,
            renderModeStatus: renderModeStatus,
            ensureModePageAllowed: ensureModePageAllowed,
            bindModeTabs: bindModeTabs,
            bindSettingsTabs: bindSettingsTabs,
            bindTalkPanelIfAvailable: bindTalkPanelIfAvailable
        };
    }

    async function boot() {
        const page = document.body.dataset.page;
        const ctx = createPageContext();

        if (page === "offline") {
            const offline = window.OrbPageOffline;
            if (offline && typeof offline.bind === "function") {
                await offline.bind(ctx);
                return;
            }
        }
        if (page === "hybrid" || page === "installation") {
            const network = window.OrbPageNetworkSetup;
            if (network && typeof network.bind === "function") {
                await network.bind(ctx, page);
                return;
            }
        }
        if (page === "mode") {
            const modeGeneric = window.OrbPageModeGeneric;
            if (modeGeneric && typeof modeGeneric.bind === "function") {
                await modeGeneric.bind(ctx, page);
                return;
            }
        }

        setStatus("No page binder for: " + page, true);
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

