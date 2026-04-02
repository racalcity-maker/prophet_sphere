from __future__ import annotations

from typing import Optional, Protocol

from .contracts import ReasonerInput, ReasonerResult


class ReasonerBackend(Protocol):
    async def infer(self, data: ReasonerInput) -> Optional[ReasonerResult]:
        ...

