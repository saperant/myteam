"""myteam and myorg in the myrcp workbench (https://github.com/saperant/myrcp).

`myteam-ui [workbench options]` starts it; choose Perspective: myteam or myorg (Ctrl+P).
The plugins call the `myteam` and `myorg` executables: $MYTEAM / $MYORG, $PATH, else the
build/ of the checkout this package was installed from (pip install -e ui).
"""

from __future__ import annotations

from collections.abc import Sequence
from pathlib import Path

PRODUCT = Path(__file__).resolve().parent


def main(args: Sequence[str] | None = None) -> None:
    from myrcp.launch import run_product

    run_product(PRODUCT, None if args is None else list(args))
