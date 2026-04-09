(function () {
    "use strict";

    function clampInt(v, min, max, fallback) {
        let n = Number(v);
        if (!Number.isFinite(n)) {
            n = fallback;
        }
        n = Math.round(n);
        if (n < min) {
            n = min;
        } else if (n > max) {
            n = max;
        }
        return n;
    }

    function rgbToHex(r, g, b) {
        const toHex = (x) => {
            const v = clampInt(x, 0, 255, 0);
            const s = v.toString(16);
            return s.length === 1 ? ("0" + s) : s;
        };
        return "#" + toHex(r) + toHex(g) + toHex(b);
    }

    function hexToRgb(hex) {
        const raw = String(hex || "").trim().replace(/^#/, "");
        if (!/^[0-9a-fA-F]{6}$/.test(raw)) {
            return { r: 255, g: 255, b: 255 };
        }
        return {
            r: parseInt(raw.slice(0, 2), 16),
            g: parseInt(raw.slice(2, 4), 16),
            b: parseInt(raw.slice(4, 6), 16)
        };
    }

    function randomRemainderDistribution(teamCount, remainder) {
        const out = Array.from({ length: teamCount }, () => 0);
        for (let i = 0; i < remainder; i += 1) {
            const idx = Math.floor(Math.random() * teamCount);
            out[idx] += 1;
        }
        return out;
    }

    function applyLotteryTeamVisibility(ctx, teamCount) {
        const shown = clampInt(teamCount, 2, 4, 2);
        for (let i = 1; i <= 4; i += 1) {
            ctx.setBlockVisible("offline-lottery-team-" + i, i <= shown);
        }
    }

    function syncLotteryTeamCountButtons(teamCount) {
        const shown = clampInt(teamCount, 2, 4, 2);
        const buttons = Array.from(document.querySelectorAll("[data-lottery-team-count]"));
        buttons.forEach((btn) => {
            const value = Number(btn.getAttribute("data-lottery-team-count") || "0");
            btn.classList.toggle("active", value === shown);
        });
    }

    function applyLotterySourceUi(teamIdx) {
        const modeEl = document.getElementById("offline-lottery-audio-mode");
        const mode = modeEl ? String(modeEl.value || "track") : "track";
        const srcEl = document.getElementById("offline-lottery-t" + teamIdx + "-source");
        const trackEl = document.getElementById("offline-lottery-t" + teamIdx + "-track");
        const ttsEl = document.getElementById("offline-lottery-t" + teamIdx + "-tts");
        if (!trackEl || !ttsEl) {
            return;
        }
        const isTrack = mode !== "tts";
        if (srcEl) {
            srcEl.value = isTrack ? "track" : "tts";
            srcEl.disabled = true;
            const srcWrap = srcEl.closest("label");
            if (srcWrap) {
                srcWrap.style.display = "none";
            }
        }
        trackEl.disabled = !isTrack;
        ttsEl.disabled = isTrack;

        const trackWrap = trackEl.closest("label");
        const ttsWrap = ttsEl.closest("label");
        if (trackWrap) {
            trackWrap.style.display = isTrack ? "" : "none";
        }
        if (ttsWrap) {
            ttsWrap.style.display = isTrack ? "none" : "";
        }
    }

    function applyLotteryFinishUi() {
        const srcEl = document.getElementById("offline-lottery-finish-source");
        const valueEl = document.getElementById("offline-lottery-finish-value");
        const labelEl = document.getElementById("offline-lottery-finish-value-label");
        if (!srcEl || !valueEl || !labelEl) {
            return;
        }
        const source = String(srcEl.value || "track");
        const labelPrefix = (source === "tts") ? "Finish TTS Text" : "Finish Track Path";
        if (labelEl.firstChild && labelEl.firstChild.nodeType === 3) {
            labelEl.firstChild.nodeValue = labelPrefix;
        } else {
            labelEl.insertBefore(document.createTextNode(labelPrefix), labelEl.firstChild || null);
        }
        if (source === "tts") {
            valueEl.placeholder = "Lottery finished. Thank you, everyone.";
        } else {
            valueEl.placeholder = "/sdcard/audio/lottery/finished/custom.mp3";
        }
    }

    function deriveLotteryAudioMode(teams) {
        let hasTrack = false;
        let hasTts = false;
        (teams || []).forEach((team) => {
            const src = String((team && team.source) || "track");
            if (src === "tts") {
                hasTts = true;
            } else {
                hasTrack = true;
            }
        });
        if (hasTts && !hasTrack) {
            return "tts";
        }
        return "track";
    }

    function updateLotteryTeamTitle(teamIdx) {
        const titleEl = document.getElementById("offline-lottery-team-" + teamIdx + "-title");
        const ttsEl = document.getElementById("offline-lottery-t" + teamIdx + "-tts");
        if (!titleEl) {
            return;
        }
        const text = (ttsEl && ttsEl.value ? String(ttsEl.value) : "").trim();
        if (text.length > 0) {
            titleEl.textContent = "Team " + teamIdx + " - " + text;
        } else {
            titleEl.textContent = "Team " + teamIdx;
        }
    }

    function updateLotteryTeamColorVisual(teamIdx) {
        const colorEl = document.getElementById("offline-lottery-t" + teamIdx + "-color");
        const swatchEl = document.getElementById("offline-lottery-t" + teamIdx + "-swatch");
        const cardEl = document.getElementById("offline-lottery-team-" + teamIdx);
        if (!colorEl || !swatchEl || !cardEl) {
            return;
        }
        const raw = String(colorEl.value || "").trim();
        const color = /^#[0-9a-fA-F]{6}$/.test(raw) ? raw : "#94a3b8";
        swatchEl.style.backgroundColor = color;
        cardEl.style.borderColor = color;
        cardEl.style.boxShadow = "inset 0 0 0 1px " + color + "33";
    }

    function renderLotteryPreview() {
        const teamCountEl = document.getElementById("offline-lottery-team-count");
        const totalEl = document.getElementById("offline-lottery-participants-total");
        const previewEl = document.getElementById("offline-lottery-preview");
        if (!teamCountEl || !totalEl || !previewEl) {
            return;
        }

        const teams = clampInt(teamCountEl.value, 2, 4, 2);
        const total = clampInt(totalEl.value, 1, 128, 17);
        const base = Math.floor(total / teams);
        const remainder = total % teams;
        const extra = randomRemainderDistribution(teams, remainder);

        const chunks = [];
        chunks.push("Base per team: " + base + ". Remainder: " + remainder + " (random each round).");
        for (let i = 0; i < teams; i += 1) {
            const amount = base + extra[i];
            chunks.push("T" + (i + 1) + ": " + amount);
        }
        previewEl.textContent = chunks.join("  |  ");
    }

    function collectLotteryParams($) {
        const finishSourceEl = $("offline-lottery-finish-source");
        const finishValueEl = $("offline-lottery-finish-value");
        const finishSource = finishSourceEl ? String(finishSourceEl.value || "track") : "track";
        let finishValue = finishValueEl ? String(finishValueEl.value || "") : "";
        finishValue = finishValue.trim();
        if (finishSource === "tts" && !finishValue) {
            finishValue = "Лотерея завершена.";
            if (finishValueEl) {
                finishValueEl.value = finishValue;
            }
        }

        const params = {
            lottery_team_count: clampInt($("offline-lottery-team-count").value, 2, 4, 2),
            lottery_participants_total: clampInt($("offline-lottery-participants-total").value, 1, 128, 17),
            lottery_mode: ($("offline-lottery-audio-mode") ? $("offline-lottery-audio-mode").value : "track"),
            lottery_finish_source: finishSource,
            lottery_finish_value: finishValue,
            save: "1"
        };
        const forcedSource = params.lottery_mode === "tts" ? "tts" : "track";
        for (let i = 1; i <= 4; i += 1) {
            const rgb = hexToRgb($("offline-lottery-t" + i + "-color").value);
            params["lottery_t" + i + "_r"] = rgb.r;
            params["lottery_t" + i + "_g"] = rgb.g;
            params["lottery_t" + i + "_b"] = rgb.b;
            params["lottery_t" + i + "_source"] = forcedSource;
            params["lottery_t" + i + "_track"] = $("offline-lottery-t" + i + "-track").value || "";
            params["lottery_t" + i + "_tts"] = $("offline-lottery-t" + i + "-tts").value || "";
        }
        return params;
    }

    function renderOfflineState(ctx, state) {
        const setText = ctx.setText;
        const setValue = ctx.setValue;

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

        if (state.lottery) {
            setValue("offline-lottery-team-count", state.lottery.team_count, 2);
            setValue("offline-lottery-participants-total", state.lottery.participants_total, 17);
            applyLotteryTeamVisibility(ctx, Number(state.lottery.team_count || 2));
            syncLotteryTeamCountButtons(Number(state.lottery.team_count || 2));

            const teams = Array.isArray(state.lottery.teams) ? state.lottery.teams : [];
            setValue("offline-lottery-audio-mode", deriveLotteryAudioMode(teams), "track");
            setValue("offline-lottery-finish-source", state.lottery.finish_source || "track", "track");
            setValue("offline-lottery-finish-value", state.lottery.finish_value || "", "");
            applyLotteryFinishUi();
            for (let i = 1; i <= 4; i += 1) {
                const team = teams[i - 1] || {};
                setValue("offline-lottery-t" + i + "-color", rgbToHex(team.color_r, team.color_g, team.color_b), "#ffffff");
                setValue("offline-lottery-t" + i + "-source", team.source || "track", "track");
                setValue("offline-lottery-t" + i + "-track", team.track_path || "", "");
                setValue("offline-lottery-t" + i + "-tts", team.tts_text || "", "");
                applyLotterySourceUi(i);
                updateLotteryTeamTitle(i);
                updateLotteryTeamColorVisual(i);
            }
        } else {
            setValue("offline-lottery-audio-mode", "track", "track");
            setValue("offline-lottery-finish-source", "track", "track");
            setValue("offline-lottery-finish-value", "", "");
            applyLotteryFinishUi();
            applyLotteryTeamVisibility(ctx, 2);
            syncLotteryTeamCountButtons(2);
            for (let i = 1; i <= 4; i += 1) {
                applyLotterySourceUi(i);
                updateLotteryTeamTitle(i);
                updateLotteryTeamColorVisual(i);
            }
        }
        renderLotteryPreview();
    }

    async function refreshOfflineAll(ctx) {
        const status = await ctx.api("/api/status");
        const state = await ctx.api("/api/offline/state");
        ctx.ensureModePageAllowed("offline", status.mode);
        ctx.renderModeStatus(status);
        renderOfflineState(ctx, state);
        return { status: status, state: state };
    }

    async function saveLotterySettings($, postForm) {
        if (typeof postForm !== "function") {
            throw new Error("post_form_not_available");
        }
        const params = collectLotteryParams($);
        console.log("[offline.lottery] save params", {
            finish_source: params.lottery_finish_source,
            finish_value: params.lottery_finish_value
        });
        await postForm("/api/offline/config", params);
    }

    async function bind(ctx) {
        const $ = ctx.$;
        const setStatus = ctx.setStatus;
        const postQuery = ctx.postQuery;
        const Core = window.OrbAppCore || {};
        const postForm = Core.postForm;

        async function ensureLotterySubmode() {
            const submodeEl = $("offline-submode-select");
            const current = submodeEl ? String(submodeEl.value || "aura") : "aura";
            if (current === "lottery") {
                return;
            }
            await postQuery("/api/offline/submode", { name: "lottery" });
            if (submodeEl) {
                submodeEl.value = "lottery";
            }
        }

        ctx.bindModeTabs();
        ctx.bindSettingsTabs();
        ctx.bindTalkPanelIfAvailable();

        const refreshBtn = $("mode-refresh-btn");
        if (refreshBtn) {
            refreshBtn.addEventListener("click", async () => {
                try {
                    await refreshOfflineAll(ctx);
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
                        save: "1"
                    });
                    await refreshOfflineAll(ctx);
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
                        save: "1"
                    });
                    await refreshOfflineAll(ctx);
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
                        save: "1"
                    });
                    await refreshOfflineAll(ctx);
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
                    await ensureLotterySubmode();
                    await postQuery("/api/offline/action", { name: "lottery_start" });
                    await refreshOfflineAll(ctx);
                    setStatus("Lottery round start requested", false);
                } catch (e) {
                    setStatus("Lottery start failed: " + e.message, true);
                }
            });
        }

        const lotterySave = $("offline-save-lottery-btn");
        if (lotterySave) {
            lotterySave.addEventListener("click", async () => {
                try {
                    await saveLotterySettings($, postForm);
                    await refreshOfflineAll(ctx);
                    setStatus("Lottery settings saved", false);
                } catch (e) {
                    setStatus("Lottery save failed: " + e.message, true);
                }
            });
        }

        const lotterySaveStart = $("offline-save-start-lottery-btn");
        if (lotterySaveStart) {
            lotterySaveStart.addEventListener("click", async () => {
                try {
                    await saveLotterySettings($, postForm);
                    await ensureLotterySubmode();
                    await postQuery("/api/offline/action", { name: "lottery_start" });
                    await refreshOfflineAll(ctx);
                    setStatus("Lottery saved and round started", false);
                } catch (e) {
                    setStatus("Save + start failed: " + e.message, true);
                }
            });
        }

        const teamCountEl = $("offline-lottery-team-count");
        if (teamCountEl) {
            teamCountEl.addEventListener("input", () => {
                const count = Number(teamCountEl.value || 2);
                applyLotteryTeamVisibility(ctx, count);
                syncLotteryTeamCountButtons(count);
                renderLotteryPreview();
            });
        }

        const participantsEl = $("offline-lottery-participants-total");
        if (participantsEl) {
            participantsEl.addEventListener("input", renderLotteryPreview);
        }
        const audioModeEl = $("offline-lottery-audio-mode");
        if (audioModeEl) {
            audioModeEl.addEventListener("change", () => {
                for (let i = 1; i <= 4; i += 1) {
                    applyLotterySourceUi(i);
                }
            });
        }
        const finishSourceEl = $("offline-lottery-finish-source");
        if (finishSourceEl) {
            finishSourceEl.addEventListener("change", applyLotteryFinishUi);
            applyLotteryFinishUi();
        }

        const quickTeamButtons = Array.from(document.querySelectorAll("[data-lottery-team-count]"));
        quickTeamButtons.forEach((btn) => {
            btn.addEventListener("click", () => {
                const value = clampInt(btn.getAttribute("data-lottery-team-count"), 2, 4, 2);
                const input = $("offline-lottery-team-count");
                if (input) {
                    input.value = String(value);
                }
                applyLotteryTeamVisibility(ctx, value);
                syncLotteryTeamCountButtons(value);
                renderLotteryPreview();
            });
        });

        const autoNamesBtn = $("offline-lottery-fill-defaults-btn");
        if (autoNamesBtn) {
            autoNamesBtn.addEventListener("click", () => {
                const count = clampInt($("offline-lottery-team-count").value, 2, 4, 2);
                for (let i = 1; i <= count; i += 1) {
                    const ttsEl = $("offline-lottery-t" + i + "-tts");
                    if (ttsEl && !String(ttsEl.value || "").trim()) {
                        ttsEl.value = "Team " + i;
                    }
                    updateLotteryTeamTitle(i);
                }
            });
        }

        for (let i = 1; i <= 4; i += 1) {
            const srcEl = $("offline-lottery-t" + i + "-source");
            if (srcEl) {
                srcEl.addEventListener("change", () => applyLotterySourceUi(i));
            }
            const colorEl = $("offline-lottery-t" + i + "-color");
            if (colorEl) {
                colorEl.addEventListener("input", () => updateLotteryTeamColorVisual(i));
            }
            const ttsEl = $("offline-lottery-t" + i + "-tts");
            if (ttsEl) {
                ttsEl.addEventListener("input", () => updateLotteryTeamTitle(i));
            }
            updateLotteryTeamColorVisual(i);
        }

        await refreshOfflineAll(ctx);
    }

    window.OrbPageOffline = {
        bind: bind
    };
})();
