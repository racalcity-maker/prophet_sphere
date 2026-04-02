from __future__ import annotations

import json
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Optional


@dataclass
class VoskWord:
    word: str
    confidence: float


@dataclass
class VoskResult:
    text: str
    confidence: float
    words: list[VoskWord]


class VoskSession:
    def __init__(self, model_path: Path, sample_rate: int = 16000, shared_model: Any = None) -> None:
        self.model_path = Path(model_path)
        self.sample_rate = int(sample_rate)
        self._model = None
        self._rec = None
        self._enabled = False
        self._final_text = ""
        self._avg_conf = 0.0
        self._conf_count = 0
        self._words: list[VoskWord] = []
        if shared_model is not None:
            self._model = shared_model
            self._enabled = True
        else:
            self._init_model()

    @property
    def enabled(self) -> bool:
        return self._enabled

    def _init_model(self) -> None:
        try:
            from vosk import Model
        except Exception:
            self._enabled = False
            return
        if not self.model_path.exists():
            self._enabled = False
            return
        self._model = Model(str(self.model_path))
        self._enabled = True

    def start(self) -> bool:
        self._final_text = ""
        self._avg_conf = 0.0
        self._conf_count = 0
        self._words = []
        if not self._enabled or self._model is None:
            self._rec = None
            return False
        try:
            from vosk import KaldiRecognizer

            self._rec = KaldiRecognizer(self._model, float(self.sample_rate))
            try:
                self._rec.SetWords(True)
            except Exception:
                pass
            return True
        except Exception:
            self._rec = None
            return False

    def feed_pcm16(self, chunk: bytes) -> None:
        if self._rec is None or not chunk:
            return
        try:
            done = bool(self._rec.AcceptWaveform(chunk))
            if done:
                self._consume_json(self._rec.Result())
        except Exception:
            return

    def end(self) -> VoskResult:
        if self._rec is None:
            return VoskResult(text="", confidence=0.0, words=[])
        try:
            self._consume_json(self._rec.FinalResult())
        except Exception:
            pass
        text = self._final_text.strip()
        conf = self._avg_conf / self._conf_count if self._conf_count > 0 else 0.0
        conf = max(0.0, min(0.99, conf))
        return VoskResult(text=text, confidence=conf, words=list(self._words))

    def _consume_json(self, payload: str) -> None:
        try:
            data = json.loads(payload or "{}")
        except Exception:
            return
        text = str(data.get("text", "")).strip()
        if text:
            if self._final_text:
                self._final_text += " "
            self._final_text += text

        words = data.get("result")
        if isinstance(words, list):
            for item in words:
                if isinstance(item, dict):
                    w = str(item.get("word", "")).strip()
                    if w:
                        try:
                            wc = float(item.get("conf", 0.0))
                        except Exception:
                            wc = 0.0
                        self._words.append(VoskWord(word=w, confidence=max(0.0, min(0.99, wc))))
                if isinstance(item, dict) and "conf" in item:
                    try:
                        self._avg_conf += float(item.get("conf", 0.0))
                        self._conf_count += 1
                    except Exception:
                        continue


def make_vosk_session(model_path: Optional[str], sample_rate: int) -> Optional[VoskSession]:
    if not model_path:
        return None
    try:
        from vosk import Model, SetLogLevel
    except Exception:
        return None

    try:
        SetLogLevel(-1)
    except Exception:
        pass

    norm = str(Path(model_path).expanduser().resolve())
    if norm in _MODEL_CACHE:
        model = _MODEL_CACHE[norm]
    else:
        model_path_obj = Path(norm)
        if not model_path_obj.exists():
            return None
        model = Model(norm)
        _MODEL_CACHE[norm] = model

    session = VoskSession(Path(norm), sample_rate=sample_rate, shared_model=model)
    return session if session.enabled else None


_MODEL_CACHE: dict[str, Any] = {}
