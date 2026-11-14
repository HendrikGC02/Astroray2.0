import re, sys, json
for s in ("S1", "S2"):
    j = json.load(open(f"out/gui_cycles_viewport_{s}.json"))
    ts = j["t_switch_since_process_start_s"]
    lines = open(f"out/gui_cycles_viewport_{s}.log", encoding="utf-8", errors="replace").read().splitlines()
    ev = []
    for ln in lines:
        m = re.match(r"(\d+):(\d+\.\d+)\s+(\w+)\s+\| (.*)", ln)
        if m:
            ev.append((int(m.group(1)) * 60 + float(m.group(2)), m.group(4)))
    keep = [e for e in ev if e[0] >= ts - 0.5 and ("Total time spent synchronizing" in e[1] or "Rendered" in e[1] or "Shader manager" in e[1] or "Using OPTIX layout" in e[1] or "lookup tables" in e[1] or "full device sync" in e[1] or "interop" in e[1].lower())]
    print(s, "switch at", round(ts, 3), "s since launch")
    for t, m in keep[:14]:
        print("  +%.3f s  %s" % (t - ts, m[:90]))
