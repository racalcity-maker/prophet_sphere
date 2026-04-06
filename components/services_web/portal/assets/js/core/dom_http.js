(function () {
    "use strict";

    function $(id) {
        return document.getElementById(id);
    }

    async function api(path, method) {
        const doFetch = async () => fetch(path, { method: method || "GET", cache: "no-store" });
        let resp;
        try {
            resp = await doFetch();
        } catch (e) {
            // ESP HTTPS can transiently drop sockets under load; one quick retry helps UI stability.
            await new Promise((resolve) => setTimeout(resolve, 120));
            resp = await doFetch();
        }
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

    function valueFromInput(id, fallback) {
        const el = $(id);
        if (!el || el.value === undefined || el.value === null || String(el.value).trim() === "") {
            return fallback;
        }
        return String(el.value);
    }

    function setBlockVisible(id, visible) {
        const el = $(id);
        if (!el) {
            return;
        }
        el.style.display = visible ? "" : "none";
    }

    function parseIntBounded(raw, min, max, fallback) {
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
    }

    function bindRangeNumberPair(rangeId, valueId, min, max) {
        const rangeEl = $(rangeId);
        const valueEl = $(valueId);
        if (!rangeEl || !valueEl) {
            return;
        }
        const syncRangeToValue = () => {
            valueEl.value = rangeEl.value;
        };
        const syncValueToRange = () => {
            const v = parseIntBounded(valueEl.value, min, max, min);
            valueEl.value = String(v);
            rangeEl.value = String(v);
        };
        rangeEl.addEventListener("input", syncRangeToValue);
        valueEl.addEventListener("input", syncValueToRange);
    }

    window.OrbAppCore = {
        $,
        api,
        postQuery,
        postForm,
        setStatus,
        setText,
        setValue,
        setChecked,
        valueFromInput,
        setBlockVisible,
        parseIntBounded,
        bindRangeNumberPair
    };
})();
