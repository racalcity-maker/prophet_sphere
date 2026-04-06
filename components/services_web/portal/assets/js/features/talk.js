(function () {
    "use strict";

    const Core = window.OrbAppCore || {};
    const $ = Core.$ || function (id) { return document.getElementById(id); };
    const postFormCore = Core.postForm;

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

    async function talkLiveStopInternal(reason, postStopCommand, setStatus) {
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
        } else if (typeof setStatus === "function") {
            setStatus(reason || "Live stream stopped", false);
        }
    }

    async function talkLiveStart(setStatus) {
        const st = talkLive;
        if (st.active) {
            return;
        }
        st.onStatus = setStatus;

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
                    talkLiveStopInternal("Live stream closed", false, setStatus).catch(() => { });
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

    function bindPanel(options) {
        const opts = options || {};
        const setStatus = opts.setStatus || function () { };
        const postForm = opts.postForm || postFormCore;

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
                const withBg = !!($("talk-say-with-bg") && $("talk-say-with-bg").checked);
                if (!text) {
                    setStatus("Talk text is required", true);
                    return;
                }
                sayBtn.disabled = true;
                try {
                    await postForm("/api/talk/say", {
                        text: text,
                        timeout_ms: timeout,
                        with_bg: withBg ? "1" : "0",
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
                    await talkLiveStopInternal("Live mic stream stopped", true, setStatus);
                } catch (e) {
                    setStatus("Live stop failed: " + e.message, true);
                } finally {
                    talkLiveSetUi(false);
                }
            });
        }

        window.addEventListener("beforeunload", () => {
            if (talkLive.active) {
                talkLiveStopInternal("", true, setStatus).catch(() => { });
            }
        });
    }

    window.OrbTalkFeature = {
        bindPanel: bindPanel,
        stop: function () { return talkLiveStopInternal("", true, null); }
    };
})();
