"""转接：断言库已挪到 dm/verify/assertlib.py（事件流与日志红标守卫也在那里）；用例照旧 import lib。 | Shim: the assertion library moved to dm/verify/assertlib.py (with the event-stream helpers and the log red-marker guard); cases still import lib."""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from assertlib import AssertFail, Result, check, load, main, precondition  # noqa: E402,F401
