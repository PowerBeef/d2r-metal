"""Wait for a quiet machine before a CPU-heavy tool starts: while the Mac game lock is held (a gate's game tiers, a benchmark, a front
scenario run), a heavy job beside it changes what those runs measure and has turned timing-sensitive gate steps red. Tools that take seconds
of full CPU call wait_quiet() first. DM_QUIET_SKIP=1 skips the wait (the caller is the lock's holder, or knows better)."""
import os
import sys
import time

LOCK = os.path.join(os.environ.get('DM_ROOT') or os.path.expanduser('~/dm-work'), 'run', 'ios', 'macsmoke.lock')


def holder(lock=LOCK):
    """The live holder of the Mac game lock as (pid, owner text), or None."""
    try:
        with open(os.path.join(lock, 'pid'), encoding='utf-8') as f: pid = int(f.read().strip())
        os.kill(pid, 0)
    except (OSError, ValueError): return None
    try:
        with open(os.path.join(lock, 'owner'), encoding='utf-8') as f: owner = f.read().strip()
    except OSError: owner = '?'
    return pid, owner


def wait_quiet(what, lock=LOCK, poll=5.0, sleep=time.sleep):
    """Returns the seconds waited. Never waits when the lock is ours (XT_LOCK_HELD=1) or DM_QUIET_SKIP=1."""
    if os.environ.get('DM_QUIET_SKIP') == '1' or os.environ.get('XT_LOCK_HELD') == '1': return 0.0
    waited = 0.0; told = False
    while (h := holder(lock)) is not None:
        if not told: print(f'{what}: waiting, the Mac game lock is held by "{h[1]}" (pid {h[0]}); DM_QUIET_SKIP=1 starts now', file=sys.stderr, flush=True); told = True
        sleep(poll); waited += poll
    return waited
