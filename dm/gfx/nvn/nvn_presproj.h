// Presented-time selection for one drawable (pure function, shared by Mac / iOS; unit test tests/t_presproj.c).
// The system's presented time wins when there is one (> 0). A background test run's window sits off every screen, so the system reports 0 for every
// frame; with projection on (mtl_present_project, set only by the Mac front end in background runs) the frame's display-link target timestamp stands in
// for it, on the same CACurrentMediaTime base, so frame pacing reads the same vsync-quantized intervals as in front. Otherwise -1 = not presented.
#pragma once

static inline double nvn_presented_or_projected(double presented, int project, double target) {
    if (presented > 0) return presented;
    return project && target > 0 ? target : -1;
}
