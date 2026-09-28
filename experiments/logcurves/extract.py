#!/usr/bin/env python
"""Render the neutral ramp through Resolve's RCM once per log curve.

Run inside Resolve's scripting environment (or via an MCP `run_script` bridge) with
DaVinci Resolve Studio open. It creates its own scratch project, so nothing you are
working on is touched, and deletes it at the end.

WHY THIS EXISTS: `og::decode_log` had two curves transcribed wrong -- Canon Log 3 by
1.33 EV and DJI D-Log by 2.00 EV, the latter with a 47x step across its own knee -- and
neither was findable by reading the code. They were found by diffing against a second
implementation of the same transform, which is the only thing that has ever worked on
this project. Resolve is that second implementation, and it is also the one the user
A/Bs against with a CST node, so matching it is the goal rather than matching a spec
sheet where the two disagree.

THE GAMUT IS PINNED IDENTICAL in/timeline/out so the 3x3 is identity and only the
transfer function acts. Output gamma is DaVinci Intermediate: it holds ~9 stops over
mid-gray inside [0,1] so nothing clips, and we already own its exact inverse as cam 1.

RUN THE IDENTITY PASS FIRST and do not skip it. It renders DaVinci Intermediate ->
DaVinci Intermediate; if the ramp does not come back unchanged, every curve measured
afterwards is describing the harness rather than the curve. It measured 0.498 of a
16-bit LSB here.
"""

RAMP_PNG = "/tmp/ogcurves/ramp.png"   # from `make && ./makeramp /tmp/ogcurves/ramp.png`
OUT_DIR  = "/tmp/ogcurves"

# name -> the exact string Resolve's RCM accepts. SetSetting returns False for anything
# it does not know, which is also how you enumerate: probe candidates and keep what sticks.
# Resolve 21.1 has NO "DJI D-Log M" -- only D-Log and D-Log2 -- so D-Log M cannot be
# sourced this way and has to come from DJI's own LUT.
CURVES = [
    ("identity", "DaVinci Intermediate"),        # the harness check -- always run it
    ("cam00",    "Blackmagic Design Film Gen 5"),
    ("cam01",    "DaVinci Intermediate"),
    ("cam02",    "S-Log3"),
    ("cam03",    "ARRI LogC3"),
    ("cam04",    "ARRI LogC4"),
    ("cam05",    "Canon Log 3"),
    ("cam06",    "RED Log3G10"),
    ("cam07",    "DJI D-Log"),
    ("cam08",    "Fujifilm F-Log2"),
    ("cam09",    "Panasonic V-Log"),
    ("cam10",    "Rec.2100 HLG"),
    ("cam11",    "Rec.2100 ST2084"),
    ("gplog2",   "GoPro GP-Log2"),
]

PROJECT = "OneGrade-CS-Extract"


def main(resolve):
    import time
    pm = resolve.GetProjectManager()
    keep = pm.GetCurrentProject().GetName()

    pm.DeleteProject(PROJECT)
    p = pm.CreateProject(PROJECT)

    # exact-size timeline, so the ramp is never resampled on the way through
    for k, v in [("timelineResolutionWidth", "4096"), ("timelineResolutionHeight", "256"),
                 ("timelineOutputResolutionWidth", "4096"),
                 ("timelineOutputResolutionHeight", "256"), ("timelineFrameRate", "24")]:
        p.SetSetting(k, v)

    mp = p.GetMediaPool()
    items = resolve.GetMediaStorage().AddItemListToMediaPool([RAMP_PNG])
    assert items, "ramp not imported -- build it with ./makeramp first"
    mp.CreateTimelineFromClips("ramp-tl", [items[0]])

    p.SetSetting("colorScienceMode", "davinciYRGBColorManagedv2")
    p.SetSetting("separateColorSpaceAndGamma", "1")
    for k, v in [("colorSpaceInput", "Rec.709"), ("colorSpaceTimeline", "Rec.709"),
                 ("colorSpaceOutput", "Rec.709"), ("colorSpaceTimelineGamma", "DaVinci Intermediate"),
                 ("colorSpaceOutputGamma", "DaVinci Intermediate"),
                 ("inputDRT", "None"), ("outputDRT", "None")]:
        p.SetSetting(k, v)
    p.SetCurrentRenderFormatAndCodec("png", "RGB16")

    start = p.GetCurrentTimeline().GetStartFrame()
    report = {}
    for name, gamma in CURVES:
        if not p.SetSetting("colorSpaceInputGamma", gamma) or \
           p.GetSetting("colorSpaceInputGamma") != gamma:
            report[name] = "REJECTED: " + gamma
            continue
        p.SetRenderSettings({"TargetDir": OUT_DIR, "CustomName": name,
                             "MarkIn": start, "MarkOut": start,
                             "ExportVideo": True, "ExportAudio": False,
                             "FormatWidth": 4096, "FormatHeight": 256})
        p.DeleteAllRenderJobs()
        jid = p.AddRenderJob()
        p.StartRendering([jid], isInteractiveMode=False)
        while p.IsRenderingInProgress():
            time.sleep(0.2)
        report[name] = p.GetRenderJobStatus(jid).get("JobStatus")

    pm.LoadProject(keep)
    pm.DeleteProject(PROJECT)
    return report


if __name__ == "__main__":
    import DaVinciResolveScript as dvr  # provided by Resolve's scripting environment
    print(main(dvr.scriptapp("Resolve")))
