(function () {
    "use strict";

    async function refreshModeGeneric(ctx, pageName) {
        const status = await ctx.api("/api/status");
        const cfg = await ctx.api("/api/config");
        ctx.ensureModePageAllowed(pageName, status.mode);
        ctx.renderModeStatus(status);
        return { status: status, config: cfg };
    }

    async function bind(ctx, pageName) {
        const $ = ctx.$;
        const setStatus = ctx.setStatus;
        const postQuery = ctx.postQuery;

        ctx.bindModeTabs();
        ctx.bindTalkPanelIfAvailable();

        const refreshBtn = $("mode-refresh-btn");
        if (refreshBtn) {
            refreshBtn.addEventListener("click", async () => {
                try {
                    await refreshModeGeneric(ctx, pageName);
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
                    await postQuery("/api/config", { save: "1" });
                    await refreshModeGeneric(ctx, pageName);
                    setStatus("Mode settings saved", false);
                } catch (e) {
                    setStatus("Settings save failed: " + e.message, true);
                }
            });
        }

        await refreshModeGeneric(ctx, pageName);
    }

    window.OrbPageModeGeneric = {
        bind: bind,
        refresh: refreshModeGeneric
    };
})();

