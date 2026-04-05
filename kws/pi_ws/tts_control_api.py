from __future__ import annotations

import asyncio
import contextlib
import json
from typing import Any, Dict
from urllib.parse import urlparse

try:
    from .tts_synth import list_tts_voices
except Exception:  # pragma: no cover
    from tts_synth import list_tts_voices  # type: ignore

_CONTROL_MAX_BODY_BYTES = 65536


def _bool_from_header_token(expected_token: str, provided: str) -> bool:
    if not expected_token:
        return True
    return provided.strip() == expected_token


def _http_json_response(
    status: int,
    payload: Dict[str, Any],
    *,
    allow_origin: str,
    methods: str = "GET,POST,OPTIONS",
) -> bytes:
    body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
    reason_map = {
        200: "OK",
        204: "No Content",
        400: "Bad Request",
        401: "Unauthorized",
        404: "Not Found",
        405: "Method Not Allowed",
        500: "Internal Server Error",
    }
    reason = reason_map.get(status, "OK")
    headers = [
        f"HTTP/1.1 {status} {reason}\r\n",
        "Content-Type: application/json; charset=utf-8\r\n",
        f"Content-Length: {len(body)}\r\n",
        "Connection: close\r\n",
        f"Access-Control-Allow-Origin: {allow_origin}\r\n",
        f"Access-Control-Allow-Methods: {methods}\r\n",
        "Access-Control-Allow-Headers: Content-Type, X-Orb-Token\r\n",
        "\r\n",
    ]
    return "".join(headers).encode("utf-8") + body


def _control_peer_name(writer: asyncio.StreamWriter) -> str:
    peer = writer.get_extra_info("peername")
    if isinstance(peer, tuple) and len(peer) >= 2:
        return f"{peer[0]}:{peer[1]}"
    return str(peer or "-")


def _control_patch_preview(patch: Dict[str, Any], *, limit: int = 6) -> str:
    if not patch:
        return "{}"
    keys = sorted(str(k) for k in patch.keys())
    preview_pairs: list[str] = []
    for key in keys[:limit]:
        val = patch.get(key)
        if isinstance(val, float):
            preview_pairs.append(f"{key}={val:.3f}")
        else:
            text = str(val)
            if len(text) > 40:
                text = text[:40] + "..."
            preview_pairs.append(f"{key}={text}")
    if len(keys) > limit:
        preview_pairs.append(f"...+{len(keys) - limit} keys")
    return ", ".join(preview_pairs)


async def start_tts_control_server(
    *,
    host: str,
    port: int,
    runtime_store: Any,
    allow_origin: str,
    auth_token: str,
) -> asyncio.base_events.Server:
    async def _handler(reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        try:
            head = await reader.readuntil(b"\r\n\r\n")
        except Exception:
            writer.close()
            await writer.wait_closed()
            return

        try:
            head_text = head.decode("utf-8", errors="replace")
            lines = head_text.split("\r\n")
            req_line = lines[0] if lines else ""
            parts = req_line.split(" ")
            if len(parts) < 2:
                writer.write(_http_json_response(400, {"ok": False, "error": "bad_request"}, allow_origin=allow_origin))
                await writer.drain()
                writer.close()
                await writer.wait_closed()
                return
            method = parts[0].upper().strip()
            full_path = parts[1].strip()
            path = urlparse(full_path).path
            peer = _control_peer_name(writer)

            headers: Dict[str, str] = {}
            for line in lines[1:]:
                if not line or ":" not in line:
                    continue
                k, v = line.split(":", 1)
                headers[k.strip().lower()] = v.strip()

            if method == "OPTIONS":
                writer.write(_http_json_response(204, {"ok": True}, allow_origin=allow_origin))
                await writer.drain()
                writer.close()
                await writer.wait_closed()
                return

            if auth_token:
                provided = headers.get("x-orb-token", "")
                if not _bool_from_header_token(auth_token, provided):
                    writer.write(_http_json_response(401, {"ok": False, "error": "unauthorized"}, allow_origin=allow_origin))
                    await writer.drain()
                    writer.close()
                    await writer.wait_closed()
                    return

            content_length = int(headers.get("content-length", "0") or "0")
            print(
                f"[tts_control] req {method} {path} peer={peer} "
                f"content_len={content_length}"
            )
            if content_length < 0 or content_length > _CONTROL_MAX_BODY_BYTES:
                print(
                    f"[tts_control] reject request_too_large peer={peer} "
                    f"content_len={content_length} max={_CONTROL_MAX_BODY_BYTES}"
                )
                writer.write(_http_json_response(400, {"ok": False, "error": "request_too_large"}, allow_origin=allow_origin))
                await writer.drain()
                writer.close()
                await writer.wait_closed()
                return

            body = b""
            if content_length > 0:
                body = await reader.readexactly(content_length)

            if path == "/api/tts/config":
                if method == "GET":
                    cfg = runtime_store.to_public_dict()
                    print(
                        f"[tts_control] get_config peer={peer} "
                        f"backend={cfg.get('tts_backend')} "
                        f"speaker={cfg.get('silero_speaker')} "
                        f"tempo={cfg.get('tts_tempo_scale')}"
                    )
                    payload = {"ok": True, "config": cfg}
                    writer.write(_http_json_response(200, payload, allow_origin=allow_origin))
                elif method == "POST":
                    if not body:
                        writer.write(_http_json_response(400, {"ok": False, "error": "missing_body"}, allow_origin=allow_origin))
                    else:
                        try:
                            patch = json.loads(body.decode("utf-8"))
                            if not isinstance(patch, dict):
                                raise ValueError("patch must be object")
                            print(
                                f"[tts_control] apply_patch peer={peer} "
                                f"keys={len(patch)} [{_control_patch_preview(patch)}]"
                            )
                            runtime_store.update_from_patch(patch)
                            cfg = runtime_store.to_public_dict()
                            print(
                                f"[tts_control] apply_ok peer={peer} "
                                f"backend={cfg.get('tts_backend')} "
                                f"speaker={cfg.get('silero_speaker')} "
                                f"tempo={cfg.get('tts_tempo_scale')} "
                                f"pause_ms={cfg.get('tts_phrase_pause_ms')} "
                                f"fx={cfg.get('tts_fx_preset')}"
                            )
                            payload = {"ok": True, "config": cfg}
                            writer.write(_http_json_response(200, payload, allow_origin=allow_origin))
                        except Exception as exc:
                            print(f"[tts_control] apply_failed peer={peer} err={exc}")
                            writer.write(
                                _http_json_response(
                                    400,
                                    {"ok": False, "error": "invalid_config_patch", "detail": str(exc)[:240]},
                                    allow_origin=allow_origin,
                                )
                            )
                else:
                    writer.write(_http_json_response(405, {"ok": False, "error": "method_not_allowed"}, allow_origin=allow_origin))
            elif path == "/api/tts/config/save":
                if method != "POST":
                    writer.write(_http_json_response(405, {"ok": False, "error": "method_not_allowed"}, allow_origin=allow_origin))
                else:
                    try:
                        runtime_store.save()
                        cfg = runtime_store.to_public_dict()
                        print(
                            f"[tts_control] save_ok peer={peer} file={cfg.get('config_file')} "
                            f"backend={cfg.get('tts_backend')} speaker={cfg.get('silero_speaker')}"
                        )
                        writer.write(_http_json_response(200, {"ok": True, "saved": True, "config": cfg}, allow_origin=allow_origin))
                    except Exception as exc:
                        print(f"[tts_control] save_failed peer={peer} err={exc}")
                        writer.write(_http_json_response(500, {"ok": False, "error": "save_failed", "detail": str(exc)[:240]}, allow_origin=allow_origin))
            elif path == "/api/tts/config/reload":
                if method != "POST":
                    writer.write(_http_json_response(405, {"ok": False, "error": "method_not_allowed"}, allow_origin=allow_origin))
                else:
                    try:
                        runtime_store.load()
                        cfg = runtime_store.to_public_dict()
                        print(
                            f"[tts_control] reload_ok peer={peer} file={cfg.get('config_file')} "
                            f"backend={cfg.get('tts_backend')} speaker={cfg.get('silero_speaker')}"
                        )
                        writer.write(_http_json_response(200, {"ok": True, "reloaded": True, "config": cfg}, allow_origin=allow_origin))
                    except FileNotFoundError:
                        print(f"[tts_control] reload_failed peer={peer} err=config_file_not_found")
                        writer.write(_http_json_response(404, {"ok": False, "error": "config_file_not_found"}, allow_origin=allow_origin))
                    except Exception as exc:
                        print(f"[tts_control] reload_failed peer={peer} err={exc}")
                        writer.write(_http_json_response(400, {"ok": False, "error": "reload_failed", "detail": str(exc)[:240]}, allow_origin=allow_origin))
            elif path == "/api/tts/voices":
                if method != "GET":
                    writer.write(_http_json_response(405, {"ok": False, "error": "method_not_allowed"}, allow_origin=allow_origin))
                else:
                    try:
                        snap = runtime_store.snapshot()
                        voices = await asyncio.to_thread(list_tts_voices, snap.tts_cfg)
                        silero_n = len((voices.get("silero", {}) or {}).get("speakers", []) or [])
                        piper_n = len((voices.get("piper", {}) or {}).get("models", []) or [])
                        print(
                            f"[tts_control] voices peer={peer} "
                            f"silero={silero_n} piper={piper_n}"
                        )
                        writer.write(_http_json_response(200, {"ok": True, "voices": voices}, allow_origin=allow_origin))
                    except Exception as exc:
                        print(f"[tts_control] voices_failed peer={peer} err={exc}")
                        writer.write(_http_json_response(500, {"ok": False, "error": "voices_failed", "detail": str(exc)[:240]}, allow_origin=allow_origin))
            elif path == "/health":
                writer.write(_http_json_response(200, {"ok": True, "service": "tts_control"}, allow_origin=allow_origin))
            else:
                writer.write(_http_json_response(404, {"ok": False, "error": "not_found"}, allow_origin=allow_origin))

            await writer.drain()
        except Exception as exc:
            try:
                writer.write(_http_json_response(500, {"ok": False, "error": "internal_error", "detail": str(exc)[:240]}, allow_origin=allow_origin))
                await writer.drain()
            except Exception:
                pass
        finally:
            writer.close()
            with contextlib.suppress(Exception):
                await writer.wait_closed()

    server = await asyncio.start_server(_handler, host=host, port=port)
    return server

