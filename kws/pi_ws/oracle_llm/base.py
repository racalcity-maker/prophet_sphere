from __future__ import annotations

from typing import Optional, Protocol

from .contracts import OracleLlmInput, OracleLlmScript


class OracleLlmBackend(Protocol):
    async def generate(self, data: OracleLlmInput) -> Optional[OracleLlmScript]:
        ...

