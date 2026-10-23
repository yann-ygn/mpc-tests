#!/usr/bin/env python3
"""xpj: read an MPC 3.x project file (.xpj).

The file is gzip-compressed text: a header of five lines (ACVS, the MPC version, SerialisableProjectData, json, the
platform) and then the project as JSON. (MPC 2 projects are XML instead, and aren't read here.)

    xpj.py summary FILE.xpj         the project's tracks and sequences, and what is selected
    xpj.py diff OLD.xpj NEW.xpj     every value that differs between two saves
    xpj.py json FILE.xpj            the JSON, indented

Where things are (MPC 3.9.1.2, project version 28; read from controlled saves, see findings/):
    tracks[i]                          the user's tracks first, then the buses (program.type 7 return, 8 submix,
                                       9 outputs); name, colour (24-bit RGB), recordArm, volume, pan
    tracks[i].program.type             0 drum, 3 plugin, 6 audio (others not seen yet)
    tracks[i].program.mixable.mute     the track's mute (Track Mute mode, the Main screen's MUTE)
    tracks[i].program.mixable.solo     the track's solo
    sequences[j].value                 name, bpm, lengthBars, loopStartBar, loopEndBar, loop
    sequences[j].value.trackClipMaps[0] one entry per track, keyed by the track's name: perClipParameterValues with
                                       key {parameter 256, instrumentIndex 257} is the mute in that sequence
                                       (trackMutePerSequence)
    currentTrackIndex, currentSequence, engineMode (the mode on screen: Browser, Track Mute ...)
"""
import json
import sys
import zlib

BUSES = {7: "return", 8: "submix", 9: "outputs"}
KINDS = {0: "drum", 3: "plugin", 6: "audio", **BUSES}


def load(path):
    raw = open(path, "rb").read()
    text = zlib.decompressobj(31).decompress(raw).decode("utf-8")
    header, body = text[:text.index("{")], text[text.index("{"):]
    return header.split(), json.loads(body)["data"]


def seq_mutes(seq):
    """Track name -> mute in this sequence."""
    out = {}
    for clip in seq.get("trackClipMaps", [[]])[0]:
        params = clip.get("value", {}).get("perClipParameterValues", {}).get("perClipParameterValues", [])
        for p in params:
            if p.get("key") == {"parameter": 256, "instrumentIndex": 257}:
                out[clip.get("key")] = p.get("value", 0) >= 0.5
    return out


def summary(path):
    header, d = load(path)
    seqs = [s["value"] for s in d["sequences"]]
    cur_seq = d.get("currentSequence", 0)
    print(f"{path}: {' '.join(header[:2])}, project version {d.get('version')}")
    print(f"selected: sequence {cur_seq + 1}, track {d.get('currentTrackIndex', 0) + 1}; mode {d.get('engineMode')}; "
          f"key {d.get('key')}; master tempo {'on' if d.get('masterTempoEnabled') else 'off'} ({d.get('masterTempo')})")
    print("sequences:")
    for j, s in enumerate(seqs):
        print(f"  {j + 1:2} {s['name']:<32} {s['bpm']:7.2f} BPM {s['lengthBars']:4} bars"
              f"  loop {'on' if s.get('loop') else 'off'} {s.get('loopStartBar', 0) + 1}-{s.get('loopEndBar', 0)}")
    mutes = seq_mutes(seqs[cur_seq]) if seqs else {}
    print(f"tracks (mute in sequence {cur_seq + 1} under 'seq'):")
    for i, t in enumerate(d["tracks"]):
        p = t["program"]
        if p.get("type") in BUSES:
            continue
        mix = p.get("mixable", {})
        flags = [f for f, on in (("mute", mix.get("mute")), ("solo", mix.get("solo")), ("seq-mute", mutes.get(t["name"])),
                                 ("arm", t.get("recordArm"))) if on]
        print(f"  {i + 1:2} {t['name']:<24} {KINDS.get(p.get('type'), 'type ' + str(p.get('type'))):<7} "
              f"#{t.get('colour', 0):06X}  vol {t.get('volume', 0):.2f} pan {t.get('pan', 0):.2f}  {' '.join(flags)}")


def flat(o, p="", out=None):
    out = {} if out is None else out
    if isinstance(o, dict):
        for k, v in o.items():
            flat(v, f"{p}/{k}", out)
    elif isinstance(o, list):
        for i, v in enumerate(o):
            flat(v, f"{p}[{i}]", out)
    else:
        out[p] = o
    return out


def main(argv):
    if len(argv) >= 3 and argv[1] == "summary":
        summary(argv[2])
    elif len(argv) >= 4 and argv[1] == "diff":
        a, b = flat(load(argv[2])[1]), flat(load(argv[3])[1])
        for k in sorted(set(a) | set(b)):
            if a.get(k) != b.get(k):
                print(f"{k}: {json.dumps(a.get(k))} -> {json.dumps(b.get(k))}")
    elif len(argv) >= 3 and argv[1] == "json":
        print(json.dumps(load(argv[2])[1], indent=1))
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main(sys.argv)
