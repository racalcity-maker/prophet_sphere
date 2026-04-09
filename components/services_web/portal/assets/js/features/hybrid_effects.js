(function () {
    "use strict";

    const core = window.OrbAppCore || {};
    const $ = core.$ || function (id) { return document.getElementById(id); };
    const setValue = core.setValue || function () { };
    const setText = core.setText || function () { };
    const valueFromInput = core.valueFromInput || function (_id, fallback) { return fallback; };
    const setBlockVisible = core.setBlockVisible || function () { };
    const parseIntBounded = core.parseIntBounded || function (raw, min, max, fallback) {
        let v = Number(raw);
        if (!Number.isFinite(v)) {
            v = fallback;
        }
        v = Math.round(v);
        if (v < min) {
            v = min;
        } else if (v > max) {
            v = max;
        }
        return v;
    };
    const bindRangeNumberPair = core.bindRangeNumberPair || function () { };

    const DEFAULTS = {
        idle_scene_id: 12,
        talk_scene_id: 14,
        speed: 170,
        intensity: 180,
        scale: 140,
        palette_mode: 0,
        color1: "#ff00b4",
        color2: "#00e6ff",
        color3: "#ffbe28"
    };
    const TARGET_DEFAULT = "idle";

    const PALETTE_MODE = {
        rainbow: 0,
        duo: 1,
        trio: 2
    };

    const PROFILES = {
        "12": { name: "hybrid idle slow breathe", hasParams: false },
        "13": { name: "hybrid touch fast breathe", hasParams: false },
        "14": { name: "internal vortex", hasParams: false },
        "15": { name: "internal vortex dim", hasParams: false },
        "16": { name: "wled metaballs", hasParams: true },
        "17": { name: "wled dna", hasParams: true },
        "18": { name: "wled twister", hasParams: true },
        "19": { name: "wled checker pulse", hasParams: true },
        "20": { name: "wled rain", hasParams: true },
        "21": { name: "wled radial burst", hasParams: true },
        "22": { name: "wled tunnel", hasParams: true },
        "23": { name: "wled bands", hasParams: true },
        "24": { name: "wled starfield", hasParams: true },
        "25": { name: "wled confetti", hasParams: true },
        "26": { name: "wled lava", hasParams: true },
        "27": { name: "wled rings", hasParams: true },
        "28": { name: "wled noise", hasParams: true },
        "29": { name: "wled scanner", hasParams: true },
        "30": { name: "wled zigzag", hasParams: true },
        "31": { name: "wled aurora", hasParams: true },
        "32": { name: "wled prism", hasParams: true },
        "33": { name: "wled clouds", hasParams: true },
        "34": { name: "wled wavegrid", hasParams: true },
        "35": { name: "wled heartbeat", hasParams: true },
        "36": { name: "wled pinwheel", hasParams: true },
        "37": { name: "wled comet", hasParams: true }
    };

    const state = {
        idleSceneId: DEFAULTS.idle_scene_id,
        talkSceneId: DEFAULTS.talk_scene_id,
        speed: DEFAULTS.speed,
        intensity: DEFAULTS.intensity,
        scale: DEFAULTS.scale,
        paletteMode: DEFAULTS.palette_mode,
        color1: DEFAULTS.color1,
        color2: DEFAULTS.color2,
        color3: DEFAULTS.color3
    };

    function profile(sceneId) {
        const key = String(sceneId || DEFAULTS.talk_scene_id);
        if (Object.prototype.hasOwnProperty.call(PROFILES, key)) {
            return PROFILES[key];
        }
        return PROFILES[String(DEFAULTS.talk_scene_id)];
    }

    function normalizeColorHex(raw, fallback) {
        const input = String(raw || "").trim().toLowerCase();
        const base = (typeof fallback === "string" && fallback.length === 7) ? fallback : "#000000";
        if (/^#[0-9a-f]{6}$/.test(input)) {
            return input;
        }
        if (/^[0-9a-f]{6}$/.test(input)) {
            return "#" + input;
        }
        return base;
    }

    function hexToRgb(hex) {
        const color = normalizeColorHex(hex, "#000000").slice(1);
        return {
            r: parseInt(color.slice(0, 2), 16),
            g: parseInt(color.slice(2, 4), 16),
            b: parseInt(color.slice(4, 6), 16)
        };
    }

    function rgbToHex(r, g, b, fallback) {
        const cr = parseIntBounded(r, 0, 255, 0);
        const cg = parseIntBounded(g, 0, 255, 0);
        const cb = parseIntBounded(b, 0, 255, 0);
        const text = "#" + [cr, cg, cb].map((v) => v.toString(16).padStart(2, "0")).join("");
        return normalizeColorHex(text, fallback);
    }

    function setColorControl(colorInputId, hexInputId, colorValue, fallback) {
        const normalized = normalizeColorHex(colorValue, fallback);
        const colorInput = $(colorInputId);
        const hexInput = $(hexInputId);
        if (colorInput) {
            colorInput.value = normalized;
        }
        if (hexInput) {
            hexInput.value = normalized.toUpperCase();
        }
        return normalized;
    }

    function readColorControl(colorInputId, hexInputId, fallback) {
        const colorInput = $(colorInputId);
        const hexInput = $(hexInputId);
        const fromHex = hexInput ? hexInput.value : "";
        const fromColor = colorInput ? colorInput.value : "";
        return normalizeColorHex(fromHex || fromColor, fallback);
    }

    function setPaletteControlsEnabled(enabled) {
        const ids = [
            "mode-hybrid-effect-color1",
            "mode-hybrid-effect-color1-hex",
            "mode-hybrid-effect-color2",
            "mode-hybrid-effect-color2-hex",
            "mode-hybrid-effect-color3",
            "mode-hybrid-effect-color3-hex"
        ];
        ids.forEach((id) => {
            const el = $(id);
            if (el) {
                el.disabled = !enabled;
            }
        });
    }

    function updatePalettePreview() {
        const c1 = normalizeColorHex(state.color1, DEFAULTS.color1).toUpperCase();
        const c2 = normalizeColorHex(state.color2, DEFAULTS.color2).toUpperCase();
        const c3 = normalizeColorHex(state.color3, DEFAULTS.color3).toUpperCase();
        const mode = parseIntBounded(state.paletteMode, 0, 2, DEFAULTS.palette_mode);

        const preview = $("mode-hybrid-effect-palette-preview");
        const dot1 = $("mode-hybrid-effect-dot1");
        const dot2 = $("mode-hybrid-effect-dot2");
        const dot3 = $("mode-hybrid-effect-dot3");
        const hex1 = $("mode-hybrid-effect-hex1");
        const hex2 = $("mode-hybrid-effect-hex2");
        const hex3 = $("mode-hybrid-effect-hex3");
        const legend3 = $("mode-hybrid-effect-legend3");

        if (dot1) {
            dot1.style.background = c1;
        }
        if (dot2) {
            dot2.style.background = c2;
        }
        if (dot3) {
            dot3.style.background = c3;
        }
        if (hex1) {
            hex1.textContent = c1;
        }
        if (hex2) {
            hex2.textContent = c2;
        }
        if (hex3) {
            hex3.textContent = c3;
        }

        if (preview) {
            if (mode === PALETTE_MODE.rainbow) {
                preview.style.background = "linear-gradient(90deg,#ff0040 0%,#ff8a00 16%,#ffd400 32%,#00d26a 48%,#00b8ff 64%,#5a78ff 80%,#c93cff 100%)";
            } else if (mode === PALETTE_MODE.duo) {
                preview.style.background = "linear-gradient(90deg," + c1 + " 0%," + c2 + " 100%)";
            } else {
                preview.style.background = "linear-gradient(90deg," + c1 + " 0%," + c2 + " 50%," + c3 + " 100%)";
            }
        }

        setBlockVisible("mode-hybrid-effect-legend3", mode === PALETTE_MODE.trio);
        setPaletteControlsEnabled(mode !== PALETTE_MODE.rainbow);
    }

    function getTarget() {
        const target = valueFromInput("mode-hybrid-effect-target", TARGET_DEFAULT).trim().toLowerCase();
        return target === "talk" ? "talk" : "idle";
    }

    function getSceneForTarget(target) {
        return target === "talk" ? state.talkSceneId : state.idleSceneId;
    }

    function updateUiVisibility() {
        const sceneEl = $("mode-hybrid-effect-scene");
        if (!sceneEl) {
            return;
        }
        const target = getTarget();
        const targetSceneId = getSceneForTarget(target);
        sceneEl.value = String(targetSceneId);
        const p = profile(targetSceneId);
        const profileEl = $("mode-hybrid-effect-profile");
        if (profileEl) {
            profileEl.value = p.name;
        }
        const paletteMode = parseIntBounded(state.paletteMode, 0, 2, DEFAULTS.palette_mode);
        setBlockVisible("mode-hybrid-effect-color3-row", paletteMode === PALETTE_MODE.trio);
        setBlockVisible("mode-hybrid-effect-params-block", !!p.hasParams);
        updatePalettePreview();
    }

    function applyConfig(cfg) {
        state.idleSceneId = parseIntBounded(
            cfg && cfg.hybrid_effect_idle_scene_id,
            0, 65535, DEFAULTS.idle_scene_id
        );
        state.talkSceneId = parseIntBounded(
            cfg && cfg.hybrid_effect_talk_scene_id,
            0, 65535, DEFAULTS.talk_scene_id
        );
        state.speed = parseIntBounded(
            cfg && cfg.hybrid_effect_speed,
            0, 255, DEFAULTS.speed
        );
        state.intensity = parseIntBounded(
            cfg && cfg.hybrid_effect_intensity,
            0, 255, DEFAULTS.intensity
        );
        state.scale = parseIntBounded(
            cfg && cfg.hybrid_effect_scale,
            0, 255, DEFAULTS.scale
        );
        state.paletteMode = parseIntBounded(
            cfg && cfg.hybrid_effect_palette_mode,
            0, 2, DEFAULTS.palette_mode
        );
        state.color1 = rgbToHex(
            cfg && cfg.hybrid_effect_color1_r,
            cfg && cfg.hybrid_effect_color1_g,
            cfg && cfg.hybrid_effect_color1_b,
            DEFAULTS.color1
        );
        state.color2 = rgbToHex(
            cfg && cfg.hybrid_effect_color2_r,
            cfg && cfg.hybrid_effect_color2_g,
            cfg && cfg.hybrid_effect_color2_b,
            DEFAULTS.color2
        );
        state.color3 = rgbToHex(
            cfg && cfg.hybrid_effect_color3_r,
            cfg && cfg.hybrid_effect_color3_g,
            cfg && cfg.hybrid_effect_color3_b,
            DEFAULTS.color3
        );

        setValue("mode-hybrid-effect-speed-range", state.speed, DEFAULTS.speed);
        setValue("mode-hybrid-effect-speed-value", state.speed, DEFAULTS.speed);
        setValue("mode-hybrid-effect-intensity-range", state.intensity, DEFAULTS.intensity);
        setValue("mode-hybrid-effect-intensity-value", state.intensity, DEFAULTS.intensity);
        setValue("mode-hybrid-effect-scale-range", state.scale, DEFAULTS.scale);
        setValue("mode-hybrid-effect-scale-value", state.scale, DEFAULTS.scale);
        setValue("mode-hybrid-effect-palette-mode", state.paletteMode, DEFAULTS.palette_mode);
        state.color1 = setColorControl(
            "mode-hybrid-effect-color1",
            "mode-hybrid-effect-color1-hex",
            state.color1,
            DEFAULTS.color1
        );
        state.color2 = setColorControl(
            "mode-hybrid-effect-color2",
            "mode-hybrid-effect-color2-hex",
            state.color2,
            DEFAULTS.color2
        );
        state.color3 = setColorControl(
            "mode-hybrid-effect-color3",
            "mode-hybrid-effect-color3-hex",
            state.color3,
            DEFAULTS.color3
        );

        const targetEl = $("mode-hybrid-effect-target");
        if (targetEl && targetEl.value !== "idle" && targetEl.value !== "talk") {
            targetEl.value = TARGET_DEFAULT;
        }

        setText(
            "mode-hybrid-effect-current",
            String(state.idleSceneId) + " / " +
                String(state.talkSceneId) + " / " +
                String(state.speed) + " / " +
                String(state.intensity) + " / " +
                String(state.scale) + " / " +
                String(state.paletteMode),
            "-"
        );
        updateUiVisibility();
    }

    function collectValues() {
        const sceneEl = $("mode-hybrid-effect-scene");
        const speedRange = $("mode-hybrid-effect-speed-range");
        const speedValue = $("mode-hybrid-effect-speed-value");
        const intensityRange = $("mode-hybrid-effect-intensity-range");
        const intensityValue = $("mode-hybrid-effect-intensity-value");
        const scaleRange = $("mode-hybrid-effect-scale-range");
        const scaleValue = $("mode-hybrid-effect-scale-value");
        const paletteModeEl = $("mode-hybrid-effect-palette-mode");
        const target = getTarget();
        const selectedSceneId = parseIntBounded(
            sceneEl ? sceneEl.value : getSceneForTarget(target),
            0, 65535, target === "talk" ? DEFAULTS.talk_scene_id : DEFAULTS.idle_scene_id
        );
        if (target === "talk") {
            state.talkSceneId = selectedSceneId;
        } else {
            state.idleSceneId = selectedSceneId;
        }

        const speed = parseIntBounded(
            speedValue ? speedValue.value : (speedRange ? speedRange.value : DEFAULTS.speed),
            0, 255, DEFAULTS.speed
        );
        const intensity = parseIntBounded(
            intensityValue ? intensityValue.value : (intensityRange ? intensityRange.value : DEFAULTS.intensity),
            0, 255, DEFAULTS.intensity
        );
        const scale = parseIntBounded(
            scaleValue ? scaleValue.value : (scaleRange ? scaleRange.value : DEFAULTS.scale),
            0, 255, DEFAULTS.scale
        );
        const paletteMode = parseIntBounded(
            paletteModeEl ? paletteModeEl.value : state.paletteMode,
            0, 2, DEFAULTS.palette_mode
        );

        if (speedRange) {
            speedRange.value = String(speed);
        }
        if (speedValue) {
            speedValue.value = String(speed);
        }
        if (intensityRange) {
            intensityRange.value = String(intensity);
        }
        if (intensityValue) {
            intensityValue.value = String(intensity);
        }
        if (scaleRange) {
            scaleRange.value = String(scale);
        }
        if (scaleValue) {
            scaleValue.value = String(scale);
        }
        if (paletteModeEl) {
            paletteModeEl.value = String(paletteMode);
        }

        state.speed = speed;
        state.intensity = intensity;
        state.scale = scale;
        state.paletteMode = paletteMode;
        state.color1 = readColorControl("mode-hybrid-effect-color1", "mode-hybrid-effect-color1-hex", DEFAULTS.color1);
        state.color2 = readColorControl("mode-hybrid-effect-color2", "mode-hybrid-effect-color2-hex", DEFAULTS.color2);
        state.color3 = readColorControl("mode-hybrid-effect-color3", "mode-hybrid-effect-color3-hex", DEFAULTS.color3);

        state.color1 = setColorControl("mode-hybrid-effect-color1", "mode-hybrid-effect-color1-hex", state.color1, DEFAULTS.color1);
        state.color2 = setColorControl("mode-hybrid-effect-color2", "mode-hybrid-effect-color2-hex", state.color2, DEFAULTS.color2);
        state.color3 = setColorControl("mode-hybrid-effect-color3", "mode-hybrid-effect-color3-hex", state.color3, DEFAULTS.color3);
        updatePalettePreview();

        const c1 = hexToRgb(state.color1);
        const c2 = hexToRgb(state.color2);
        const c3 = hexToRgb(state.color3);

        return {
            target: target,
            selectedSceneId: selectedSceneId,
            idleSceneId: state.idleSceneId,
            talkSceneId: state.talkSceneId,
            speed: state.speed,
            intensity: state.intensity,
            scale: state.scale,
            paletteMode: state.paletteMode,
            c1: c1,
            c2: c2,
            c3: c3
        };
    }

    function bind(options) {
        const opts = options || {};
        const isHybridPage = !!opts.isHybridPage;
        const setStatus = opts.setStatus || function () { };
        const postQuery = opts.postQuery || core.postQuery;
        const refreshNetworkSetupState = opts.refreshNetworkSetupState;
        const pageName = opts.pageName || "hybrid";

        const targetEl = $("mode-hybrid-effect-target");
        const sceneEl = $("mode-hybrid-effect-scene");
        const paletteModeEl = $("mode-hybrid-effect-palette-mode");
        const previewBtn = $("mode-hybrid-effect-preview-btn");
        const applyBtn = $("mode-hybrid-effect-apply-btn");

        bindRangeNumberPair("mode-hybrid-effect-speed-range", "mode-hybrid-effect-speed-value", 0, 255);
        bindRangeNumberPair("mode-hybrid-effect-intensity-range", "mode-hybrid-effect-intensity-value", 0, 255);
        bindRangeNumberPair("mode-hybrid-effect-scale-range", "mode-hybrid-effect-scale-value", 0, 255);

        if (targetEl) {
            targetEl.addEventListener("change", () => {
                updateUiVisibility();
            });
        }
        if (sceneEl) {
            sceneEl.addEventListener("change", () => {
                const target = getTarget();
                const selectedSceneId = parseIntBounded(
                    sceneEl.value,
                    0, 65535, target === "talk" ? DEFAULTS.talk_scene_id : DEFAULTS.idle_scene_id
                );
                if (target === "talk") {
                    state.talkSceneId = selectedSceneId;
                } else {
                    state.idleSceneId = selectedSceneId;
                }
                updateUiVisibility();
            });
        }
        if (paletteModeEl) {
            paletteModeEl.addEventListener("change", () => {
                state.paletteMode = parseIntBounded(paletteModeEl.value, 0, 2, DEFAULTS.palette_mode);
                updateUiVisibility();
            });
        }

        function bindColorPair(colorId, hexId, fallback, stateKey) {
            const colorEl = $(colorId);
            const hexEl = $(hexId);
            if (colorEl) {
                colorEl.addEventListener("input", () => {
                    const normalized = setColorControl(colorId, hexId, colorEl.value, fallback);
                    state[stateKey] = normalized;
                    updatePalettePreview();
                });
            }
            if (hexEl) {
                hexEl.addEventListener("input", () => {
                    const normalized = normalizeColorHex(hexEl.value, fallback);
                    state[stateKey] = normalized;
                    setColorControl(colorId, hexId, normalized, fallback);
                    updatePalettePreview();
                });
                hexEl.addEventListener("blur", () => {
                    const normalized = normalizeColorHex(hexEl.value, fallback);
                    state[stateKey] = normalized;
                    setColorControl(colorId, hexId, normalized, fallback);
                    updatePalettePreview();
                });
            }
        }

        bindColorPair("mode-hybrid-effect-color1", "mode-hybrid-effect-color1-hex", DEFAULTS.color1, "color1");
        bindColorPair("mode-hybrid-effect-color2", "mode-hybrid-effect-color2-hex", DEFAULTS.color2, "color2");
        bindColorPair("mode-hybrid-effect-color3", "mode-hybrid-effect-color3-hex", DEFAULTS.color3, "color3");

        if (previewBtn) {
            previewBtn.addEventListener("click", async () => {
                if (!isHybridPage) {
                    setStatus("Effects tab is available only in hybrid mode", true);
                    return;
                }
                if (!postQuery) {
                    setStatus("Effect preview API unavailable", true);
                    return;
                }
                const fx = collectValues();
                try {
                    await postQuery("/api/led/effect", {
                        scene: fx.selectedSceneId,
                        duration_ms: 0,
                        speed: fx.speed,
                        intensity: fx.intensity,
                        scale: fx.scale,
                        palette_mode: fx.paletteMode,
                        palette_c1_r: fx.c1.r,
                        palette_c1_g: fx.c1.g,
                        palette_c1_b: fx.c1.b,
                        palette_c2_r: fx.c2.r,
                        palette_c2_g: fx.c2.g,
                        palette_c2_b: fx.c2.b,
                        palette_c3_r: fx.c3.r,
                        palette_c3_g: fx.c3.g,
                        palette_c3_b: fx.c3.b
                    });
                    setStatus("Selected effect preview started (" + fx.target + ")", false);
                } catch (e) {
                    setStatus("Effect preview failed: " + e.message, true);
                }
            });
        }

        if (applyBtn) {
            applyBtn.addEventListener("click", async () => {
                if (!isHybridPage) {
                    setStatus("Effects tab is available only in hybrid mode", true);
                    return;
                }
                if (!postQuery) {
                    setStatus("Effect apply API unavailable", true);
                    return;
                }
                const fx = collectValues();
                try {
                    await postQuery("/api/config", {
                        hybrid_effect_idle_scene_id: fx.idleSceneId,
                        hybrid_effect_talk_scene_id: fx.talkSceneId,
                        hybrid_effect_speed: fx.speed,
                        hybrid_effect_intensity: fx.intensity,
                        hybrid_effect_scale: fx.scale,
                        hybrid_effect_palette_mode: fx.paletteMode,
                        hybrid_effect_color1_r: fx.c1.r,
                        hybrid_effect_color1_g: fx.c1.g,
                        hybrid_effect_color1_b: fx.c1.b,
                        hybrid_effect_color2_r: fx.c2.r,
                        hybrid_effect_color2_g: fx.c2.g,
                        hybrid_effect_color2_b: fx.c2.b,
                        hybrid_effect_color3_r: fx.c3.r,
                        hybrid_effect_color3_g: fx.c3.g,
                        hybrid_effect_color3_b: fx.c3.b,
                        save: "1"
                    });
                    await postQuery("/api/led/effect", {
                        scene: fx.selectedSceneId,
                        duration_ms: 0,
                        speed: fx.speed,
                        intensity: fx.intensity,
                        scale: fx.scale,
                        palette_mode: fx.paletteMode,
                        palette_c1_r: fx.c1.r,
                        palette_c1_g: fx.c1.g,
                        palette_c1_b: fx.c1.b,
                        palette_c2_r: fx.c2.r,
                        palette_c2_g: fx.c2.g,
                        palette_c2_b: fx.c2.b,
                        palette_c3_r: fx.c3.r,
                        palette_c3_g: fx.c3.g,
                        palette_c3_b: fx.c3.b
                    });
                    if (typeof refreshNetworkSetupState === "function") {
                        await refreshNetworkSetupState(pageName);
                    }
                    setStatus("Hybrid effect + palette applied and saved for " + fx.target, false);
                } catch (e) {
                    setStatus("Hybrid effect apply failed: " + e.message, true);
                }
            });
        }
    }

    window.OrbHybridEffects = {
        defaults: DEFAULTS,
        profiles: PROFILES,
        state: state,
        applyConfig: applyConfig,
        updateUiVisibility: updateUiVisibility,
        bind: bind
    };
})();
