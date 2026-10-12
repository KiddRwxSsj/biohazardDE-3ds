# block profiler for emu.py: EXTRA=tools/prof.py PROF=start:frames, writes prof.txt into the run dir
_ps, _pn = [int(x) for x in os.environ.get('PROF', '1000:60').split(':')]
_blocks = collections.Counter()
_ohook = [None]
_d0 = [0]
_oswap = H_.get('eglSwapBuffers')
def _on_block(uc_, addr, size, ud):
    _blocks[addr] += size
def _swap(*a):
    r = _oswap(a[0], a[1]) if _oswap else 1
    if state.frames == _ps and _ohook[0] is None:
        print('[prof] start at frame', state.frames)
        _d0[0] = state.draws
        _ohook[0] = uc.hook_add(UC_HOOK_BLOCK, _on_block, begin=BASE, end=BASE + 0x200000)
        # blocks translated before the hook existed would never call it
        uc.ctl_flush_tb()
    if state.frames == _ps + _pn and _ohook[0] is not None:
        uc.hook_del(_ohook[0]); _ohook[0] = 0
        tot = sum(_blocks.values())
        with open(os.path.join(OUT, 'prof.txt'), 'w') as f:
            for a, n in _blocks.most_common(400):
                f.write(f'{a - BASE:06x} {n} {100.0 * n / tot:.2f}\n')
        print('[prof] done, total bytes executed', tot, 'draws per frame', (state.draws - _d0[0]) / _pn)
        state.quit = True; uc.emu_stop()
    return r
H_['eglSwapBuffers'] = _swap
